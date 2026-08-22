# 53 - Session summary, 2026-08-22

A single long working session that landed the offline application queue, a
toolchain repair, several findings-derived features, and the datalog analysis
-> tune -> auto-tune loop. All work is committed to `main`, built clean under
MinGW GCC-15 `-Werror`, and test/visually verified.

## What shipped (commits, newest last)

1. `929583e` was HEAD at session start (Aug 2); everything below is new.
2. **Offline app queue + toolchain** (`9147e2c`): GUI edit round-trip smoke
   scenario; Project Readiness verification report (summary tally + remediation
   actions); flash-ready checksum (`st::tune_export::checksum_state` /
   `flash_checksum` / idempotent `repair_balance`) distinct from project
   integrity; table CSV/percent/interpolate/smooth round-trips; edit->undo/redo
   ->save->reopen and per-ROM-history and write_typed quantization invariants.
3. **Axis-length fix + dead-weight collapse** (`6d6b7bc`): `defgen
   --axis-corrections` overrides the halved VA axis dimensions from a bundled
   firmware-verified TSV; `Atlas.factory_immutable` flag + a sidebar default-
   hide of never-tuned tables.
4. **Log analysis** (`5537f8e`): `st::library::log_analysis` -> plain-language
   knock / DAM / boost / AFR findings in the Log Explorer + Markdown export.
5. **Designer undo/redo** (`9bea8fa`): diff-based graph snapshots; toolbar
   buttons + Ctrl+Z / Ctrl+Y.
6. **Jump-to-sample** (`a97dae2`) and **clickable validation findings**
   (`cb36b35`): findings navigate to the log sample / offending graph node.
7. **Log -> tune navigation** (`98d4d9e`): "Open in Tune" on a finding selects
   the calibration table and highlights the cell at the finding's rpm/load.
8. **Log -> auto-tune seeding** (`4a978b3`) + **use-after-move fix**
   (`e79995a`): "Suggest timing pull from this log" seeds the knock-pull modal;
   debugging it surfaced a real use-after-move that zeroed the ledger axis
   labels. End-to-end the demo proposes a -0.75 deg pull at the knock cell.
9. **Broadened log analysis for real AP logs** (this doc's commit): real COBB
   AccessPort column names + fuel-trim and injector-duty findings.

## Current tuning loop

read ROM -> verify (integrity + flash-ready checksum) -> datalog ->
auto-analysis (knock / DAM / boost / AFR / fuel-trim / injector-duty) -> jump
to the offending cell -> seed auto-tune -> concrete timing-pull proposal ->
review / apply. Custom features have undo/redo and actionable lint.

## Toolchain note (important)

- `build/win-mingw` (GCC 15.2) is the working local toolchain; builds clean
  under `-Werror`.
- `build/win-clang-coherent` is NOT broken but must be built through
  `tools/windows-build.ps1` (pins VS2022 14.44); a bare `cmake --build` lets
  clang-cl grab the newly-installed VS18 MSVC 14.51 STL, which clang-cl 19.x
  can't parse. See `docs/47` item 7.

## Demo fixtures gained this session

- `fixtures/demo-pack/tables/ignition.toml` + `timing_deg` scaling (for the
  log->tune navigation and the knock-pull demo).
- `fixtures/demo-knock-log.csv` gained a cell of sustained knock so the
  auto-tune proposal is non-zero end-to-end.

## Next candidates (open)

- Broaden log analysis further (over-temp needs unit detection; closed-loop vs
  open-loop fueling context).
- Table-lookup primitive for custom features (real capability gap; needs SH-2A
  + RH850 interpolation codegen).
- GUI live-tuning consumer for the 422-DID WDBI catalog (CLI already has it).
- The `docs/52` remaining items (axis orientation, tune-library dedup).

See `docs/52-findings-followup.md` for the findings-derived feature status and
`docs/47-task-list.md` for the master queue.
