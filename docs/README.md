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
23. [22-auto-update.md](22-auto-update.md) — `st::updater` design sketch (Phase 6): channels, manifest, signature verification, swap mechanics
24. [23-security-access.md](23-security-access.md) — SecurityAccess (UDS 0x27): factory SSMCAN1 Feistel + COBB-AP / Fehr-active variants in tree, CLI-selectable
25. [24-sniff-workflows.md](24-sniff-workflows.md) — Y-cable sniffing playbooks (during-flash capture, RAM-poll discovery, protocol learning, feature RE)
26. [25-config-system.md](25-config-system.md) — `st::config` user-config layout, `[security_access].handheld_serial`, persistence rules
27. [26-bulk-reflash-cipher.md](26-bulk-reflash-cipher.md) — Optional gated 0xB6 bulk-transfer write path: two-step arming, failure modes, brick-risk rationale
28. [27-fehr-analysis-2026-05-26.md](27-fehr-analysis-2026-05-26.md) — Fehr e-tune calibration analysis: cal delta vs factory, SA L1/L3 status, patch manifests
29. [28-bench-rig-build.md](28-bench-rig-build.md) — Junkyard-ECU bench-rig assembly runbook: FSM-page-referenced harness wiring, minimum-viable power-on, OBDX bring-up, first read, brick-recovery loop

## Status

**Phases 0–4 done hardware-free; Phase 5 design + IR + SH-2A codegen for VA shipped + RH850 codegen for VB partial (5 primitives); bench validation is the open gate before v1.0** (as of 2026-05-30). Live-side shape: `st::core`, `st::Rom` (read + write), `st::Definition` (loader with `extends` chains, typed reads + writes, scaling + inverse, 1D/2D/3D axis/table extraction, diff, writeback), `st::edit` (Rect-scoped ops + Snapshot + History undo/redo with branching), `st::Project` (.stune directory persistence with persisted edit history), `st::transport` (ITransport + MockTransport + J2534 + OBDX + native handheld codecs), `st::ecu::ssm` + `st::ecu::uds` (complete catalog: RDBI/WDBI/RMBA/WMBA/SA/CommControl/DSC/ECUReset/TP/RoutineControl/Download/Transfer/Exit + Mode 0x09 vehicle-info), `st::ecu::subaru_security` (factory Feistel + COBB-AP + Fehr-active L1/L3 variants), `st::ecu::bulk_reflash` (gated 0xB6 cipher, off by default), `st::flash` (orchestrator with delta detection, per-sector erase/write/verify, dry-run, manifest, journal-based resume, optional 0xB6 path, `PolicyDenied` gate), `st::log` (LogStream + LogSession + CsvSink), `st::can` + `st::dbc` + `st::discover` (full replay-path CAN RE toolkit + `.cdb` bundle), `st::autotune` (MAF + knock-pull kernels + CSV reader + smoothing + lint), `st::feature::Graph` + `st::feature::ir` + `st::feature_codegen::Sh2aBackend` (22 primitives, fan-out dedup, FPU bridge, address-gate, hardcoded `flex_fuel_scale` curve) + `Rh850Backend` (5 primitives: add_int, subtract_int, and_bool, or_bool, not_bool), `st::policy`, `st::config`, `tools/defgen`. CLI: 60+ subcommands incl. `rom-{info,pull,diff}`, `dump-axis`, `dump-table`, `table-edit`, `project-*`, `pack-*`, `log`, `flash`, `feature-compile`, `can-*`, `transport-list`, `doctor`, `checksum-{verify,repair}`, `ssm-a8-poll`, `--sa-variant`, `--cobb-tuned`, `--enable-bulk-reflash-cipher`. GUI: docking, Dark/Light + purple accent themes, ImPlot heatmap, 3D slice picker, autotune modals, Flash modal with policy gate, Stats panel, Settings dialog, custom-features designer canvas. **1100+ C++ + 36 Python tests green** locally on MinGW g++ 15.2; CI: Win MSVC / macOS Apple Clang / Linux GCC / Linux Clang ASan+UBSan + defgen tests. Remaining RH850 codegen slices (multiply/divide, Int compares, Float/FPU) and bench-validation against a real ECU concentrate the open work (OBDX Pro VX is in hand 2026-05-24).
