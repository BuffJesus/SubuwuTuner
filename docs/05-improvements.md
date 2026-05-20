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
- Recovery shim sits in a separate flash bank or in a region the bootloader is guaranteed to read
- Every flash operation publishes a tamper-evident manifest (BLAKE3 over sector hashes) that the user can keep
- If the host machine dies mid-flash, the next boot of SubuwuTuner offers to resume from the manifest

The whole subsystem is bench-tested on real ECUs as part of CI — junkyard ECUs on a bench harness, automated. See `08-testing-strategy.md`.

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
3. **Cold-start tuning workflow** — define a methodology (target lambda by ECT, recommended timing pull by ambient) and a GUI mode that gates the cold-start tables behind a checklist. Atlas exposes the maps; nobody ships a workflow around them. Domain scaffold at `src/log/include/st/log/coldstart.hpp` — phase classifier (PreCrank / Cranking / InitialFiring / HighIdle / Warmup / ClosedLoop) + ECT-binned aggregation + compliance vs a user-defined `TargetLambdaCurve`. UI on top sequences the user through "what to log next" and surfaces table-edit proposals from the binned observations.
4. **Boost-controller PID assistant** — fit the EBCS PID gains from a tip-in log. The table exists in every WRX def; the fitting methodology is absent from the community. Closes a real long-standing complaint (boost overshoot / undershoot on tip-in).

Plays 1 and 2 are pure visualization (low risk, ship in OSS). Plays 3 and 4 are tuning-domain features that share infrastructure with the auto-tune kernels in `docs/12`.

### FA-DIT logger XML supplement

All four plays depend on extended SSM PIDs whose RAM addresses are firmware-specific. RomRaider's v370 logger XML (Nov 2021) is the latest public release and predates FA-DIT VA/VB WRX coverage entirely (see `fixtures/private/PAK_DECODE_RESULTS.md`). The SubuwuTuner-native fix is `tools/defgen/data/logger_supplement_fadit.xml` — a small additive logger XML in the RomRaider DTD shape that `loggergen.py` already consumes. Initially empty (except for a stub entry that proves the round-trip), it grows by contribution as community members capture real RAM addresses from hardware. Provenance rules and clean-room boundaries are baked into the file's header comment per `docs/15-clean-room-engineering.md`.

---

## Where we are conservative

- **Vehicle coverage at v1.0.** VA + VB WRX MT only because that's what we can brick-test on the Phase-4 bench rig. Architecture is multi-platform from day one (see `02-architecture.md`); expansion order is in `04-roadmap.md`.
- **No defeat-preset content shipped first-party.** The tool exposes what the ECU exposes; we don't ship pre-built "delete" calibrations.
- **No write path on unsupported adapters.** Flashing only goes over verified, documented J2534-class paths until proven safe elsewhere.
