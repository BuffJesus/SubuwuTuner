# 02 — Architecture

## Designed for breadth from day one

The longer-term scope is **all Subaru platforms we can brick-test**, not just VA/VB WRX MT. The architecture below is laid out so that adding a new platform is additive, not invasive:

- **New vehicle** = new TOML definition pack (user-supplied; see Path B in `docs/17`). `st::defs` doesn't care whether the pack describes a 2019 WRX, a 2008 STI, or a 2015 Forester XT.
- **New ECU family / protocol variant** = new module under `st::ecu::<family>`. Existing modules (`ssm`, `uds`) are untouched. Per-family flash quirks belong in `st::flash`'s checksum-repair seam (`IChecksumRepair`) rather than a per-family subdirectory.
- **New transport** = new module under `st::transport::<adapter>` implementing the same `ITransport` interface and registered with the `open_transport` factory.

What this means in practice: every interface in the layering below takes a `Platform` or `Definition` argument; nothing is hard-coded to "WRX". Per Path B (`docs/17`) the public repo no longer bundles VA/VB WRX calibration packs (older Subaru packs + SSM PIDs + ecuparams fragments still ship) — `subuwutuner-cli rom-info` will work on any Subaru ROM the moment a matching pack is supplied (or generated with `tools/defgen/`).

See `04-roadmap.md` for the v1.x platform-expansion order.

## Layering

```
┌────────────────────────────────────────────────────┐
│  UI layer    (Dear ImGui + GLFW + ImPlot)          │
├────────────────────────────────────────────────────┤
│  Application services  (project mgr, undo/redo,    │
│                         policy gate, autotune,     │
│                         feature designer)          │
├────────────────────────────────────────────────────┤
│  Domain model  (ROM, Table, Axis, Definition,      │
│                 Edit, LogSession, FlashPlan,       │
│                 feature::Graph, feature::ir)       │
├────────────────────────────────────────────────────┤
│  Transport    (J2534, OBDX Pro VX DVI,             │
│                native USB-CDC framed codec)        │
├────────────────────────────────────────────────────┤
│  Platform abstraction  (USB, serial, FS, threads)  │
└────────────────────────────────────────────────────┘
```

Every layer depends only on layers below it. The domain model has no ImGui or USB types in its public headers — that is what lets us unit-test it and reuse it from a CLI. The UI layer reads domain types directly (no `MVC`/`MVVM` ceremony) but writes back through application-services entry points so undo/redo and project-state invariants are enforced in one place.

## Polish layer (UI)

"Looks great + functions great" decomposes into concrete deliverables on top of the base Dear ImGui shell:

- **Theme** — tuned dark palette (high contrast for numerical work, low chroma for long sessions), Inter as UI font, JetBrains Mono in grids and chart axes, padding/rounding/border tuned away from ImGui defaults.
- **Docking** — ImGui docking branch with viewports so panels can tear off into OS-level windows on multi-monitor setups.
- **Charts** — ImPlot for every live-data view (datalogger, AFR/timing trace, fuel-trim heatmap).
- **Dialogs** — nativefiledialog-extended (nfd) for Open/Save; ImGui modal popups for confirmations.
- **Map editor** — first-party 2D/3D table widget with axis headers, color-coded heatmap overlay, paste-from-spreadsheet, keyboard navigation, undo/redo bound to `st::edit::History`.

## Module map

| Module | Responsibility | Key types |
|---|---|---|
| `st::core` | Value types, error handling, units | `Result<T>`, `Error`, `ErrorCode`, `Status` |
| `st::rom` | Binary ROM I/O, CRC | `Rom` |
| `st::defs` | Calibration definitions, scaling | `Definition`, `Table`, `Axis`, `Scaling`, `Pid`, `Hook` |
| `st::edit` | Undoable edit history (table cells + raw bytes) | `History`, `Edit`, `TableEdit`, `ByteEdit`, `Snapshot` |
| `st::project` | `.stune` project files | `Project` |
| `st::policy` | Jurisdiction-profile lint / flash gate | `Profile`, `Decision`, `Action` |
| `st::transport` | ECU comms abstraction | `ITransport`, `Frame`, `LinkConfig`, `MockTransport`, `IByteChannel`, `open_transport` |
| `st::transport::j2534` | J2534 v04.04 vendor-DLL wrapper + Windows registry discovery | `J2534Library`, `j2534::Transport`, `j2534_discovery::scan` |
| `st::transport::obdx` | OBDX Pro VX DVI codec + transport (USB CDC byte channel) | `obdx::Transport`, DVI codec |
| `st::transport::native` | SOF/seq/opcode/LEN/CRC16 framing for the standalone-master handheld | `native::Transport`, `native` codec |
| `st::ecu::ssm` | Subaru SSM (K-Line + CAN-encapsulated) | `SsmClient` |
| `st::ecu::uds` | ISO 14229 UDS / KWP-on-CAN | `UdsClient` |
| `st::flash` | Erase/program/verify + brick guard + checksum repair seam | `Flasher`, `FlashPlan`, `FlashReport`, `IChecksumRepair`, `make_checksum_repair`, `apply_checksum_repair` |
| `st::log` | Datalogging (SPSC ring + I/O thread + sinks) | `LogStream`, `LogSession`, `LogChannel`, `CsvSink` |
| `st::can` | CAN frames + .asc trace I/O | `Frame`, `AscReader`, `AscWriter` |
| `st::dbc` | DBC parser / emitter / decoder | `Database`, `Message`, `Signal` |
| `st::discover` | CAN reverse-engineering: baseline + change detection + .cdb bundle | `BaselineModel`, `ChangeDetector`, `DiscoveryEvent`, `Bundle` |
| `st::autotune` | Docs/12 MAF + knock-pull tuning kernels | `MafTuneOptions`/`Result`, `KnockPullOptions`/`Result`, `LintViolation` |
| `st::feature` | Custom-feature node graph (designer-canvas source) | `Graph`, `Node`, `Edge`, `Pin`, `feature::ir::Module` |
| `st::feature::codegen` | Graph → IR → patch bytes (per-ISA backends) | `IBackend`, `Sh2aBackend`, `Rh850Backend`, `PatchObject`, `RamAllocator` |
| `st::ui` | GUI shell (Dear ImGui + GLFW + ImPlot) | windows, panels, theme, view code bound to domain |
| `st::cli` | Headless tool (`subuwutuner-cli`) | argparse + same domain |

## Concurrency model

Three thread categories:

- **UI thread** — the only one allowed to touch ImGui state or call `ImGui_ImplOpenGL3_*` / `glfwPoll*`.
- **Worker pool** — `std::jthread` workers driven by a lightweight task system for CPU-bound work (compression, CRC, codegen).
- **I/O reactor** — one dedicated thread per transport device. ECU comms are inherently serial; multiplexing them onto a reactor avoids leaking USB/serial handles across threads.

Communication is via message passing (a typed `concurrent_queue<Cmd>` per worker, plus a `Result`-bearing future returned to the caller). No shared mutable state across thread boundaries.

## Error handling

`std::expected<T, st::Error>` everywhere in domain/transport code. Exceptions only at the UI boundary, and only to interrupt long-running operations through `std::stop_token`. Cancellation must propagate to in-flight flash operations safely — see `05-improvements.md` for the brick-protection design. ImGui's immediate-mode loop never throws across frames; any exception caught at the UI surface gets logged + surfaced via a modal popup.

## Plugin / extension surface

We expose two extension points for community contributions and power-user customization:

1. **Definition packs** — TOML files / directories. The public repo carries the demo pack + older Subaru packs (Impreza, Forester, Legacy, etc.) + SSM PID + ecuparams fragments; VA/VB WRX packs are user-supplied per Path B (`docs/17`) and can be generated from public RomRaider XML via `tools/defgen/`. Users point the CLI/GUI at any pack path.
2. **`.stmod` feature graphs** — TOML documents holding `[graph]` + `[[node]]` + `[[edge]]` (the source graph) and optionally `[patch]` (the codegen output bundled in the same file). Lowered through `st::feature::ir` and compiled by `st::feature::codegen` per `docs/16`. The IR is a typed dataflow SSA representation, not Lua — the earlier Lua-as-IR direction was dropped before any of it shipped.

Native (DLL/SO) plugins are explicitly **out of scope for v1** — they are a vector for malicious tunes and a portability hazard.
