# Changelog

All notable changes to SubuwuTuner are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/).

Pre-1.0 entries describe substantial state changes rather than strict semver releases. Versioning hardens at v1.0.

## [Unreleased]

### Added
- `st::flash::BackupStore` — mandatory pre-write full-ROM backup with CRC32 verify gate. Flash refuses to proceed without a verified backup. Retention policy: keep last 10 + all pinned. (See `docs/05-improvements.md` §4.)
- `st::policy::FlashPreflight` — composable pre-flight validator pipeline. Built-in validators: `EcuIdMatch`, `VinMatch`, `BatteryVoltageOk`, `IgnitionState`, `ChecksumKnown`, `BackupStorePresent`. Pipeline returns a `PreflightReport` with explicit `blockers`/`warnings`. Surface in flash modal + CLI.
- Plugin / extension interface seams under `st/core/ext/` (header-only). Stable abstract interfaces for `ChecksumStrategy`, `TransportDriver`, `KernelDescriptor`, `Validator`, `LogChannelSource`, `RomFormat`, `DefinitionSource`, `UnitConverter`, `ActionHandler`, `HelpTopic`. No dynamic loading yet — interfaces only. (See `docs/02-architecture.md` and the recommendation in `Findings/08_recommended_improvements.md#I-12`.)
- `CHANGELOG.md` (this file).
- `SECURITY.md` — responsible-disclosure policy and scope.
- `THIRD-PARTY-INSPIRATIONS.md` — clean-room inspiration trail.

### Tooling
- `.pre-commit-config.yaml` — local mirror of the CI `clang-format (required)` lane, plus trailing-whitespace, end-of-file-fixer, yaml/toml/merge-conflict checks. Install: `pip install --user pre-commit && pre-commit install`. Stops format drift at commit time before CI.
- CI smoke now exercises `pack-info --json fixtures/demo-pack/pack.toml` on every matrix lane — guards the demo pack against drift + exercises the `subuwutuner.pack-info.v1` JSON emitter.

### Documentation
- External multi-file design + gap analysis under `D:\Subuwu\findings\` (`00_reference_scan.md` through `12_final_summary.md`). Not part of the public repo; informs the v1.0 roadmap.
- `docs/33-analyst-review-triage-2026-06-01.md` — triage of the external analyst review. Identifies what's already shipped vs real gaps, flags factual errors in the review, sequences forward-looking recommendations. Companion to the staged review at `fixtures/private/findings_reviews_2026-06-01/`.

## Pre-Unreleased (historical snapshot — 2026-05-30)

This entry summarizes the state of the project at the point this CHANGELOG was introduced. Earlier history lives in `git log`. From here on, every user-visible change should add an entry under `[Unreleased]`.

### Phase 0–4 (hardware-free)
- Core: `st::Result<T>`, CRC32, CSV, error codes, version.
- ROM container, definition pack loader (TOML), `edit::History` with undo/redo.
- `.stune` project format.
- Transport: OBDX Pro VX, native handheld, J2534 (Windows), mock.
- ECU protocols: SSM + full UDS catalog including flashing flow + OBD-II Mode 0x09.
- Datalogger end-to-end via `MockTransport`.
- Flash orchestrator end-to-end hardware-free including delta detection, per-sector erase/write/verify, dry-run, manifest, journal-based resume.
- SecurityAccess: factory SSMCAN1 (Gen-A 16-round Feistel) + COBB-AP / Fehr-active L1+L3 variants in tree, CLI-selectable via `--sa-variant`.

### v1.1 surface (live alongside v1.0)
- Auto-tune kernels: MAF + knock-pull with CSV readers, smoothing, lint, CLI, project integration via `edit::History`.

### Phase 5 (custom features)
- `st::feature::Graph` + `st::feature::ir`: lower, dump, lint, RT-budget cost.
- SH-2A codegen for VA shipped (21 primitives; designer canvas; sample graphs in `fixtures/samples/`).
- RH850 codegen for VB at SH-2A parity (22 primitives across int/float arithmetic, compares, bool logic, branchless select, nested CallPrimitive operands).

### Tooling
- Optional gated 0xB6 bulk-transfer write path behind two-step arming (`ST_ENABLE_BULK_REFLASH_CIPHER` build flag + `--enable-bulk-reflash-cipher` runtime flag, both default off in public builds).
- CAN reverse-engineering toolkit replay-path: `Frame` + `.asc` I/O, DBC parser/emitter/decoder, `BaselineModel` + `ChangeDetector`, `.cdb` bundle, five CLI subcommands.
- `subuwutuner-cli ssm-a8-poll` for live OEM SSM-0xA8 RAM polling over ISO-15765 with offline correlator `tools/cross_ref_ssm_a8.py`.

### GUI
- Dear ImGui v1.91 + GLFW 3.4 + ImPlot v0.16 + nativefiledialog-extended.
- Docking workspace, Dark/Light themes with purple accent `(0.55, 0.35, 0.85)`.
- Autotune modals, Flash… modal with policy gate, Stats panel, status-bar profile chip, jurisdiction-profile persistence, Settings dialog.

### Open gate to v1.0
- Bench-validation against a real ECU per `docs/28-bench-rig-build.md` and `docs/08` Tier 4.

---

## Format

Each `[Unreleased]` section uses these sub-headings (omit empty ones):

```
### Added
### Changed
### Deprecated
### Removed
### Fixed
### Security
### Documentation
```

When cutting a release, rename `[Unreleased]` to the new version with the date, e.g. `[1.0.0] - 2026-09-15`, and start a new empty `[Unreleased]` block.
