# `src/` — Module layout

One-line summaries of every module. Each module has its own `include/st/<name>/` (public headers) + `src/` (impl) + a corresponding `tests/unit/<name>/`. Per `docs/02-architecture.md`, the project is layered — lower modules don't depend on higher ones.

## Foundations (every other module depends on these)

| Module | Purpose | Doc |
|---|---|---|
| **`core/`** | `Result<T>` + `Error` + units + `Span` + JSON util + version. The C++23 idiom layer. | `docs/02` |
| **`rom/`** | Raw ROM bytes — read/write, CRC32, sector hashes. Format-agnostic. | — |
| **`defs/`** | TOML definition packs. Loader, `extends` chains, validation, `find_table`/`find_axis`/`find_workflow`. | `docs/11` |
| **`edit/`** | Rect-scoped table edits + Snapshot + `History` (undo/redo + `undo_while_tag` for transactional batches). | — |
| **`project/`** | `.stune` directory model — `project.toml`, `source.bin`, `working.bin`, `edits.toml`, optional `[[rom]]` additional ROMs. | `docs/21` |
| **`policy/`** | Jurisdiction profiles + `FlashPreflight` pipeline + per-edit gates. | `docs/06` |
| **`profile/`** | Per-vehicle profile registry (CID, VIN, handheld serial). User-local. | `docs/25` |
| **`config/`** | `st::config` user-config + `settings.toml`. PackRegistry paths. | `docs/25` |
| **`audit/`** | CRC32-protected append-only log. Subscriber seam wired across UDS + Flasher. | `docs/02` |

## ECU protocols + transport

| Module | Purpose | Doc |
|---|---|---|
| **`transport/`** | `ITransport` interface + concrete impls (`mock`, `j2534`, `obdx_dvi`, `native`, `ets`). USB-CDC + libusb + DLL dynload. | `docs/13` |
| **`ecu/`** | SSM (K-line + CAN) + UDS clients. Full UDS catalog incl. flash flow + OBD-II Mode 0x09. SecurityAccess variants (factory + aftermarket L1/L3). Optional gated 0xB6 bulk-reflash cipher. | `docs/13`, `docs/23`, `docs/26` |
| **`discover/`** | J2534 adapter discovery via Windows registry. CAN signal discovery (BaselineModel + ChangeDetector). | `docs/14` |
| **`devices/ets/`** | COBB AccessPort V3 client — `Client` (state/ls/read/write/remove/raw), file-vault codec, `.ptm` cipher chain (XTEA + base64 + AES-256-custom-CTR + bzip2), OTA `.img` Blowfish, architectural classifier, datalogger-cfg codec. Gated by `ST_ENABLE_COBB_AP_CIPHER` / `ST_ENABLE_COBB_AP_PTM_REWRITE`. | `docs/34`, `docs/35` |

## Datalogging + CAN reverse engineering

| Module | Purpose | Doc |
|---|---|---|
| **`log/`** | `LogStream` + `LogSession` + `CsvSink` + `LiveBuffer` (SPSC ring for gauges). Coldstart / EBCS / knock / adaptive-history analyzers. | `docs/12`, `docs/32` |
| **`can/`** | CAN `Frame` + Vector `.asc` I/O. | `docs/14` |
| **`dbc/`** | DBC parser/emitter/decoder. `.cdb` session bundles. | `docs/14` |
| **`ai/`** | Tier-1 rules-based drift classifier + LLM backend abstraction (advisory only, no write path). | `docs/20` |

## Flash + custom features

| Module | Purpose | Doc |
|---|---|---|
| **`flash/`** | `Flasher` orchestrator — delta detection, per-sector erase/write/verify, dry-run, manifest, journal-based resume, optional gated 0xB6 path, brick-protection preflight, boot-signature host-side mirror. Safety-critical. | `docs/05`, `docs/26`, `docs/31`, `docs/37`, `docs/40` |
| **`autotune/`** | MAF + knock-pull kernels. CSV reader + smoothing + lint. Wires into `edit::History` via `apply_op_table`. | `docs/12` |
| **`feature/`** | Node-graph custom-feature model + `Graph` + `ir::Module` lowerer + linters. `.stmod` round-trip. | `docs/16` |
| **`feature_codegen/`** | IR → bytecode. SH-2A + RH850 backends at parity (22 primitives each). `gate_patch` writable-region check. | `docs/16` |
| **`feature_patch/`** | Patch insertion layer — ROM allocator + splice mechanics (SH-2A short/long-form, RH850 dual-bank). `PatchObject → PatchedRom`. Bench-validation gated. | `docs/30` |
| **`diff/`** | ROM-vs-ROM byte + table diff. | — |
| **`library/`** | `patch_decoder` + `tune_diff` + `table_mapping` (patch ↔ table) + `ptm_xml_builder` + `interpret_inspect`/`interpret_diff` heuristic prose + `Atlas` (corpus-derived tuning knowledge). | `docs/35`, `docs/36`, `docs/39` |

## Front-ends

| Module | Purpose | Doc |
|---|---|---|
| **`cli/`** | `subuwutuner-cli` — 70+ subcommands. Single `main.cpp`. Stable JSON/TOML output for `--format` flags. | — |
| **`ui/`** | `subuwutuner-gui` — Dear ImGui + ImPlot + GLFW. Docking layout, themes (Dark/Light + purple accent), panels (Welcome / Sidebar / TableView / Stats / Compare / History / Audit / KnockDashboard / AdaptiveHistory / Coldstart / EBCS / DTCs / FeaturesDesigner / GaugeCluster / AccessPort Browser), modals (Flash / ReadRom / NewProject / DefRegistry / Settings / FirstRun / Help / Shortcuts / About / CSV / Autotune-MAF / Autotune-Knock / PtmImport / PtmInspect / PtmDiff / PtmExport / FA24Swap / TGV-EGR-Delete / Unsaved). F1 context-routing per panel. | — |

## Layer rules (per `docs/02`)

- **Domain (everything except `ui/`)** must NOT depend on ImGui or libusb in its public headers. Use opaque `IByteChannel` / `ITransport` / `IUsbDevice` interfaces; UI implements them.
- **`ui/` depends on everything below it.** Inverse not allowed.
- **`cli/` depends on everything except `ui/`.** Same direction rule.
- **No global state.** Services flow in via constructor injection at the application layer (cli/main.cpp + ui/main.cpp).

## Adding a new module

1. `mkdir src/<name>/{include/st/<name>,src}` + `tests/unit/<name>/`.
2. `src/<name>/CMakeLists.txt` declares `add_library(st_<name>) target_include_directories(...)` — copy from a small sibling like `audit/` or `profile/`.
3. Add the line `add_subdirectory(src/<name>)` to root `CMakeLists.txt`.
4. Wire test-build in `tests/CMakeLists.txt`.
5. Public headers in `include/st/<name>/` — installed; downstream module deps via `target_link_libraries(st_other PUBLIC st_<name>)`.

## Cross-references

- `docs/02-architecture.md` — full architectural model with diagrams.
- `docs/04-roadmap.md` — phase-by-phase delivery plan that maps to these modules.
- `docs/07-build-and-tooling.md` — CMake / CI / warnings policy that every module ships under.
- `tests/README.md` — test-tree convention (mirrors this layout under `tests/unit/<module>/`).
- `CLAUDE.md` — load-bearing technical invariants future sessions need.
