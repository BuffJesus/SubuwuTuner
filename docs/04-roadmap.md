# 04 — Roadmap

Six phases. Each ends with a demoable artifact and a "ship gate" that must pass before the next phase starts. Time estimates assume one to two developers part-time.

**Status legend** (per phase / per bullet):
✅ shipped · 🟡 partially shipped · ⬜ not yet · 🔒 blocked on hardware

## Phase 0 — Foundations (2–3 weeks) ✅ done

- ✅ Repo skeleton, CMake presets, vcpkg manifest, CI on Win/Mac/Linux
- ✅ `st::core` (`Result`, `Error`, units, `Span`)
- ✅ Catch2 wired up, **700+ tests green** (was sized at 50 originally)
- ✅ `subuwutuner-cli` binary with version + 40+ subcommands

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

## Phase 3 — Comms & datalogging (5–7 weeks) 🟡 in progress

Substantial architecture landed this session; the platform USB / DLL layer + real-hardware validation gate on adapter arrival.

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

## Phase 4 — Flashing (6–10 weeks, the dangerous one) 🟡 skeletons done

Per the prior handoff, "Phases 0–4 done hardware-free." Orchestrator + Manifest + journal + policy gate + checksum-type field all exist; the actual algorithms (vendor-specific checksum repair, seed/key derivation, brick recovery) need hardware to develop + validate against.

- ✅ `Flasher::read_full_rom` via `ITransport` + UDS RequestDownload/TransferData (mock-trace exercised)
- ✅ FlashPlan + sector model (`st::flash`)
- ✅ Manifest + journal + `plan_resume` for crash-safe writes
- ✅ Policy + mutation gate (`docs/06` + `st::policy`)
- ✅ `checksum_type` field in pack `[pack]` table (added `58a821f`); enum mirrors RR's ChecksumSTD / ALT / ALT2 family
- ✅ `IChecksumRepair` seam + `make_checksum_repair` factory + `apply_checksum_repair(span, Definition)` wrapper + CLI `checksum-verify` / `checksum-repair` exit-3-on-NotImplemented (every concrete kind still returns NotImplemented with an RR citation pointer)
- ⬜ Checksum-repair implementations (subaru_std, subaru_alt, subaru_alt2) — seam ready; algorithms still need byte-validation against a known stock dump
- ⬜ Seed/key authentication — no SSM seed/key code exists in RomRaider per this session's findings doc; we'll derive it from forum threads + bench captures
- 🔒 Brick protection bootstrap + recovery shim — bench rig prerequisite
- 🔒 Delta-only flashing + dry-run mode — same
- ⬜ Patch insertion layer (`src/feature_patch/`) — for Phase 5 features; needs real ECU vector tables

**Gate:** 🔒 100 successful flash cycles on a junkyard ECU bench rig — zero bricks, zero corrupted images. **No customer ever flashes a car until this gate is met.** Hardware-blocked.

## Phase 5 — Custom features (4–6 weeks) 🟡 design + tooling complete

Heavy progress this session — what was sized at 4–6 weeks for the editor + IR + one backend shipped much faster. Delivery path (patch insertion + flashing) waits on Phase 4 hardware.

- ✅ Visual node-graph editor with pin labels + per-pin defaults + wire dragging + pin-context menus; promoted out of `View → Debug` to top-level `View → Custom features designer (preview)` (`0cc5c1b`)
- ✅ Graph data model (`st::feature::Graph`) + IR lowerer (`st::feature::ir::Module`) + graph-level + IR-level linters
- ✅ SH-2A codegen: Int arithmetic (add/sub/mul/`divide_int` via FPU bridge), Int compares, Bool ops, select (int/bool/float), Float arithmetic via FPU (FADD/FSUB/FMUL/FDIV), Float compares (FCMP/EQ/GT), cross-hook value flow, fan-out dedup. **18 primitives recognized.**
- ⬜ Curve / table-lookup primitives (e.g. `flex_fuel_scale`) — needs pack-format extension for breakpoints + values
- ⬜ RH850 codegen for VB — stub backend returns NotImplemented; per `docs/16` recommendation, drop first under timing pressure
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
- ⬜ Installer / codesigning / auto-update channel
- ⬜ Documentation site (Jekyll → MkDocs or similar)
- ⬜ Onboarding flow for first-time users — partial (welcome panel + new-project modal pack hints landed)
- ⬜ Telemetry **opt-in only**, crash-report-only, no analytics

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
| **v2.0** | **Crosstrek / Forester (current FB-powered)**, regional variants | Definitions work plus light protocol additions |

## Cross-cutting v1.x improvements

- ⬜ VB Linux/M-series J2534 parity
- ⬜ ELM327 write path (only if we can prove it's safe — likely never)
- ⬜ Bench-tools mode for ECU benches (Tactrix Pro J)
- ✅ `defgen` tool to convert RomRaider XML → our TOML schema (Python 3.12+ in `tools/defgen/`; 88 tests)
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
