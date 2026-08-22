# Handoff — 2026-08-21 bedtime

## Session outcome

Work stayed hardware-independent because the bench ECU remains unavailable.
The application now has a substantially stronger ROM-save integrity path and a
usable, autonomously exercised Custom Features Designer workflow.

### ROM editing and project integrity

- `Project::save_working_rom()` and `save_all()` perform read-after-write
  verification by reopening the project.
- Verification compares source/working/additional ROM bytes, canonical edit
  histories (not only record counts), active metadata, definition identity,
  and checkpoint integrity state.
- The GUI remains dirty after failure, reports **Saved and verified** after
  success, and shows saved-state verification in Project Readiness.
- Regression coverage includes external `working.bin` drift and an on-disk
  edit record modified without changing history size/cursor.
- An isolated demo-project edit changed one cell from 50.0 to 51.0, produced
  exactly one diff row, and passed all eight `project-validate` checks after
  save/reopen.

### Custom Features Designer

- Added document path and dirty-state tracking. Node creation, movement,
  wiring, defaults, edge/node deletion, and other graph mutations mark the
  document dirty.
- Save/load dialogs default to `<project>/features`. Save repeats directly
  after the first path is established. Save/load failures and successes use a
  common toast/error path.
- Clear and Load both protect dirty graphs with explicit Keep editing / Discard
  choices.
- Added `feature::save_file` / `feature::load_file` and deterministic coverage
  for exact canonical round trips, missing files, and malformed documents.
- Added a real in-canvas compile preview:
  - graph validation and IR lowering;
  - definition-backed SH-2A/RH850 selection;
  - transient `PatchObject` compilation;
  - IR/hook/code/RAM metrics;
  - writable-region address gate;
  - expandable IR;
  - stale-preview detection;
  - explicit caution when no writable hook patch is emitted.
- Backend selection now recognizes canonical dotted platform ids such as
  `subaru.va.wrx.mt` and `subaru.vb.wrx.fa24dit` by exact path token. It does
  not use loose substring matching (`subaru.vanguard` remains unsupported).
- Patch insertion and flashing remain absent from the Designer preview and
  hardware-gated everywhere else.

## Autonomous GUI evidence

`tools/gui-visual-smoke.ps1 -Scenario Designer` now covers thirteen visual
checkpoints:

1. empty designer;
2. insert palette;
3. pack-backed hook node;
4. hook + primitive;
5. compatible wire;
6. pin-default popup;
7. committed default and lint transition;
8. node context menu;
9. edge context menu;
10. validation status;
11. compile preview;
12. dirty Load confirmation;
13. dirty Clear confirmation.

The latest successful evidence is under
`.cache/ui-smoke-designer-compile-fixed/`. It shows `sh2a`, three IR
instructions, zero hook/code/RAM output for the intentionally incomplete
Engine RPM → Add graph, and the address-gate result. The GUI was responsive and
stderr was empty.

The harness uses physical desktop coordinates, so a missed palette click can
shift later checkpoints. Screenshot sanity alone is not semantic proof. Core
graph/file/codegen tests remain the deterministic oracle; visual inspection is
the layout and interaction layer. A future harness should expose semantic
test ids or a deterministic designer fixture to reduce coordinate dependence.

## Verification at handoff

- Coherent Windows clang build: passed.
- `[feature],[feature_codegen]`: **856 assertions in 170 test cases**, passed.
- Backend canonical-platform selection: **90 assertions in 13 test cases**,
  passed during the focused run.
- Earlier combined project/feature run: **493 assertions in 101 test cases**,
  passed before the compile-preview addition.
- Headless GUI startup against `fixtures/demo.stune`: passed.
- Live Designer scenario: thirteen screenshots, responsive, clean stderr.
- `git diff --check`: passed except pre-existing CRLF normalization warnings
  for two definition files.

## Exact next queue

1. Add a checked-in, pack-compatible patch-producing `.stmod` fixture and use
   it to show non-zero hook/code/RAM preview output.
2. Add visual and deterministic failure coverage for lowering, unsupported
   operations, missing free RAM, and writable-region address-gate refusal.
3. Run the equivalent preview against a canonical `subaru.vb.*` fixture and
   keep the RH850 hardware-unverified caveat visible.
4. Automate Save → Clear → Load → canonical graph comparison without relying
   entirely on native-dialog coordinates.
5. Turn lint/compiler findings into selectable entries that navigate to the
   implicated node/pin.
6. Add graph undo/redo and keyboard-accessible node, edge, connection,
   deletion, and property editing.
7. Return to the ROM queue: isolated GUI cell edit → undo/redo → save → close/
   reopen, checksum-state reporting, then CSV/range/interpolate/smooth
   round-trip coverage.

## Safety and worktree notes

- Do not infer hardware readiness from compiler, insertion, or mock/synthetic
  tests. The bench ECU state and real recovery path remain the governing gate.
- The repository was already heavily dirty. Existing changes belong to the
  user/other work; do not reset or broadly reformat them.
- The coherent toolchain launcher is required. An ambient shell can mix the
  Visual Studio 18 preview STL with clang 19 and fail `STL1000`; use
  `tools/windows-build.ps1 -Compiler clang -BuildDirectory
  build/win-clang-coherent`.
- Ninja repeatedly prints `premature end of file; recovering` and rebuilds a
  large surface, but coherent builds complete successfully. Investigate the
  incremental-build metadata separately rather than bypassing the launcher.

