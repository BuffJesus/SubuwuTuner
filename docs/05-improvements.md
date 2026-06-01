# 05 — What Makes SubuwuTuner Different

SubuwuTuner is a comprehensive Subaru tuning suite designed to be the tool the community would build for itself if it started from scratch today. The improvements below are the places where being a native, modern C++ codebase, open from the start, gives us a structural edge over existing tools.

## 1. Native startup, memory, and binary size

JVM-based tuning tools pay a real cost in cold-start time and idle RAM. Targets for SubuwuTuner v1.0:

| Metric | SubuwuTuner target |
|---|---|
| Cold start to interactive window | < 800 ms |
| Idle RAM after opening a project | < 150 MB |
| Installer size | < 60 MB |

## 2. Headless / scriptable mode

`subuwutuner-cli` shares the same domain model as the GUI: open a project, dump a map to CSV, batch-apply a patch across N ROMs, run a flash from a recipe file. This unlocks CI for tune authors, fleet workflows for shops, and reproducible pipelines for dyno operators — none of which the existing GUI-only Subaru tuning tools support.

## 3. Definition format as source-of-truth, version-controlled

`*.stune` projects are git-friendly. Definitions are TOML. Tune development becomes diffable, mergeable, reviewable — bring tune-shop workflows into the same world the rest of software lives in.

## 4. Brick protection — formalized, testable

Brick protection is a first-class subsystem with a written threat model:

- Recovery shim is installed and **verified by reading it back** before any subsequent write
- Recovery strategy is **ISA-family specific** — see below
- Every flash operation publishes a tamper-evident manifest (currently CRC32 over sector hashes; BLAKE3 upgrade is staged for after the bench rig validates it — see `docs/03` tech-stack §Hashing) that the user can keep
- If the host machine dies mid-flash, the next boot of SubuwuTuner offers to resume from the manifest

### 4a. Recovery strategy by ISA family

The "separate flash bank or bootloader-guaranteed region" guarantee depends on the silicon. The two families we ship against need separate recovery designs.

| ISA family | Parts | Bank layout | Recovery strategy |
|---|---|---|---|
| SH-2A (VA WRX) | SH7055 / SH7058 / SH7059 | **Single bank** | Minimize the bad-vector window (program → readback-verify → flip boot vector last). Rely on the **ECU's hardware serial boot mode** (mode pin + reset) as the actual brick exit; an in-flash shim cannot live where it would need to during recovery. |
| RH850 (VB WRX) | RH850 / F1x | **Dual bank** (FA-DIT) | Recovery shim in the secondary bank; primary-bank corruption recovers from the shim on the next boot without external intervention. The boot vector flip is still last. |

The bench rig validates **the actual recovery path per family**, not a generic "we wrote a shim" check:

- **SH parts:** intentionally brick → trigger serial boot mode → recover via host tool → confirm. Document whether the mode pin is reachable via the OBD-II connector or requires opening the ECU case (the difference between "recoverable on the side of the road" and "shop-only recovery").
- **RH850 parts:** intentionally brick the primary bank → cold-boot → confirm secondary-bank shim takes over → re-flash primary → confirm normal boot resumes.

Single-bank ≠ dual-bank; treating them as one design is how tuners brick cars they thought were protected. Per-family recovery recipes live in `docs/31-brick-protection-by-isa.md` — concrete FCU register map, sector allow-list, deliberate-brick + serial-boot recovery procedure for SH-2A; dual-bank atomic-swap design for RH850 with open bench-rig items called out. Tracked as a v1.0 ship blocker per `04-roadmap.md`.

**Facts staged for the recipes (2026-05-24).** Analyst-mode RE of the plaintext A-series and B-series ROMs has produced:

- `fixtures/private/findings_flash_region_map/` — per-CID Bootloader / Calibration / EEPROM / RAM-mirror / IO ranges for all 24 CIDs (2015–2026).
- `fixtures/private/findings_algorithms/checksum-recompute.md` — the sum-of-16-bit-shorts-to-`0x5AA5` invariant, magic `0x55 0x55` at offset 0, A-series BE / B-series LE; verified across all 24 plaintext ROMs.
- `fixtures/private/findings_decrypted_inventory/INVENTORY.md` — FULL / MIXED / PARTIAL decryption status per family; the FULL set is what the bench-rig recovery harness can validate against without hardware. Only those are staged under `fixtures/private/roms_plaintext_by_cid/`.
- `fixtures/private/findings_algorithms/` `brick-protection.md` companion: SH-2A 2 MB FCU register MMIO (`0xFFFFE800..0xFFFFE873` primary, `0xFFFFEC00..0xFFFFEC4F` extended), 41 distinct FCU register accesses found in the bootloader, sector-erase allow-list excludes bootloader sectors `0x00..0x0F`, reset PC `0x000000E8`. These are concrete anchors for the SH-2A recovery recipe.

The Tier 4 HIL plan can now reference these as a known-good baseline when junkyard ECUs come online.

The whole subsystem is bench-tested on real ECUs as part of CI — junkyard ECUs on a bench harness, automated. See `08-testing-strategy.md`.

### 4b. Cancellation invariants

`std::stop_token` is the cancellation primitive across the orchestrator, but **mid-PDU cancellation is how you brick the ECU.** The invariants the flasher honors (and `tests/unit/flash/` enforces):

- Cancellation **never** aborts an in-flight UDS service. Once a `RequestDownload` or `TransferData` is on the wire, the orchestrator finishes that PDU and then queues a clean `RequestTransferExit` + session exit. Cancel arrives between PDUs, not within one.
- Same posture for SSM block writes — "finish this 256-byte block, then stop," never "drop the link."
- The "ECU is in programming session" state is **persistent across host-side crashes.** On the next launch, `Project::open()` checks the journal in the project directory; if a flash was in flight, the GUI/CLI offers either (a) resume from the manifest (`Flasher::plan_resume`), or (b) cleanly exit the session via UDS DSC = `defaultSession`.

These invariants are testable without hardware via `MockTransport` + `FaultInjector`. See `docs/08-testing-strategy.md` Tier 2 for the test plan.

## 5. Real-time datalogging quality

- Sustained 100 Hz on supported adapters
- Lock-free sample ring buffer; UI samples a snapshot, never blocks the I/O thread
- Per-PID timestamps from the adapter (where available), not wall-clock interpolation
- Replay format is CSV (`st::log::CsvSink`) — text-greppable, drops straight into the autotune CLI without a separate parser

## 6. Plugin safety

A community-shared `.stmod` is a TOML document — `[graph]` + `[[node]]` + `[[edge]]` for the source graph plus an optional pre-compiled `[patch]` table. The host-side path is parse → `feature::ir::lower` → `feature::codegen::compile`; there is no arbitrary code execution surface, no `io` / `os` / FFI exposure, because the IR is a typed dataflow SSA — not a scripting runtime. A malicious shared pack cannot exfiltrate project files or write outside the editor's review flow. Any compiled patch is rendered as bytes in `feature-compile`'s preview before it ever touches a ROM.

## 6a. First-class, open auto-tune

The Subaru community's options for auto-tune are dated, GUI-only, or paid-and-closed. SubuwuTuner ships first-party MAF auto-tune and knock-based ignition pull in v1.1, with closed-loop trim integration, boost auto-trim, and idle trim in v1.2. CLI-first so a driver can run `subuwutuner-cli autotune maf --log run.csv --project mytune.stune` between drives, or so a dyno operator can batch-process a day's worth of logs. Engine-safety linting runs over every proposal regardless of jurisdiction profile. See `docs/12-auto-tuning.md`.

## 6b. Jurisdiction profiles instead of paternalism

The user picks a configurable **jurisdiction profile** on first run (Alberta-CA, California-US, EU-roadworthy, motorsport-only, etc.). The default is `motorsport-only` — least restrictive. Each profile decides whether emissions-flagged edits draw a silent badge, a warning, or a confirmation. Engine-safety warnings (AFR way off, dangerous timing) stay blocking in every profile. See `06-legal-ethics.md`.

## 7. Better gauges, fewer dependencies

A first-party 2D gauge widget set built on ImGui's `DrawList` (direct triangles + lines, no widget overhead), plus **ImPlot** for log plotting and live datalog charts — same author as ImGui, 100k-point real-time series without dropping frames. Goal: 60 fps on integrated graphics. No WebGL, no embedded browsers.

## 8. Cross-platform on day one

We ship CI-tested binaries for **Windows x64, macOS arm64+x64 universal, Linux x64, Linux arm64** from day one, with the same feature set. No second-class platforms.

## 9. Native adapter drivers with proper backpressure

Adapter support (Tactrix, OBDLink, ELM327, OBDX Pro) is built on a coroutine-based AT-command framework where every command has a typed response, a timeout, and a retry policy declared in code — easier to extend with new adapter quirks than a hand-rolled state machine.

## 10. Telemetry posture

**Zero analytics, zero phone-home.** Crash reporting is opt-in, contains no project content, and is sent to a self-hostable endpoint.

## 11. Surfacing under-served definition coverage

The RomRaider XMLs and the packs we ship from them already expose a long tail of maps and parameters that the existing tools *technically support* but for which **no community workflow exists**. Tuners ignore them because the UI doesn't make them legible, the methodology isn't written down, or both. SubuwuTuner uses this as a deliberate differentiator: ship the visualization and the workflow, not just the editor cell.

The under-served categories below are present in nearly every per-CID RR pack we generate but rarely touched in published tunes:

| Category | What's in the def | Why it's neglected |
|---|---|---|
| Cold-start fuel + spark | Coolant-temp-conditioned tables active first 60–120 s | Dyno tuners only tune warm; no community methodology |
| Adaptive-learning state arrays | LTFT history, idle-adapt, knock-learn DAM history | Read-only views; no UI surfaces drift over time |
| Per-cylinder knock thresholds | Cyl-1..4 separate noise-floor maps | Community workflow collapses to one global value |
| Cruise-control PID gains | Throttle PID for set-speed hold | Treated as "comfort," not "performance" |
| Cooling-fan hysteresis curves | Fan-on/off temp curves | Edited only for race-car overheat fixes |
| EBCS / WGDC controller gains | Boost-solenoid PID (not just duty/target) | Tuners adjust target/duty, ignore controller loop |
| VVT actuator gains | Intake/exhaust cam PID coefficients | Tuners adjust cam targets, ignore the loop |
| CACT compensation | Charge-air-cooler temp pull/enrichment | Logged raw, never overlaid on its own comp map |
| MAF compensation by RPM band | Per-RPM MAF-flow trim | Subsumed into "MAF scaling," loses RPM-axis structure |
| AFR target outside peak load | Light-throttle / part-load AFR rows | Tuners hammer WOT row, leave the rest OEM |
| Cat over-temp protection thresholds | EGT-conditioned fuel enrichment | Never touched — but it's the thing preventing meltdown |
| Knock-window crank-angle map | Sensor sampling window per cylinder | RR-exposed, never adjusted in any published tune |
| Boost-by-gear taper | Gear-conditioned target boost | Partially tracked (1st-gear cut), 4th+ usually flat-OEM |

### v1.x feature plays this enables

Four concrete features fall out directly. None require new hardware; all run against existing log + ROM data.

1. **Adaptive-learning history visualizer** — chart LTFT / DAM / idle-adapt drift over weeks. The state arrays are already in the def; the missing piece is a long-cycle UI on top. No IP risk, pure infrastructure win, big diagnostic value (catches dropping injectors, MAF aging, vacuum-leak drift). Implementation: `src/log/{include/st/log/adaptive_history.hpp,src/adaptive_history.cpp}` (typed snapshot + bucketing + least-squares drift slope), `tests/unit/log/test_adaptive_history.cpp` (12 cases), `subuwutuner-cli adaptive-history` (summary table + `--verbose` per-bucket detail), and a GUI panel in `subuwutuner-gui` under **View → Adaptive history (preview)** (summary table + ImPlot time-series chart per signal). Try it without bringing your own log:

   ```
   subuwutuner-cli adaptive-history --log fixtures/demo-adaptive-history.csv \
       --timestamp-col ts --ltft-col ltft --dam-col dam --idle-adapt-col iac
   ```

   The shipped fixture (`fixtures/demo-adaptive-history.csv`) is a 32-day synthetic where LTFT slowly trends to -4% (injector fouling), DAM stays stable, and IAC adapt rises (idle vacuum leak). Implementation contrasts with the knock dashboard on time-scale: knock windows ~10 s of samples within one log; this one buckets samples across days/weeks (default 1-day buckets) and computes a least-squares drift slope per signal — that slope is the "is something getting worse over time?" indicator.
2. **Per-cylinder knock dashboard** — split the cyl-1..4 noise tables into a real cylinder-comparison view with log overlay. Catches uneven fueling, weak coil-pack, knock-sensor placement issues that single-value views hide. Cylinder-count-aware: H4 (FA / EJ) gets a 2×2 grid, H6 (EZ30 / EZ36) gets 3×2; EG33 (SVX) is supported in degraded mode (no per-cyl FLKC in that firmware era — single-channel knock surface). Implementation: `src/log/{include/st/log/knock_dashboard.hpp,src/knock_dashboard.cpp}` (pure-domain types + windowed aggregator + CSV reader), `tests/unit/log/test_knock_dashboard.cpp` (10 cases), `subuwutuner-cli knock-snapshot` (text-mode dashboard), and a GUI panel in `subuwutuner-gui` under **View → Knock dashboard (preview)**. Try it without bringing your own log:

   ```
   subuwutuner-cli knock-snapshot --log fixtures/demo-knock-log.csv \
       --rpm-col rpm --load-col load \
       --flkc-cols flkc1,flkc2,flkc3,flkc4 \
       --fbkc-cols fbkc1,fbkc2,fbkc3,fbkc4 \
       --sample-rate-hz 5 --window-seconds 60
   ```

   The shipped fixture (`fixtures/demo-knock-log.csv`) is a synthetic 3rd-gear pull where cyl 1 picks up persistent knock retard and cyls 3-4 stay clean — the output should clearly flag cyl 1 as the outlier.
3. **Cold-start tuning workflow** — define a methodology (target lambda by ECT, recommended timing pull by ambient) and a GUI mode that gates the cold-start tables behind a checklist. Atlas exposes the maps; nobody ships a workflow around them. Implementation: `src/log/{include/st/log/coldstart.hpp,src/coldstart.cpp}` (phase classifier PreCrank / Cranking / InitialFiring / HighIdle / Warmup / ClosedLoop + ECT-binned aggregation + compliance vs a user-defined `TargetLambdaCurve`), `tests/unit/log/test_coldstart.cpp` (15 cases), `subuwutuner-cli coldstart-analyze` (phase table + ECT-bin table + deviation report), and a GUI panel under **View → Cold-start analysis (preview)** (phase summary + observed-vs-target ImPlot + sortable ECT-bin table). Try it without bringing your own log:

   ```
   subuwutuner-cli coldstart-analyze --log fixtures/demo-coldstart-log.csv \
       --timestamp-col ts --ect-col ect --iat-col iat --rpm-col rpm \
       --observed-lambda-col obs --commanded-lambda-col cmd \
       --target "0:0.82,20:0.90,40:0.95,55:1.00" --min-samples-per-bin 1
   ```

   The shipped fixture (`fixtures/demo-coldstart-log.csv`) is a synthetic 30-second WRX cold-start (5 °C ambient, key-on → drive to operating temp) so the panel has something to chart on first launch.
4. **Boost-controller PID assistant** — fit the EBCS PID gains from a tip-in log. The table exists in every WRX def; the fitting methodology is absent from the community. Closes a real long-standing complaint (boost overshoot / undershoot on tip-in). Implementation: `src/log/{include/st/log/ebcs.hpp,src/ebcs.cpp}` (tip-in detector + step-response characterization: rise time, overshoot, settling time, steady-state error + heuristic suggestions), `tests/unit/log/test_ebcs.cpp` (9 cases), `subuwutuner-cli ebcs-analyze` (summary + suggestions + `--verbose` per-event detail), and a GUI panel under **View → EBCS PID assistant (preview)** (metrics + suggestion list + sortable event table). Output is advisory — verify on a dyno. Try it without bringing your own log:

   ```
   subuwutuner-cli ebcs-analyze --log fixtures/demo-ebcs-log.csv \
       --timestamp-col ts --target-boost-col target_boost \
       --actual-boost-col actual_boost --throttle-col throttle \
       --wgdc-col wgdc --rpm-col rpm
   ```

   The shipped fixture (`fixtures/demo-ebcs-log.csv`) is a 5-second 3rd-gear synthetic log with two tip-ins (first overshoots ~20%, second clean) so the panel has both quality classes to display.

Plays 1 and 2 are pure visualization (low risk, ship in OSS). Plays 3 and 4 are tuning-domain features that share infrastructure with the auto-tune kernels in `docs/12`.

### Closing the loop: from suggestion to edit

The four plays currently surface metrics + advisory suggestions. The natural next step is letting users *act* on those suggestions through the existing `st::edit::History` system so the edits get full undo/redo + project-history journaling. This is forward-looking; deliberately not landed in v1.0 because of one structural blocker.

**The blocker: pack-format extension for table roles.** Each panel knows what KIND of table its suggestion targets (cold-start → "Open Loop Fueling Enrichment vs ECT"; EBCS → "Wastegate PID Kp/Ki/Kd"; knock → "Per-Cylinder Knock Noise") but a generic pack has no machine-readable role field on its `[[table]]` entries. Today a user has to cross-reference a suggestion against table names manually.

The fix: extend the pack format with an optional `[[table.role]]` field tagging known tuning roles. The schema would look something like:

```toml
[[table]]
id   = "ol_fuel_enrichment_vs_ect"
name = "Open Loop Fueling Enrichment"
address = 0x...
role = "coldstart.open_loop_fuel_vs_ect"   # canonical role string
```

Role strings are defined in a small enum in `st::defs` (so the GUI / autotune / §11 panels can map them deterministically) and populated by `tools/defgen/` against a small known-mapping table per platform. Existing packs without `role` fields keep working — the suggestion-to-edit affordance just stays inactive on those tables.

**Once that lands, the per-panel "Apply suggestion" path becomes:**

1. Panel computes its `DriftDiagnosis` / cold-start lambda deviation / EBCS gain recommendation
2. User clicks "Apply suggestion as edit" on a specific cell
3. The panel queries `Definition::find_table_by_role(role_string)` for the user's loaded pack
4. Constructs a `ByteEdit` against that table at the appropriate cell
5. Routes the edit through `edit::History::apply()` — same path as a manual GUI edit, full undo/redo
6. Engine-safety policy linter runs against the proposed bytes (same posture as any other edit; see `docs/06` and `docs/19` for the policy gate)

**Until that lands**, the §11 panels surface their suggestions as text and link the user to the relevant table category — they can find the table in the sidebar and edit it manually. The path through `edit::History` still applies to whatever they type in.

Roadmap placement: **schema bump pulled into v1.0**, panel wires land incrementally. Adding an optional `role` field to the `[[table]]` schema, the role-string enum in `st::defs`, and `Definition::find_table_by_role()` is a single PR — it gates nothing on auto-tune work and unblocks all four §11 panels' suggestion-to-edit affordance as soon as `tools/defgen/` (or contributor PRs) populate role mappings per platform. The closed-loop trim integration in `docs/12` still lands in v1.2; the schema does not need to wait for it.

### FA-DIT logger XML supplement

All four plays depend on extended SSM PIDs whose RAM addresses are firmware-specific. RomRaider's v370 logger XML (Nov 2021) is the latest public release and predates FA-DIT VA/VB WRX coverage entirely (see `fixtures/private/PAK_DECODE_RESULTS.md`). The SubuwuTuner-native fix is `tools/defgen/data/logger_supplement_fadit.xml` — a small additive logger XML in the RomRaider DTD shape that `loggergen.py` already consumes. Initially empty (except for a stub entry that proves the round-trip), it grows by contribution as community members capture real RAM addresses from hardware. Provenance rules and clean-room boundaries are baked into the file's header comment per `docs/15-clean-room-engineering.md`.

---

## Where we are conservative

- **Vehicle coverage at v1.0.** VA + VB WRX MT only because that's what we can brick-test on the Phase-4 bench rig. Architecture is multi-platform from day one (see `02-architecture.md`); expansion order is in `04-roadmap.md`.
- **No defeat-preset content shipped first-party.** The tool exposes what the ECU exposes; we don't ship pre-built "delete" calibrations.
- **No write path on unsupported adapters.** Flashing only goes over verified, documented J2534-class paths until proven safe elsewhere.
