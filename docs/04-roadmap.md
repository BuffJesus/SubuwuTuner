# 04 — Roadmap

Six phases. Each ends with a demoable artifact and a "ship gate" that must pass before the next phase starts. Time estimates assume one to two developers part-time.

**Status legend** (per phase / per bullet):
✅ shipped · 🟡 partially shipped · ⬜ not yet · 🔒 blocked on hardware

## Phase 0 — Foundations (2–3 weeks) ✅ done

- ✅ Repo skeleton, CMake presets, FetchContent deps, CI on Win/Mac/Linux (vcpkg manifest mode deferred — `docs/07`)
- ✅ `st::core` (`Result`, `Error`, units, `Span`)
- ✅ Catch2 wired up, **1100+ tests green** (was sized at 50 originally)
- ✅ `subuwutuner-cli` binary with version + 60+ subcommands

**Gate:** green CI on all three OSes; binary runs. ✅

## Phase 1 — ROM viewer, no hardware (4–6 weeks) ✅ done hardware-free

- ✅ `st::rom` reads raw binary dumps, computes CRC32 / sector hashes
- ✅ TOML definition parser (`st::defs`) with directory + single-file modes, inheritance via `extends`, validation
- ✅ `tools/defgen` Python pipeline + per-platform mapping YAMLs
- ✅ Read-only 2D and 3D table render (Grid + Heatmap views) in a Dear ImGui window
- ✅ CLI `dump-table` / `dump-axis` / `rom-info` / `rom-diff` for headless inspection

**Gate:** 🔒 hardware-blocked. Code works end-to-end against synthetic packs + fixtures; the "≥ 20 maps from real definitions on real ROM" verification needs a real ROM dump (drop in `fixtures/private/` per scaffolding at `0a5e0fc`).

## Phase 2 — Editing & projects (3–4 weeks) ✅ done

- ✅ Undo/redo stack via `st::edit::History` (per-action ByteEdits; DTC toggles also undoable as of `0ad6d13`)
- ✅ Table editor with selection, paste, smooth, interpolate, percent-scale, bulk CSV import + export
- ✅ `.stune` project save/load (directory-backed, not zip — simpler diffability)
- ✅ Diff view between two ROMs (`rom-diff` CLI + `project-diff`)
- ✅ Jurisdiction profile gate (`st::policy`) at edit time per `06-legal-ethics.md`
- ✅ DTC bitmap toggles via `[[dtc_bitmap]]` schema (CLI `project-disable-dtc` / `project-enable-dtc`)

**Gate:** ✅ done — `project-new` → `project-edit` → save → reopen round-trips work; covered by integration tests.

## Phase 3 — Comms & datalogging (5–7 weeks) ✅ done hardware-free / 🔒 live validation hardware-gated

Architecture, protocol catalog, and datalogger pipeline are complete end-to-end via MockTransport. Platform USB / DLL layer + real-hardware validation gate on adapter arrival; OBDX Pro VX is now in hand (2026-05-24), so the gate is actionable.

- ✅ `st::transport::ITransport` interface + `MockTransport`
- ✅ J2534 v04.04 ABI types + status decoder + `J2534Library` wrapper (`44896d4`)
- ✅ `j2534::Transport` ITransport impl on top of the function table (`2f0d054`)
- ✅ J2534 adapter discovery via Windows registry walk (`666dd33`) + CLI `transport-list`
- ✅ OBDX DVI codec (`265e692`) + `obdx::Transport` (`70b2af4`) with ELM→DVI handshake
- ✅ Native protocol codec (`ee09837`) + `native::Transport` (`059ac9b`) for our own doc-18 handheld when PC-tethered
- ✅ `IByteChannel` shared abstraction for USB CDC byte-stream transports
- ✅ `open_transport()` factory + CLI `rom-pull --transport j2534|obdx|native` flag (`3f16b2e`)
- ✅ `st::ecu::ssm` (SSM K-line + CAN) + `st::ecu::uds` clients (adapter-agnostic; consume `ITransport`)
- 🔒 Platform USB CDC `IByteChannel` impl (libusb on Win/Linux, native CDC on macOS) — bench-blocked
- 🔒 Platform DLL dynamic-load for `J2534Library::load()` (`LoadLibraryA` / `dlopen`) — adapter-blocked
- ⬜ ELM327 backend over serial — deferred (low-priority; ELM is read-only by policy and the existing adapters cover the v1.0 surface)
- ⬜ Streaming on concrete Transports — `start_streaming` / `stop_streaming` stubbed `NotImplemented` on all three. Lands with the datalogger I/O thread + ring buffer.
- ⬜ Live gauge cluster (4–8 gauges) + CSV log export — design exists in `docs/13`; impl waits on streaming
- ⬜ Sustained 50 Hz logging across 20 PIDs — same

**Gate:** 🔒 hardware-blocked. Code paths are real + unit-tested (108 transport tests across the trio + factory + discovery). When OBDX Pro VX adapter arrives, the path to first ROM dump is: implement one platform `IByteChannel` (libusb on Windows ~1 file), wire it into `obdx::Transport`, run `subuwutuner-cli rom-pull --transport obdx --device <COM>`.

## Phase 4 — Flashing (6–10 weeks, the dangerous one) ✅ done hardware-free / 🔒 live validation hardware-gated

Orchestrator + Manifest + journal + policy gate + checksum-type field all exist and are end-to-end test-covered; SecurityAccess is fully recovered for the A-series (factory + aftermarket variants in tree). Vendor-specific checksum repair + brick recovery need hardware to validate against.

- ✅ `Flasher::read_full_rom` via `ITransport` + UDS RequestDownload/TransferData (mock-trace exercised)
- ✅ FlashPlan + sector model (`st::flash`)
- ✅ Manifest + journal + `plan_resume` for crash-safe writes
- ✅ Policy + mutation gate (`docs/06` + `st::policy`)
- ✅ `checksum_type` field in pack `[pack]` table (added `58a821f`); enum mirrors the `<checksum module="…">` strings in RomRaider's public definition XML (`ChecksumSTD` / `ALT` / `ALT2` …)
- ✅ `IChecksumRepair` seam + `make_checksum_repair` factory + `apply_checksum_repair(span, Definition)` wrapper + CLI `checksum-verify` / `checksum-repair` exit-3-on-NotImplemented (every concrete kind still returns NotImplemented; algorithm impls wait on a known-good stock dump for byte validation)
- ✅ **Seed/key authentication (SecurityAccess)** — A-series SSMCAN1 fully recovered: factory 16-round Feistel (`ssmcan1_key_stub`), COBB-AP L1 + L3 variants (`ssmcan1_l{1,3}_cobb_ap`), Fehr-active aliases. CLI-selectable via `--sa-variant`. See `docs/23-security-access.md` for the full catalog.
- ✅ **Optional gated 0xB6 bulk-transfer write path** — compile-flagged + runtime-flagged. See `docs/26-bulk-reflash-cipher.md`.
- ⬜ Checksum-repair implementations (subaru_std, subaru_alt, subaru_alt2) — seam ready; algorithms still need byte-validation against a known stock dump
- 🔒 Brick protection bootstrap + recovery shim — bench rig prerequisite
- 🔒 Delta-only flashing + dry-run mode — same
- ⬜ Patch insertion layer (`src/feature_patch/`) — for Phase 5 features; needs real ECU vector tables

**Gate:** 🔒 100 successful flash cycles on a junkyard ECU bench rig — zero bricks, zero corrupted images. **No customer ever flashes a car until this gate is met.** Hardware-blocked.

## Phase 5 — Custom features (4–6 weeks) 🟡 design + IR + SH-2A codegen for VA shipped; RH850 codegen for VB partial; patch insertion remaining

Heavy progress — what was sized at 4–6 weeks for the editor + IR + one backend shipped much faster. RH850 codegen for VB is at SH-2A parity: all 3 IR shapes wired, 22-primitive CallPrimitive slice covering int arithmetic, int compares, bool logic, branchless select, float arithmetic, float unary (`sqrt_float`, `flex_fuel_scale`), float compares, and nested CallPrimitive operands (intermediate RAM slot per non-root primitive). Patch-insertion layer + end-to-end flashing gate on Phase 4 hardware validation; the float-compare path also has an unverified TRFSR-direction assumption the bench rig settles. See `docs/16-custom-features.md` for the granular current-state matrix.

- ✅ Visual node-graph editor with pin labels + per-pin defaults + wire dragging + pin-context menus; promoted out of `View → Debug` to top-level `View → Custom features designer (preview)` (`0cc5c1b`)
- ✅ Graph data model (`st::feature::Graph`) + IR lowerer (`st::feature::ir::Module`) + graph-level + IR-level linters
- ✅ SH-2A codegen: Int arithmetic (add/sub/mul/`divide_int` via FPU bridge), Int compares, Bool ops, select (int/bool/float), Float arithmetic via FPU (FADD/FSUB/FMUL/FDIV), `sqrt_float`, Float compares (FCMP/EQ/GT), `flex_fuel_scale` (E0=1.00 → E85=1.28 hardcoded linear curve), cross-hook value flow, fan-out dedup. **22 primitives recognized.**
- 🟡 General curve / table-lookup primitives — `flex_fuel_scale` lands as a 1-arity hardcoded curve; an N-point lookup primitive with pack-supplied breakpoints + values still pending
- 🟢 RH850 codegen for VB — all 3 IR shapes wired; CallPrimitive slice covers 22 leaf-operand primitives at parity with SH-2A (int arithmetic, int compares, bool logic, branchless select, float arithmetic, `sqrt_float`, `flex_fuel_scale`, float compares) plus nested CallPrimitive operands via the same topological-walk + intermediate-RAM-slot pattern as SH-2A. Bench-rig HIL still gates first real-ECU patch (float-compare TRFSR-direction is the open verification item)
- ✅ `[[hook]]` + `[[primitive]]` schema in def packs with `name` (codegen-canonical) + `label` (display) split per pin
- ✅ Linter: type-checks via Graph::connect / IR lowerer; RT-budget runs against per-primitive cycle costs derived from public SH-2A spec + FPU latencies (e.g. `divide_int` = 18, `add_int` = 1). Bench profiling will refine.
- ✅ `.stmod` format = TOML `[graph]` + `[patch]` halves, single file, round-trippable via `feature::from_toml` + `feature_codegen::patch_from_toml` (`92daa48`)
- ✅ CLI `feature-compile <stmod> --def <pack> [--arch sh2a|rh850] [--format hex|toml|raw|stmod] [--output <file>] [--validate-only]` (`87a59f5`, `--format stmod` `1406aa5`, `--validate-only` `1f7aafe`)
- ✅ Sample packs: `clutch-kill`, `flat-foot-shift`, `launch-control` compile end-to-end through SH-2A → real SH-2A bytes. `flex-fuel` blocks on the curve primitive.
- 🔒 Patch insertion layer (`src/feature_patch/`) — finds free RAM, writes the hook table, splices into existing vector tables. Bench-rig-blocked (needs a real ECU vector table to develop against).

See `docs/16-custom-features.md` for the full current-state matrix + primitive coverage table.

**Gate:** 🔒 community can publish + import a feature pack; samples ship in-box (✅ ship; ⬜ end-to-end flash gated on Phase 4 hardware + patch insertion).

## Phase 6 — Polish & 1.0 (ongoing) 🟡 in progress

- 🟡 Themed UI — Dark/Light themes shipped with purple accent (`accent_for(Theme)`); audit pass shipped 5 fixes (`0cc5c1b`, `4622bbe`, `9d6aabe`, `03f48cd`) covering modal feedback hygiene, status-msg TTL, discoverability hints, Revert-all confirm, plain-language lint findings
- ⬜ Accessibility pass (Tab nav in modals, screen-reader labels)
- ⬜ Installer / codesigning / auto-update channel — `st::updater` design sketched in `docs/22-auto-update.md` (channels, manifest, signature verification, swap mechanics); implementation tracks Phase 6 polish. Solo-dev iteration pain (zip-and-send between desktop and laptop) is solved meanwhile by file-sync, not by the in-tool updater.
- ⬜ Documentation site (Jekyll → MkDocs or similar)
- ⬜ Onboarding flow for first-time users — partial (welcome panel + new-project modal pack hints landed)
- ⬜ Telemetry **opt-in only**, crash-report-only, no analytics

### Pre-1.0 ship blockers (from the 2026-05 review pass)

These came out of the audit against `docs/05-improvements.md` and the external code review. Sequenced so the safety items land first.

1. 🟡 **Brick protection per-ISA** — recovery design split landed at `docs/31-brick-protection-by-isa.md` (SH-2A single-bank serial-boot path with concrete FCU register map + sector allow-list + deliberate-brick recipe; RH850 dual-bank atomic-swap design with open bench-rig items). Bench-rig validation per ISA (Tier 4 plan in the same doc) remains; that's the hardware-gated half. Top-level still tracked in `docs/05-improvements.md` §4a.
2. ✅ **Cancellation invariants enforced + tested** — see `docs/08-testing-strategy.md` Tier 2a. `Flasher::execute(plan, cancel)` polls the cancel flag at PDU boundaries (between sectors AND between TransferData blocks); on observed cancel it emits `RequestTransferExit` if a download is in flight then always emits `DiagnosticSessionControl(default)` as the final PDU. Tests in `tests/unit/flash/test_cancellation_invariants.cpp` cover all four UDS invariants (pre-set cancel, between-sectors cancel, mid-TransferData cancel, session-exit assertion + crash-mid-flash recovery + resume idempotence), plus `tests/unit/ecu/test_uds_pdu_atomicity.cpp` for the PDU-atomicity-by-construction guarantee the orchestrator-level deferral relies on. SSM mid-block cancellation is moot for v1.0 (VA/VB WRX run CAN-UDS, not K-Line SSM); lands with v1.3 EJ-era support.
3. ✅ **Codegen writable-region gate** — `st::feature::codegen::gate_patch` refuses to emit a `PatchObject` whose per-hook `[splice_address, splice_address + code.size())` range is not fully contained in one declared `[[writable_region]]` of the loaded `Definition` (schema landed in `docs/11-definition-format.md`). Fail-closed against packs with zero declared regions. Wired into `Sh2aBackend::compile()`'s exit path so callers can't bypass it. Coverage at `tests/unit/feature_codegen/test_address_gate.cpp` (15 tests). RAM allocations stay gated by per-hook `free_ram` + `RamAllocator` (unchanged). See `docs/16-custom-features.md` §Safety #6.
4. ✅ **`[[table.role]]` schema in v1.0** — optional `role` field on `[[table]]` entries (`std::optional<std::string>` in `st::Table`) + `Definition::find_table_by_role()` lookup. Schema bump only; per-panel wiring (§11 panels' suggestion-to-edit affordance — cold-start, EBCS, knock, adaptive history) lands incrementally as each panel adopts `find_table_by_role()`. Packs without a `role` field are unaffected — the suggestion-to-edit affordance simply stays inactive on those tables. Test coverage in `tests/unit/defs/test_defs.cpp`. See `docs/11-definition-format.md` and `docs/05-improvements.md` §11.
5. ✅ **`.stune` format spec** — pinned in `docs/21-stune-format.md` (directory layout, `project.toml` schema, `source.bin` / `working.bin` / `edits.toml` semantics, schema-version + forward-compat rules, git-friendliness recipes, programmatic-access example). Synced with the current `render_project_toml` / `render_history_toml` shape: `policy_profile` field documented; per-edit `history/*.toml` replaced with the actual single `edits.toml`; flash manifest noted as caller-controlled path, not auto-routed under the project dir.
6. ✅ **`subuwutuner-cli doctor`** — single subcommand: tool/build identity, J2534 adapter probe, definition-pack health check, ROM CID identification (with `--rom` + `--pack-dir`), traffic-light status per section + actionable "what to do next" hints. Exit 0 on healthy/advisory-only, 1 on any FAIL, 2 on bad CLI usage. Closes the install → defgen → open bootstrap cliff.
7. ⬜ **Frozen `defgen` binary** — PyInstaller / Nuitka build of `tools/defgen/` shipped in the installer, so end users do not need a Python environment.
8. ⬜ **README platform-feature matrix** — explicit "what works on which OS" table, particularly flagging J2534 flashing as Windows-only.
9. ⬜ **OFL font licenses in `NOTICE`** — Inter and JetBrains Mono ship as binary blobs; license text must accompany the binary.
10. 🟡 **CI performance gate** — binary-size axis enforced via `scripts/check-binary-sizes.sh` (CLI ≤ 20 MB, GUI ≤ 40 MB; headroom over current ~6 MB CLI + ~14 MB GUI). Cold-start time + idle-RAM (§1's other thresholds) remain aspirational — both need a GUI launch harness which is its own follow-up; CLI runs aren't a clean proxy for "cold start to interactive window."
11. ✅ **Property-based tests** on the codec layer + UDS framing — in-tree harness at `tests/unit/_helpers/property.hpp` (deterministic seeded PRNG, no external dep). Coverage: DVI codec (`test_obdx_dvi_properties.cpp`, 8 cases), SSM framing (`test_ssm_properties.cpp`, 8 cases), native SOF/seq/CRC-16 (`test_native_properties.cpp`, 11 cases), UDS framing + NRC dispatch + robustness across the whole parser catalog (`test_uds_properties.cpp`, 17 cases). Each parser is verified against: build/parse roundtrip, negative-response (any NRC) rejection, mismatched-echo rejection (wrong DID / wrong session / wrong counter), and adversarial-random-bytes robustness (no crash, only clean errors). ⬜ UDS state-machine sequence properties (the orchestrator-level state, not the framing) still remain.

## Personas — who each milestone serves

The architecture serves four user personas; mapping milestones to personas keeps the investment thesis legible.

| Persona | Primary tool | Headline needs |
|---|---|---|
| **Street tuner** (largest population) | GUI | Open project → edit map → flash, with strong brick protection. Cold-start + adaptive-history panels (§11) are aimed here. v1.0 + v1.2 §11 panels deliver. |
| **Dyno operator** | CLI + GUI | Batch-process logs, repeatable pipelines, `--json` output. v1.1 auto-tune CLI + (future) `--json` mode + `subuwutuner-cli doctor` deliver. |
| **Shop / professional** | GUI primarily | Fleet workflows, signed updates, jurisdiction profile management, support for handing keys to less-experienced staff. v1.2 closed-loop trim + v1.5 live tuning deliver the day-to-day iteration win. |
| **Vehicle developer / engine swapper** | CLI + CAN toolkit + custom-features designer | Reverse-engineering bus traffic, building DBCs, dumping ROMs from non-standard targets, and authoring the patch code that makes a cross-platform swap drive correctly (e.g. FA20→FA24 into a VA WRX needs a VVT/cam-angle remap as a custom feature alongside HPFP + VE table rescaling — see `docs/16-custom-features.md` worked example). v1.5+ CAN toolkit + v2.0 AI advisory deliver the reverse-engineering side; the custom-features side ships once Phase 5 patch insertion + bench validation land. |

The roadmap rows below carry which persona they target so milestone trade-offs aren't anonymous.

## After 1.0 — platform expansion

The architecture (see `02-architecture.md`) is multi-platform from day one. v1.0 ships VA + VB WRX MT only because that's what we can brick-test on the Phase-4 bench rig. Subsequent versions add platforms in this order, prioritised by community demand × engineering reuse:

| Version | Platform / feature | Engineering cost |
|---|---|---|
| **v1.1** | VA + VB WRX **AT** | Small. Same ECU, additional transmission map set + AT-specific definitions |
| **v1.1** | **MAF auto-tune + knock-based ignition pull** (see `docs/12-auto-tuning.md`) ✅ shipped (kernels + lint + CLI + GUI modals) | Medium. Pure-domain function, no hardware deps |
| **v1.2** | **VA STI (EJ257)** + older STI 2008+ | Medium. Different engine family but shares much of the protocol surface |
| **v1.2** | **Closed-loop trim integration, boost auto-trim, idle target trim** | Medium |
| **v1.2** | **Under-served-coverage feature pack** — adaptive-learning history visualizer, per-cylinder knock dashboard, cold-start tuning workflow, EBCS PID assistant (see `05-improvements.md` §11) | Medium. Pure-domain features over existing pack data; shares infra with auto-tune |
| **v1.3** | **Older EJ-powered cars** (early WRX/STI, Forester XT, Legacy GT, Outback XT, EJ20/EJ25) | Medium. Oldest ECU tech but very well mapped by RomRaider — mostly definitions work |
| **v1.4** | **BRZ / Toyota 86 (FA20D NA)** | Large. Toyota-partnership ECU, different vendor, biggest single-platform engineering ask |
| **v1.5** | **Live tuning** — RAM-shadow override + UDS WriteDataByIdentifier for on-dyno cell editing (see `docs/19-live-tuning.md`) | Large. New `st::live_tune` module + per-CID RAM-shadow address tables + per-write safety linting; gated on Phase 4 hardware validation |
| **v2.0** | **AI advisory surface** — rules-based drift classifier + optional local-LLM explanation layer + "explain this log" assistant (see `docs/20-ai-integration.md`). Advisory only; no path into the write surface. | Medium. New `st::ai` module + Backend abstraction (Ollama / OpenAI / Anthropic) + classifier rules over the existing log snapshots. Compile-time-optional. |
| **v2.0** | **Crosstrek / Forester (current FB-powered)**, regional variants | Definitions work plus light protocol additions |

## Cross-cutting v1.x improvements

- ⬜ VB Linux/M-series J2534 parity
- ⬜ ELM327 write path (only if we can prove it's safe — likely never)
- ⬜ Bench-tools mode for ECU benches (Tactrix Pro J)
- ✅ `defgen` tool to convert RomRaider XML → our TOML schema (Python 3.12+ in `tools/defgen/`; 88 tests)
- ✅ **OBD-II Mode 0x09 vehicle-info** (CAL ID / CVN / VIN) — `st::ecu::uds` extension + CLI; lets a tool read the calibration identifier without an authenticated session, which underpins per-CID definition-pack auto-selection.
- ✅ **SecurityAccess for A-series SSMCAN1** — factory Feistel + COBB-AP / Fehr-active L1+L3 variants in tree, CLI `--sa-variant {factory,cobb-ap,fehr-active-l1,fehr-active-l3}`; see `docs/23-security-access.md`.
- ✅ **Optional gated 0xB6 bulk-reflash cipher** (build flag `ST_ENABLE_BULK_REFLASH_CIPHER` + runtime `--enable-bulk-reflash-cipher`); see `docs/26-bulk-reflash-cipher.md`.
- ✅ **DTC enable/disable** via `[[dtc_bitmap]]` schema in definition packs (see `11-definition-format.md`). CLI:
  ```
  pack-dtcs <DEF>                       # list known codes + emissions flag
  project-disable-dtc --code P0401[,...] <dir>
  project-enable-dtc  --code P0401[,...] <dir>
  ```
  Same jurisdiction-profile linter applies as for emissions-flagged table edits. Edits route through `st::edit::History` (DTC undo lands at `0ad6d13`).
- ⬜ Doc 18 standalone handheld — `transport_native` codec + Transport landed PC-side; the embedded firmware (FlexCAN_T4 + K-Line direct silicon + ESP32 radio coprocessor) is its own multi-quarter project per `docs/18-standalone-master-plan.md`.

## v1.5+ — CAN reverse-engineering toolkit

For users doing engine swaps, cluster integration, or general "what does this byte mean?" reverse-engineering work on a vehicle's CAN bus, SubuwuTuner grows a programmatic discovery loop: tool watches the bus, builds a baseline statistical model, prompts the user when a stable byte changes, records labeled events, exports to a draft DBC. Full design in `docs/14-can-reverse-engineering.md`. Reuses `st::transport::ITransport::start_streaming` so the live mode plugs into existing adapters; replay mode lets the discovery algorithm run unit-tested without any hardware. Optional LLM-assisted bit-field refinement step on the resulting `.cdb` file.

CLI shape (✅ = shipped, replay-only path):

```
can-record    --bus hs --duration 60s out.asc                       # hardware-only
can-discover  --baseline 10s --from out.asc --output session.cdb    # ✅ replay
can-discover  --live ...                                            # hardware-only
can-replay    out.asc                                               # ✅
can-decode    --dbc subaru.dbc out.asc > signals.csv                # ✅
can-diff      a.asc b.asc                                           # ✅
can-export-dbc session.cdb > draft.dbc                              # ✅
```
