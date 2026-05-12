# 04 — Roadmap

Six phases. Each ends with a demoable artifact and a "ship gate" that must pass before the next phase starts. Time estimates assume one to two developers part-time.

## Phase 0 — Foundations (2–3 weeks)

- Repo skeleton, CMake presets, vcpkg manifest, CI on Win/Mac/Linux
- `st::core` (`Result`, `Error`, units, `Span`)
- Catch2 wired up, first 50 tests green
- `subuwutuner-cli` "hello" binary that prints version and exits

**Gate:** green CI on all three OSes; binary runs.

## Phase 1 — ROM viewer, no hardware (4–6 weeks)

- `st::rom` reads raw binary dumps, computes CRC32 / sector hashes
- TOML definition parser (`st::defs`)
- Port public RomRaider definitions for VA-WRX-MT and VB-WRX-MT into our TOML
- Read-only 2D and 3D table render in a Dear ImGui window
- Open a known stock dump → see the boost target map laid out correctly

**Gate:** load a known ROM, identify ≥ 20 factory maps with correct scaling, screenshot looks like a table.

## Phase 2 — Editing & projects (3–4 weeks)

- Undo/redo stack (`st::project`)
- Table editor with selection, paste, smooth, interpolate, percent-scale
- `.stune` project save/load (zip-backed)
- Diff view between two ROMs of the same definition

**Gate:** open stock, change a map, save project, close, reopen, change is preserved and visible.

## Phase 3 — Comms & datalogging (5–7 weeks)

- `st::transport` abstraction
- J2534 backend (Tactrix OP2.0 via vendor DLL on Windows; OpenPort runtime on Mac/Linux)
- ELM327 backend over serial — read-only datalogging at first
- `st::ecu.ssm` for VA; UDS skeleton for VB
- Live gauge cluster (4–8 gauges) and CSV log export
- Sustained 50 Hz logging across 20 PIDs

**Gate:** connect to a real VA WRX, see RPM update live, log a 10-minute drive, replay the CSV in our viewer.

## Phase 4 — Flashing (6–10 weeks, the dangerous one)

- Read full ROM from ECU
- Seed/key auth, sector erase, program, verify
- **Brick protection bootstrap** — install our recovery shim *before* the first user write touches main flash. This is the safety story. See `05-improvements.md`.
- Delta-only flashing — only re-write sectors that changed since the last read
- "Dry run" mode that exercises every step except the actual write command

**Gate:** 100 successful flash cycles on a junkyard ECU bench rig — zero bricks, zero corrupted images. **No customer ever flashes a car until this gate is met.**

## Phase 5 — Custom features (4–6 weeks)

- Lua-based "custom feature" runtime
- Visual node graph compiler that lowers to Lua → bytecode patches inserted into the ROM
- Import/export of standalone feature packs ("STMods")

**Gate:** community can publish and import a feature pack; sample pack (e.g. flat-foot shifting, launch control) ships in-box.

## Phase 6 — Polish & 1.0 (ongoing)

- Themed UI, accessibility pass, installer/codesigning, auto-update channel
- Documentation site (rebuild of the Jekyll content under our own brand)
- Onboarding flow for first-time users
- Telemetry **opt-in only**, crash-report-only, no analytics

## After 1.0 — platform expansion

The architecture (see `02-architecture.md`) is multi-platform from day one. v1.0 ships VA + VB WRX MT only because that's what we can brick-test on the Phase-4 bench rig. Subsequent versions add platforms in this order, prioritised by community demand × engineering reuse:

| Version | Platform / feature | Engineering cost |
|---|---|---|
| **v1.1** | VA + VB WRX **AT** | Small. Same ECU, additional transmission map set + AT-specific definitions |
| **v1.1** | **MAF auto-tune + knock-based ignition pull** (see `docs/12-auto-tuning.md`) | Medium. Pure-domain function, no hardware deps |
| **v1.2** | **VA STI (EJ257)** + older STI 2008+ | Medium. Different engine family but shares much of the protocol surface |
| **v1.2** | **Closed-loop trim integration, boost auto-trim, idle target trim** | Medium |
| **v1.3** | **Older EJ-powered cars** (early WRX/STI, Forester XT, Legacy GT, Outback XT, EJ20/EJ25) | Medium. Oldest ECU tech but very well mapped by RomRaider — mostly definitions work |
| **v1.4** | **BRZ / Toyota 86 (FA20D NA)** | Large. Toyota-partnership ECU, different vendor, biggest single-platform engineering ask |
| **v2.0** | **Crosstrek / Forester (current FB-powered)**, regional variants | Definitions work plus light protocol additions |

## Cross-cutting v1.x improvements

- VB Linux/M-series J2534 parity
- ELM327 write path (only if we can prove it's safe — likely never)
- Bench-tools mode for ECU benches (Tactrix Pro J)
- `defgen` tool to convert RomRaider XML → our TOML schema, run on every supported platform
- **DTC enable/disable** via `[[dtc_bitmap]]` schema in definition packs (see `11-definition-format.md`). CLI:
  ```
  pack-dtcs <DEF>                       # list known codes + emissions flag
  project-disable-dtc --code P0401[,...] <dir>
  project-enable-dtc  --code P0401[,...] <dir>
  ```
  Same jurisdiction-profile linter applies as for emissions-flagged table edits.

## v1.5+ — CAN reverse-engineering toolkit

For users doing engine swaps, cluster integration, or general "what does this byte mean?" reverse-engineering work on a vehicle's CAN bus, SubuwuTuner grows a programmatic discovery loop: tool watches the bus, builds a baseline statistical model, prompts the user when a stable byte changes, records labeled events, exports to a draft DBC. Full design in `docs/14-can-reverse-engineering.md`. Reuses `st::transport::ITransport::start_streaming` so the live mode plugs into existing adapters; replay mode lets the discovery algorithm run unit-tested without any hardware. Optional LLM-assisted bit-field refinement step on the resulting `.cdb` file.

CLI shape (planned):

```
can-record    --bus hs --duration 60s out.asc
can-discover  --baseline 10s [--from out.asc | --live] session.cdb
can-replay    out.asc
can-decode    --dbc subaru.dbc out.asc > signals.csv
can-diff      a.asc b.asc
can-export-dbc session.cdb > draft.dbc
```
