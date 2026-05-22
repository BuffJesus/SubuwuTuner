# SubuwuTuner — Design & Planning Docs

This folder contains the design and planning documents for **SubuwuTuner**, a comprehensive, free, open-source Subaru ECU tuning suite written in modern C++23. v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission); v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and beyond as definition packs land and flash routines clear bench testing.

## Read in this order

1. [00-overview.md](00-overview.md) — What we are building and why
2. [01-reverse-engineering.md](01-reverse-engineering.md) — ECU protocol study plan and definition-format strategy
3. [02-architecture.md](02-architecture.md) — Module layout and core abstractions in C++
4. [03-tech-stack.md](03-tech-stack.md) — Compiler, build, GUI, libraries
5. [04-roadmap.md](04-roadmap.md) — Phased delivery plan and milestones
6. [05-improvements.md](05-improvements.md) — What makes SubuwuTuner different
7. [06-legal-ethics.md](06-legal-ethics.md) — Emissions, IP, distribution constraints
8. [07-build-and-tooling.md](07-build-and-tooling.md) — Toolchain, CI, packaging
9. [08-testing-strategy.md](08-testing-strategy.md) — Unit, hardware-in-the-loop, fuzzing
10. [09-risks.md](09-risks.md) — What can kill the project, and mitigations
11. [10-glossary.md](10-glossary.md) — ECU/tuning vocabulary cheat sheet
12. [11-definition-format.md](11-definition-format.md) — TOML schema for definition packs
13. [12-auto-tuning.md](12-auto-tuning.md) — MAF/knock/closed-loop auto-tune design (v1.1+)
14. [13-transport.md](13-transport.md) — Phase 3 transport-layer + ECU protocol design
15. [14-can-reverse-engineering.md](14-can-reverse-engineering.md) — Programmatic CAN signal discovery (Phase 5+)
16. [15-clean-room-engineering.md](15-clean-room-engineering.md) — Clean-room methodology (analyst/implementer wall, solo-dev adaptations, AI-tool contamination channels)
17. [16-custom-features.md](16-custom-features.md) — Node-graph custom-feature designer (Phase 5)
18. [17-data-distribution-policy.md](17-data-distribution-policy.md) — Path B: tool ships public, definition packs are user-supplied
19. [18-standalone-master-plan.md](18-standalone-master-plan.md) — Portable Teensy/ESP32 handheld plan
20. [19-live-tuning.md](19-live-tuning.md) — RAM-shadow live tuning design (v1.5+)
21. [20-ai-integration.md](20-ai-integration.md) — AI / LLM as advisory surface, not modification surface (v2.0+)
22. [21-stune-format.md](21-stune-format.md) — `.stune` project directory layout and TOML schema

## Status

**Phase 0 done; Phase 1 CLI side done; Phase 2 MVP done (persistence + undo/redo); Phase 3 protocol framing complete; Phase 3 datalogger pipeline end-to-end hardware-free; CAN toolkit replay path mostly built (Frame + `.asc` I/O + DBC parser/emitter + BaselineModel/ChangeDetector in)** (2026-05-12). Working: `st::core`, `st::Rom` (read + write), `st::Definition` (single-file or directory loader with `extends` chain resolution, typed reads + writes, scaling + inverse, axis/table value extraction for 1D/2D/3D, diff, writeback), `st::edit` (Rect-scoped set/add/multiply/percent/smooth/interpolate, Snapshot, History undo/redo with branching), `st::Project` (.stune directory persistence with persisted edit history), `st::transport` (ITransport + MockTransport), `st::ecu::ssm` (A8 read + B0 single-byte write + B8 block write + SsmClient), `st::ecu::uds` (**complete catalog**: RDBI/WDBI/RMBA/WMBA/SA/CommControl/DSC/ECUReset/TP/RoutineControl/Download/Transfer/Exit — full flashing flow including erase + check-deps through MockTransport), `st::log::LogStream` (SPSC lock-free ring; stress-tested at 50k samples) + `st::log::LogSession` (I/O-thread orchestrator: batched SSM A8 reads, per-channel scaling, timestamped sample push) + `st::log::CsvSink` (header + per-channel precision + ms timestamps), `st::can::Frame` + `.asc` reader/writer (Vector lingua-franca text format), `st::dbc::Database` parser + emitter (BO_/SG_ messages + signals; tolerates unknown sections; round-trip stable), `st::discover::BaselineModel` + `ChangeDetector` (per-byte Stable/Cyclic/Noisy classification; coalesces multi-byte changes; 500ms debounce; NewId events), `tools/defgen` (Python RomRaider→TOML with `<base>` inheritance). CLI: `rom-info`, `dump-axis`, `dump-table [--csv]`, `rom-diff`, `table-edit`, `project-{new,info,edit,undo,redo}`, `pack-info`, `table-list`, `log` (trace-file → CSV via MockTransport). GUI: docking, welcome panel, chip-based table header + status bar, ImPlot heatmap view, 3D slice picker, edit toolbar with undo/redo. **267 C++ + 36 Python tests green** locally on MinGW g++ 15.2. CI: Win MSVC / macOS Apple Clang / Linux GCC / Linux Clang ASan+UBSan + Python defgen tests. Phase 5+ custom-features designer designed in `docs/16`. Remaining hardware-free work on the CAN toolkit: `.cdb` discovery file format (TOML serialisation of BaselineModel + DiscoveryEvent stream), `can-*` CLI subcommands (`can-record`, `can-discover --from`, `can-replay`, `can-decode`, `can-export-dbc`, `can-diff`).
