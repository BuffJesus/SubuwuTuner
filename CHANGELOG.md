# Changelog

All notable changes to SubuwuTuner are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/).

Pre-1.0 entries describe substantial state changes rather than strict semver releases. Versioning hardens at v1.0.

## [Unreleased]

### Added
- **Architectural classifier** — `Layer` + `LayerMap` + `classify_patches()` + `summarize()` + `layer_label()` per `docs/35-tuner-overlay-architecture.md`. Most-specific (smallest containing range) match — needed because the spec's range list has intentional overlap (`PrimaryCode` and `SecondaryCode` sit inside `MainCalibration`).
- **History panel — collapsible tag groups** — runs of consecutive edits sharing the same non-empty `tag` (e.g. all 4 edits from a FA24-swap workflow tagged `fa24_swap`) collapse to a single chevron header by default once they exceed 10 edits. Header shows aggregate flags (S/E), tag name, count, and a Jump-to-end-of-run action. Click the chevron to expand; a "Collapse expanded groups" button at the bottom re-folds all in one click.
- **Settings modal — Environment tab** — new tab surfacing env-var-driven features with set/unset status + purpose strings, plus build-time feature flags with ON/OFF state. Pure discovery surface — read-only, no edit affordances (env vars come from the process environment, which Settings can't write back without restart).
- **F1 context routing — Welcome + History panels wired** — Welcome was in the HelpContext enum but no panel was setting it; History had no enum value at all. Welcome now routes to docs/00-overview; History routes to docs/21-stune-format (covers the edits.toml shape that drives the history table).
- **Shortcuts modal — F1 description sharpened** — F1 row now describes its context-aware routing (lands on the topic relevant to the focused panel) instead of just "open help."
- **F1 context routing — Stats / DTCs / Gauge Cluster panels wired** — three more panels gained context-aware F1 routing (Stats → docs/11-definition-format, DTCs → docs/06-legal-ethics, Gauge Cluster → docs/32-live-datalogger). HelpContext enum extended; pattern now covers every non-render-only panel.
- **docs/README.md status paragraph refreshed** — "as of 2026-06-04" → "as of 2026-06-13"; test count and live-side shape updated.
- **`src/README.md`** — module-by-module orientation: 25 modules grouped by tier (foundations / ECU + transport / datalog + CAN RE / flash + features / front-ends), one-line purpose per module, cross-doc pointers, layer rules from docs/02, scaffolding recipe for adding a new module. Onboarding aid; reduces the "where does X live?" cliff for new contributors and future Claude sessions.
- **`docs/getting-started.md`** — public-repo onboarding guide. 5-minute path from `git clone` to first tune edit: build → demo project → first edit. Sits alongside `install.md`. Covers GUI surface (F1 context routing, Ctrl+K palette, status-bar chips) + CLI surface + common first-time questions.
- **GUI: Tools → Run Diagnostics…** — new modal (`src/ui/src/modals/doctor.cpp`) surfacing install-health checks visually. Re-implements the CLI `doctor` command's checks directly against the domain-layer API (build identity, compiler, user config load, definitions-root existence + pack count, ROM-dump-root). Traffic-light per row + actionable hint when something's off + overall status chip + Refresh button. Closes the "I installed but something's wrong" support cliff for users who don't open a terminal.
- **`CONTRIBUTING.md`** — public-repo contributor guide. Quick start, repo layout, code style + comment style, the IP rules (clean-room methodology + analyst/implementer wall — protects future contributors from accidentally GPL-contaminating the codebase by lifting third-party source), testing conventions, PR flow, communication.
- **`.github/PULL_REQUEST_TEMPLATE.md` + `.github/ISSUE_TEMPLATE/{bug,feature}.md`** — GitHub-standard templates prompting for the right info on every PR/issue. Bug template asks for `doctor` output + build flag state + hardware in use; feature template asks for the user-visible benefit + scope-trade-off checklist.
- **`docs/04-roadmap.md` — "Sequencing the open v1.1 items" section** — post-2026-06-13 planning subsection. Identifies the remaining open v1.1 items, groups by shared blockers, proposes order-of-operations.
- **`docs/42-bench-rig-validation-runbook.md`** — picks up where `docs/28` ends. Sequences the hardware-side validation that retires the open bench-rig-gated items: full-ROM read-back → boot-signature verify → SA against rig → flash protocol sequence capture → first deliberate write → power-loss inject → delta-flash six-state validation → patch-insertion validation. Concrete pass criteria + failure-mode tracking per step.
- **`tools/library_inventory/inventory.py`** — standalone Python script that walks local tune-file collections, MD5-hashes each file, parses vendor / stage / fuel / variant from filenames, dedups by content hash, emits `library_index.toml` + a human-readable `library_summary.md`. Pre-stages the in-tool tune-library feature without touching C++. Companion `tools/library_inventory/README.md` documents the output format.
- **`tools/library_inventory/` — `--query <md5>` mode** — lookup-by-hash flag. Tab-separated single-line output on match; exit 0 / 1 / 2 conventions. Tolerates hex-prefixed or wrapped-string input. Pipeline-friendly.
- **`st::library::TuneIndex` + Tune Library panel** — C++ side of the library cross-reference work. Header at `src/library/include/st/library/tune_index.hpp`, impl + tests + new GUI panel at `src/ui/src/panels/library.cpp`. Reads `library_index.toml` (produced by `tools/library_inventory/inventory.py`), surfaces vendor / stage / variant / size / MD5 per tune in a sortable table, includes vendor-facet dropdown + substring filter + re-scan button. Tools → View → Tune Library menu entry + `Ctrl+K → Show Tune Library` palette entry. 10 new test cases covering schema validation, MD5 case-insensitivity, substring filter, malformed-entry skip.
- **Tune Library panel — right-click per-row context menu** — verbs: **Diff against…** (two-step: first click sets diff source, accent chip surfaces the picking state), **Reveal in file browser** (path validated via `st::validate_shell_safe_path` before shelling out to `explorer /select` / `open -R` / `xdg-open`), **Copy MD5** (clipboard + toast confirmation). State persisted in new AppState fields.
- **Tune Library panel — Full rescan button** — subprocess to `tools/library_inventory/inventory.py` via `std::system`. Discovery: `ST_LIBRARY_INVENTORY_PYTHON` env override, else cwd-upward walk. Aims `--out-dir` at the existing index's directory (or cwd). Graceful fallback to "Re-read TOML" when python isn't on PATH or the script can't be found. Toast on completion.
- **Status bar — Library chip** — surfaces `library_status_snapshot()` when the Tune Library panel has loaded an index: "Library: N" with tune count. Tooltip breaks down counts.
- **`panels.hpp` — new `LibraryStatusSnapshot` struct + `library_status_snapshot()` accessor.** Pattern matches the existing snapshot accessors.
- **`st::config::Paths::library_root`** — third user-configurable path alongside `definitions_root` and `rom_dump_root`. Default: `<user-docs>/SubuwuTuner/library/` (Win) / `$XDG_DATA_HOME/SubuwuTuner/library/` (POSIX). Persists in `[paths]` of `config.toml`; forward-compatible (old configs lacking the field fall back to the default). Resolved by `st::config::library_root()` via the established override > `ST_LIBRARY_ROOT` env > config > default chain. Settings dialog wiring is a follow-up; today the field is set via `subuwutuner-cli config set paths.library_root <path>` or by hand-editing `config.toml`.
- **`tests/README.md`** — top-level test-tree orientation: directory layout (`_helpers / unit / hil / private / fuzz`), tag conventions (`[.live]`, `[.private]`, `[!shouldfail]`), Windows + MinGW ASCII-only TEST_CASE convention, how to add a test.
- **Welcome panel — dead-recent ✕ remove button** — recent projects whose path no longer exists (moved/deleted projects) now show a ✕ remove-from-recents affordance. Kb-Enter and click dispatch both defensively re-check path existence before firing `try_open_project` — prevents UI freeze when a recent's drive disconnected.
- **`docs/40-delta-flash-brick-protection.md`** — per-ISA brick-protection spec for the v1.5 delta-flash workflow. Extends `docs/31` with a six-state SH-2A recovery enumeration (mid-erase / mid-write of signature vs non-signature sectors; between-sectors; mid-verify), journal-extension format for power-loss resume, four additional bench-rig tests on top of `docs/31`'s five.
- **FA24 swap workflow modal** (`src/ui/src/modals/fa24_swap.cpp`) — guided 3-step recipe for the FA20 → FA24 engine swap into a VA WRX. Steps: cam strategy (Keep FA24 / Swap FA20 cams / RS Motors kit), basemap import (.bin file picker via NFD + size sanity-check), review changes. Applies Engine Displacement / HPFP Base Offset / AVCS Intake Cam Target / Injector Mult Table as four `apply_op_table` writes tagged `fa24_swap`. Reachable from the welcome panel's "Common workflows" card and from Tools → Common Workflows → FA24 swap.
- **`st::edit::Edit::tag` + `History::undo_while_tag`** — optional transaction tag on edits so workflow modals (FA24 swap today; future Stage1→2 step, E85 conversion) can record their writes as a tagged batch and undo them as a single unit via the status-bar badge's "Revert All". Forward-compatible serialization to edits.toml — old readers ignore unknown keys, no schema bump.
- **`apply_op_table(state, label, tag, table_id, op)` helper** (in `src/ui/src/actions.hpp`) — sibling of `apply_op` for "I have a table id but no user selection." Reads the named table from the active ROM, runs the op over the whole rect, writes back, records with an optional transaction tag. Lets workflow modals write multiple tables without forcing the user to manually select each one first.
- **`[[workflow]]` pack-TOML registry** — first-class support for pack-declared multi-table workflows. Each entry has `id` / `display_name` / `modal` / `required_tables`. `Definition::workflows()` / `find_workflow(id)` / `supports_workflow(id)` drive UI eligibility gating without code changes. Extends-aware: child packs inherit + override by id. `pack-info` CLI surfaces the declared workflows with a "(missing required_tables — not eligible)" note when applicable. (See `docs/16-custom-features.md`.)
- **Sidebar "Common workflows" card** on the welcome panel — discovery surface listing pack-declared workflows; disabled-with-tooltip when the loaded pack lacks coverage.
- **Tools menu → Common Workflows submenu** — enumerates available workflows for the loaded pack; enabled-state gated on `pack_supports_*`.
- **Status-bar FA24-swap badge** — persistent purple-accent chip when the active history carries at least one `fa24_swap`-tagged edit. Click → popup lists the recorded edits + a "Revert All" button calling `revert_fa24_swap` (which walks `History::undo_while_tag` for the batch).
- **Sidebar "Collapse all" affordance** — right-aligned link-styled button under the filter input. One-shot flag closes every TreeNode for one frame; imgui.ini-persisted user choices take over again on the next click.
- **`subuwutuner-cli workflow-list <DEF>`** — enumerates pack-declared workflows with per-table presence check + `--id` filter + `--eligible` filter + `--json` emitter (schema `subuwutuner.workflow-list.v1`). Exit 1 on any ineligible workflow when no `--id` filter — fits a CI gate that asserts a pack's declared workflows still resolve all required tables after a regen.
- **`pack-info --json` workflows surface** — adds `counts.workflows` + a `workflows[]` array with `id` / `display_name` / `modal` / `required_tables` / `eligible`. Schema major stays at v1 — additive optional fields only.
- **FA24 swap modal: basemap CID check at file pick** — after the size check, identifies the picked basemap via `Definition::matches()` and compares its CID against the project's source CID. Three colored outcomes under the "Yes" radio (green match / yellow mismatch or unrecognized / red load failure). Catches the most common wrong-firmware-family basemap before Apply instead of failing mid-table-copy with a harder-to-read error.
- **Ctrl+K command palette → "FA24 swap (VA WRX)…"** — discovery path for the workflow under a new "Workflows" category. Only listed when pack qualifies.
- **Sidebar auto-opens + scrolls to selected table on cross-reference jump** — when a table is selected from anywhere other than the sidebar itself (command palette, compare panel, history panel, jump_to_table), the sidebar force-opens the containing top + leaf group AND scrolls the row into view. One-shot flag, clears at end-of-frame so users can collapse again.
- `st::flash::BackupStore` — mandatory pre-write full-ROM backup with CRC32 verify gate. Flash refuses to proceed without a verified backup. Retention policy: keep last 10 + all pinned. (See `docs/05-improvements.md` §4.)
- `st::policy::FlashPreflight` — composable pre-flight validator pipeline. Built-in validators: `EcuIdMatch`, `VinMatch`, `BatteryVoltageOk`, `IgnitionState`, `ChecksumKnown`, `BackupStorePresent`. Pipeline returns a `PreflightReport` with explicit `blockers`/`warnings`. Surface in flash modal + CLI.
- Plugin / extension interface seams under `st/core/ext/` (header-only). Stable abstract interfaces for `ChecksumStrategy`, `TransportDriver`, `KernelDescriptor`, `Validator`, `LogChannelSource`, `RomFormat`, `DefinitionSource`, `UnitConverter`, `ActionHandler`, `HelpTopic`. No dynamic loading yet — interfaces only. (See `docs/02-architecture.md`.)
- `CHANGELOG.md` (this file).
- `SECURITY.md` — responsible-disclosure policy and scope.
- `THIRD-PARTY-INSPIRATIONS.md` — clean-room inspiration trail.

### Changed
- **`apply_fa24_swap` refactored to a data-driven loop** — the 4 hardcoded `apply_op_table` calls (each duplicated across basemap-vs-defaults branches, ~60 lines) collapse to a single pass over a `kWorkflowTables` descriptor array. Adding a 5th edit is now a one-line append.
- **First-run wizard rewrite** — Welcome / Units / Theme / Demo steps rewritten in the design philosophy set by yesterday's jurisdiction rewrite. Lead with what the choice does in plain language, "Not sure?" reassurance line, concrete use-case blurbs over implementation-detail blurbs. Imperial radio disabled-with-tooltip ("coming in a follow-up release") since no conversion layer is wired yet. Esc wired to the skip flow with a `skip_wizard` lambda deduplicating the body across window-X / Skip button / Esc.
- **Sidebar table list now hierarchical** — pack categories like `fuel - injectors - pulse` split on the first ` - ` into a top-level group (`fuel`) and a sub-group label (`injectors - pulse`). Top-level and leaf-level both default-CLOSED so a fresh project shows ~9 group headers instead of 91 flat folders. Per-row MDL2 grid icon + per-header folder icon dropped (the indent + arrow already convey hierarchy). S/E policy badges still right-aligned. Glossary hover works at both levels.
- **FA24-swap pack-support check** is now purely pack-declared — the transitional hardcoded table-presence fallback that was falsely qualifying packs by table-name presence alone is dropped. Future packs opt in by adding `[[workflow]]` to their TOML.
- **`definitions/impreza/lf79101p.toml`** now declares `extends = "lf79103p"` so the child pack inherits the LF79103P table catalog (same firmware family; CID descriptor at 0x37C51 is the only difference). `checksum_type` restated explicitly so `merge_over` doesn't drop family-specific fields.

### Fixed
- **`Definition::from_file` now resolves `extends`** — the single-file loader (used by `Project::open` for the project's `def_path`) silently skipped extends resolution, so packs relying on inheritance loaded zero inherited tables at runtime and workflow guards returned false against valid project state. Adds `find_sibling_pack_file` (flat-file counterpart to `find_sibling_pack_dir`) with cycle-protected recursion. `pack-info lf79101p.toml` now reports 293 tables (inherited) instead of 0.

### Tooling
- `.pre-commit-config.yaml` — local mirror of the CI `clang-format (required)` lane, plus trailing-whitespace, end-of-file-fixer, yaml/toml/merge-conflict checks. Install: `pip install --user pre-commit && pre-commit install`. Stops format drift at commit time before CI.
- CI smoke now exercises `pack-info --json fixtures/demo-pack/pack.toml` on every matrix lane — guards the demo pack against drift + exercises the `subuwutuner.pack-info.v1` JSON emitter.
- **CI workflow-eligibility gate** — every matrix lane runs `workflow-list definitions/impreza/lf79103p.toml`. Exits 1 if any declared workflow has a missing required-table id, so a future pack regen that drops one of `fa24_swap`'s required tables trips CI immediately rather than silently disabling the welcome card / Tools menu entry.

### Tests
- **In-tree pack workflow eligibility tests** under `[in_tree]` — load the real `definitions/impreza/lf79103p.toml` and assert `fa24_swap` is declared with all 5 required tables present; same for `lf79101p.toml` via the `extends` chain. Both tolerate missing-file (sparse / path-B-only checkouts) via WARN + skip. Complements the CI workflow-list smoke gate by running inside ctest.
- New compile-time constant `ST_DEFINITIONS_DIR` exposed to tests (same pattern as the existing `ST_FIXTURE_DEMO_STUNE`) so future "real-pack regression" tests can build absolute paths to canonical packs.

### Definitions
- New base pack `lf9d012h.toml` (296 tables, full FA24-swap cluster coverage matching its LF9-family siblings).
- New scaffolding stubs: `va_2015_2018_base.toml`, `va_2019_2021_base.toml`, `vb_all_base.toml` — pattern follows the existing ecuparams/ shared base files.
- `[[workflow]] fa24_swap` block added to lf79103p / lf9l000e / lf9d012h (lf79101p inherits via `extends`).

### Documentation
- External multi-file design + gap analysis under `D:\Subuwu\findings\` (`00_reference_scan.md` through `12_final_summary.md`). Not part of the public repo; informs the v1.0 roadmap.
- `docs/33-analyst-review-triage-2026-06-01.md` — triage of the external analyst review. Identifies what's already shipped vs real gaps, flags factual errors in the review, sequences forward-looking recommendations. Companion to the staged review at `fixtures/private/findings_reviews_2026-06-01/`.
- `docs/16-custom-features.md` — new "FA24 swap workflow modal (shipped 2026-06-07)" and "Workflow registry pattern" subsections describe the shipped 3-step flow, the `kWorkflowTables` descriptor, and the `[[workflow]]` TOML schema + `Definition::supports_workflow()` + `apply_op_table` + `undo_while_tag` transactional batch pattern that future workflow modals (Stage1→2 step, E85 conversion) follow.
- `docs/28-bench-rig-build.md` — Phase 2 pin table gains a wire-color column (mined from FSM WI-160 / WI-161 wiring diagrams); B134-38 expected-voltage corrected from "1 V" to "~0 V (active-low — ECU sinks to engage relay)"; new disambiguation note for the two solid-Lg wires on B134 (back-up power at pin 23 vs main-relay control at pin 38). Phase 3 CAN section gains wire colors (B134-9 Light blue = CAN-Lo, B134-10 Red = CAN-Hi) with an explicit "Subaru uses non-ISO-standard colors" callout.
- `CLAUDE.md` status snapshot rolled forward through 2026-06-07 — captures the workflow modal infrastructure, hierarchical sidebar, extends bug fix, and the `workflow-list` CLI.

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
- SecurityAccess: factory SSMCAN1 (Gen-A 16-round Feistel) + additional installer-state variants in tree, CLI-selectable via `--sa-variant`.

### v1.1 surface (live alongside v1.0)
- Auto-tune kernels: MAF + knock-pull with CSV readers, smoothing, lint, CLI, project integration via `edit::History`.

### Phase 5 (custom features)
- `st::feature::Graph` + `st::feature::ir`: lower, dump, lint, RT-budget cost.
- SH-2A codegen shipped (21 primitives; designer canvas; sample graphs in `fixtures/samples/`).
- RH850 codegen at SH-2A parity (22 primitives across int/float arithmetic, compares, bool logic, branchless select, nested CallPrimitive operands).

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
