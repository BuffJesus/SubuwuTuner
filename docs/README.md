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
15. [14-can-reverse-engineering.md](14-can-reverse-engineering.md) — Programmatic CAN signal discovery (replay-path shipped)
16. [15-clean-room-engineering.md](15-clean-room-engineering.md) — Clean-room methodology (analyst/implementer wall, solo-dev adaptations, AI-tool contamination channels)
17. [16-custom-features.md](16-custom-features.md) — Node-graph custom-feature designer (Phase 5)
18. [17-data-distribution-policy.md](17-data-distribution-policy.md) — Path B: tool ships public, definition packs are user-supplied
19. [18-standalone-master-plan.md](18-standalone-master-plan.md) — Portable Teensy/ESP32 handheld plan
20. [19-live-tuning.md](19-live-tuning.md) — RAM-shadow live tuning design (v1.5+)
21. [20-ai-integration.md](20-ai-integration.md) — AI / LLM as advisory surface, not modification surface (v2.0+)
22. [21-stune-format.md](21-stune-format.md) — `.stune` project directory layout and TOML schema
23. [22-auto-update.md](22-auto-update.md) — `st::updater` design sketch (Phase 6): channels, manifest, signature verification, swap mechanics
24. [23-security-access.md](23-security-access.md) — SecurityAccess (UDS 0x27): factory SSMCAN1 Feistel + aftermarket-installer variants in tree, CLI-selectable
25. [24-sniff-workflows.md](24-sniff-workflows.md) — Y-cable sniffing playbooks (during-flash capture, RAM-poll discovery, protocol learning, feature RE)
26. [25-config-system.md](25-config-system.md) — `st::config` user-config layout, `[security_access].handheld_serial`, persistence rules
27. [26-bulk-reflash-cipher.md](26-bulk-reflash-cipher.md) — Optional gated 0xB6 bulk-transfer write path: two-step arming, failure modes, brick-risk rationale
28. [28-bench-rig-build.md](28-bench-rig-build.md) — Junkyard-ECU bench-rig assembly runbook: FSM-page-referenced harness wiring, minimum-viable power-on, OBDX bring-up, first read, brick-recovery loop
29. [29-ssm-a8-poll-workflow.md](29-ssm-a8-poll-workflow.md) — Live OEM SSM-0xA8 RAM polling over ISO-15765: `ssm-a8-poll` CLI + offline correlator for recovering tuner-pack DID byte layouts without ROM disasm
30. [30-patch-insertion.md](30-patch-insertion.md) — Patch insertion layer design (`src/feature_patch/`): manifest format, ROM-allocator, splice mechanics (SH-2A short/long-form + RH850 dual-bank gate), end-to-end `PatchObject → PatchedRom`
31. [31-brick-protection-by-isa.md](31-brick-protection-by-isa.md) — Per-ISA recovery recipes (SH-2A single-bank serial-boot vs RH850 dual-bank atomic-swap); bench-rig validation plan per ISA; safety properties common to both; ship blocker #1
32. [32-live-datalogger.md](32-live-datalogger.md) — Live gauge-cluster GUI design (LiveBuffer SPSC ring, LogSession multi-sink fan-out, ImPlot mini-lines, recording-while-gauging); CSV-replay panels stay; Phase 3 live deliverable
33. [33-analyst-review-triage-2026-06-01.md](33-analyst-review-triage-2026-06-01.md) — Triage of the external analyst review (13 docs at `fixtures/private/findings_reviews_2026-06-01/`); identifies what's already shipped vs real gaps; sequences forward-looking recommendations
34. [34-cobb-ap-as-tune-vault.md](34-cobb-ap-as-tune-vault.md) — Offline Cobb AP/AS tune-vault and comparison workflow; hardware-free data-mining reference
35. [35-tuner-overlay-architecture.md](35-tuner-overlay-architecture.md) — Three-layer tuner-overlay model (Layer 1 OEM tables, Layer 2 tuner-additions in dead-fill regions, Layer 3 code patches); per-ISA region map; substrate for the architectural classifier
36. [36-tune-as-patch-set.md](36-tune-as-patch-set.md) — Reframe of tunes as composable patch sets rather than monolithic ROM images; underlies the future composition canvas and v1.5 differential flash
37. [37-subaru-flash-protocol.md](37-subaru-flash-protocol.md) — Reference architecture for `ecu::subaru::SH_CAN_Flash::*` UDS sequence + `Rh850::*` checksum-fixup pipeline; clean-room sequence-only summary; primitive `Flasher::ecu_compute_checksum(routine_id, start, end)` shipped
38. [38-subaru-sa-variants.md](38-subaru-sa-variants.md) — Catalog of SecurityAccess variants and the cross-reference for which `--sa-variant` to pass against which ECU state
39. [39-tuning-knowledge-atlas.md](39-tuning-knowledge-atlas.md) — Machine-readable consolidated tuning knowledge (170 LF79103P tables + 9 tuner clusters + 8 safety pairs + 3 anchors); TOML at `fixtures/tuner_atlas/`, C++ loader at `st::library::Atlas`, CLI surface via `subuwutuner-cli tuner-atlas`; corpus-derived augmentation of the LF79103P def
40. [40-delta-flash-brick-protection.md](40-delta-flash-brick-protection.md) — Per-ISA brick-protection extension for v1.5 differential flash; six-state SH-2A recovery enumeration (mid-erase / mid-write of signature vs non-signature sectors); journal extensions; additive to `docs/31`, four new bench-rig tests on top of its five
41. [41-async-worker-model.md](41-async-worker-model.md) — Single dedicated worker thread + FIFO queue inside `st::devices::ets::Client` (C2 from the 2026-06-13 strategic-decisions handoff); sync verbs stay blocking via `_async().get()` shims, async verbs return `std::future<Result<T>>` for GUI frame-loop polling; FIFO ordering, RAII destructor join, worker-exclusive channel ownership; stop_token cancellation planned for v1.5
42. [42-bench-rig-validation-runbook.md](42-bench-rig-validation-runbook.md) — Hardware-side validation runbook for the bench rig: picks up where `docs/28` leaves off (first ROM read done) and sequences read-back → boot-signature verify → SA against the rig → flash-protocol capture → first deliberate write → power-loss inject → delta-flash six-state validation. Pass criteria + failure-mode tracking per step.
43. [43-jtag-recovery.md](43-jtag-recovery.md) — Quarantined, unvalidated recovery reference for a bricked LF79xxxP-style ECU. The E2-Lite recommendation is withdrawn for the current SH72543-style target; exact-board probe support is required.
44. [44-tune-export.md](44-tune-export.md) — Atlas tune-export pipeline spec: `st::tune_export` module that transforms cal diffs into a sum-preserving write plan. Architecture invariants (sum target `0x5AA5` over `[0x6000, 0x200000)`, FACI lock catalog, boot integrity bytes), `validate / build_image / emit_write_plan / compute_balance` API, CID portability across LF79xxxP. Shipped as `src/tune_export/` with 18 unit tests; round-59 T3#7 confirmed cross-CID validity.
50. [45-product-direction-and-coverage.md](45-product-direction-and-coverage.md) — product design direction, UX priorities, and offline ROM/RE queue

51. [46-tuning-software-user-needs.md](46-tuning-software-user-needs.md) - public user-needs synthesis and SubuwuTuner product priorities

52. [47-task-list.md](47-task-list.md) - dependency-ordered master queue for recovery, product work, definitions, RE, and release gates
53. [48-ui-ux-and-re-plan.md](48-ui-ux-and-re-plan.md) - phased UI/UX and reverse-engineering plan, including offline-first work and hardware-return gates
54. [49-diy-sh2a-recovery-probe.md](49-diy-sh2a-recovery-probe.md) - hardware-free research boundary and future exact-board SH-2A recovery probe investigation
55. [50-replacement-ecu-intake.md](50-replacement-ecu-intake.md) - exact-CID donor intake and identity-gate procedure
56. [51-documentation-audit-and-plan.md](51-documentation-audit-and-plan.md) - documentation audit, source-of-truth ordering, and ranked execution plan
54. [49-diy-sh2a-recovery-probe.md](49-diy-sh2a-recovery-probe.md) - feasibility and staged plan for a community-built SH-2A H-UDI/JTAG recovery probe
55. [50-replacement-ecu-intake.md](50-replacement-ecu-intake.md) - exact-CID donor selection, intake, and first-power-on qualification checklist
56. [51-documentation-audit-and-plan.md](51-documentation-audit-and-plan.md) - documentation truth audit and dependency-ordered execution plan
57. [52-immobilizer-swap-integration.md](52-immobilizer-swap-integration.md) - owner-authorized OEM-ECU engine swaps, chassis gateways, and replacement-firmware contracts
58. [53-lf9-decompilation-baseline.md](53-lf9-decompilation-baseline.md) - exact-source hashes and function-index coverage for LF9C102P, LF9D012H, LF9G003T, and LF9L000E
59. [54-lf9-subsystem-seams.md](54-lf9-subsystem-seams.md) - direct-call subsystem evidence, cross-CID seam parity, and the first promoted semantic candidate
60. [55-va-sti-dccd-integration.md](55-va-sti-dccd-integration.md) - passive-CAN and bench research plan for OEM VA STI DCCD, combination-meter, and WRX integration
61. [56-findings-rollup-2026-08-21.md](56-findings-rollup-2026-08-21.md) - canonical corpus repairs, recovered SH-2A/VB checksum contracts, recovery corrections, definition hazards, and their product work queue

## Other docs (not part of the numbered sequence)

- [getting-started.md](getting-started.md) — 5-minute path from a fresh clone to a first tune edit: build presets, where the binaries land, the demo project / welcome-panel flow, picking the first table to touch. Pairs with `install.md` and `00-overview.md`.
- [install.md](install.md) — User-facing guide for installing definition packs (`%APPDATA%\SubuwuTuner\definitions\` on Windows etc.); covers single-file vs directory pack layouts, where to drop packs generated by `tools/defgen/`. Linked from the repo-root README.
- [analyst-mode-prompt.md](analyst-mode-prompt.md) — Kickoff prompt for Claude analyst-mode sessions (fact extraction from protected references into specs under `SubuwuTuner-specs/`). Enforces output isolation per the clean-room methodology in `15-clean-room-engineering.md`.

## Current status (2026-08-21)

The LF79002P bench ECU is silent after power-cycle. The former E2-Lite
recovery purchase is withdrawn; exact-board probe support is unvalidated.
The immediate hardware path is a qualified recovery service or an exact-CID
replacement donor. Hardware-independent work continues through the readiness,
checkpoint, preflight, compare, restore, Atlas, reverse-engineering, and Map
Explorer tracks. The GUI now binds approved exact-CID calibration regions into
offline Flash Review while keeping live observed-CID proof as a hard blocker;
the Tables sidebar has category and safety/emissions Map Explorer facets.
See [47-task-list.md](47-task-list.md) and
[50-replacement-ecu-intake.md](50-replacement-ecu-intake.md). The 2026-08-21
findings sweep additionally recovered the VB/RH850 little-endian `0x5AA5`
checksum window, confirmed the shared `0xB6` Feistel path across SH-2A and
RH850, repaired the corpus inventory, and identified an unsafe VA Wastegate
Duty dimension mismatch. See
[56-findings-rollup-2026-08-21.md](56-findings-rollup-2026-08-21.md).

## Historical status snapshot (2026-06-19)

**Phases 0–4 done hardware-free; Phase 5 design + IR + SH-2A codegen + RH850 codegen (22 primitives, SH-2A parity) shipped; AI Tier 1 drift classifier shipped; bench validation + patch-insertion layer are the open gates before v1.0** (as of 2026-06-19). Live-side shape: `st::core`, `st::Rom` (read + write), `st::Definition` (loader with `extends` chains, typed reads + writes, scaling + inverse, 1D/2D/3D axis/table extraction, diff, writeback), `st::edit` (Rect-scoped ops + Snapshot + History undo/redo with branching), `st::Project` (`.stune` directory persistence with persisted edit history, multi-ROM with per-ROM `histories/<id>.toml`), `st::transport` (ITransport + MockTransport + J2534 + OBDX + native handheld codecs), `st::ecu::ssm` + `st::ecu::uds` (complete UDS catalog: RDBI/WDBI/RMBA/WMBA/SA/CommControl/DSC/ECUReset/TP/RoutineControl/Download/Transfer/Exit + Mode 0x09 vehicle-info), `st::ecu::subaru_security` (SecurityAccess variants, CLI-selectable via `--sa-variant`), `st::ecu::bulk_reflash` (gated 0xB6, off by default), `st::flash` (orchestrator with delta detection, per-sector erase/write/verify, dry-run, manifest, journal-based resume, optional 0xB6 path, `PolicyDenied` gate, audit-log subscriber seam), `st::log` (LogStream + LogSession + CsvSink), `st::can` + `st::dbc` + `st::discover` (full replay-path CAN RE toolkit + `.cdb` bundle), `st::autotune` (MAF + knock-pull kernels + CSV reader + smoothing + lint), `st::feature::Graph` + `st::feature::ir` + `st::feature_codegen::Sh2aBackend` + `Rh850Backend` (both at 22 primitives, fan-out dedup, FPU bridge / RH850 mask-merge select, address-gate, hardcoded `flex_fuel_scale` curve), `st::ai::drift` (Tier 1 rules-based classifier), `st::audit` (CRC32-protected append-only log with subscriber seam wired across UDS + Flasher), `st::profile` (vehicle-profile registry), `st::diff`, `st::policy`, `st::config`, `tools/defgen`. CLI: 70+ subcommands incl. `rom-{info,pull,diff}`, `dump-axis`, `dump-table`, `table-edit`, `project-*`, `pack-{info,lint}`, `log`, `flash`, `feature-compile`, `can-*`, `transport-list`, `doctor`, `checksum-{verify,repair}`, `ssm-a8-poll`, `ai-drift`, `knock-snapshot --json/--csv`, `coldstart-analyze --json/--csv`, `audit stats`, `--sa-variant`, `--enable-bulk-reflash-cipher`. GUI: docking, Dark/Light + purple accent themes (with side-by-side swatch picker in Settings), ImPlot heatmap, 3D slice picker, autotune modals, Flash modal with policy gate, Stats panel, Settings dialog (with Validate-pack button), audit panel (pin/star sidecar, bulk pin/unpin, NDJSON export with pinned-only scope), sidebar drag-reorder + right-click reset, adaptive-history panel with inline drift diagnosis, custom-features designer canvas. **~1700 C++ + 36 Python tests green** locally on MinGW g++ 15.2; CI: Win MSVC / macOS Apple Clang / Linux GCC / Linux Clang ASan+UBSan + defgen tests + defgen-freeze matrix (PyInstaller single-file binary per OS). Remaining work concentrates on RH850 bench-validation against a real ECU and the patch-insertion layer (OBDX Pro VX in hand 2026-05-24; bench-rig Phase 2 boot validated 2026-06-07; Phase 5 stop-point cleared 2026-06-15). **Subaru DIT flash-protocol arc** (rounds 14–59, June 2026): Phase C unblock via `--burst-write-extended`, full Phase D + erase + B6×N + 37 + B7 + commit chain via `subaru-dsc-unblock-sequence --write-cycle`, NRC 0x78 swallow contract documented (`docs/13`), commit gate identified as the u16 BE sum sentinel `0x5AA5` over `[0x6000, 0x200000)` (round-58 T1#1 — verified bit-exact against Fehr decat tune), FCU silent-drop catalog mapped (`[0, 0x8000)` FACI-locked, `>=0x200000` WDT trap), boot integrity decoded (3 flash sigs + 56-byte RAM hash with battery-backed markers), CID portability confirmed across LF79002P/LF79101P. Bench LF79002P bricked round-57 from cumulative real-flash drift (B7 staging-buffer ≠ real flash — see README §"What We Learned by Killing Our First ECU"); JTAG recovery procedure documented (`docs/43-jtag-recovery.md`), Renesas E2-Lite on order. Atlas tune-export pipeline `st::tune_export` shipped (`src/tune_export/`, `docs/44-tune-export.md`, 18 unit tests) — sum-preserving cal writes with skip-unchanged-blocks plan emission, ready to ship to the user's actual car once bench rig 2 validates end-to-end post-JTAG-restore.
