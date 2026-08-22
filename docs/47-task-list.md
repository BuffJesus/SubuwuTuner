# 47 - SubuwuTuner master task list

Updated 2026-08-21. This is the working queue for the tuning application, the
bench-rig recovery, and the offline reverse-engineering work. It is ordered by
dependency and risk, not by novelty.

## Current state

- The bench rig has been built and used, but the test ECU is currently bricked
  after a calibration-block modification.
- The safest next hardware action is boot/JTAG recovery or ECU replacement,
  followed by a controlled validation run. Do not treat repeated OBD flashing
  as a recovery plan.
- The application is already useful without a functioning ECU: ROM viewing,
  editing, projects, definitions, compare, logging pipelines, AP file-vault
  workflows, auto-tune analysis, custom-feature IR/codegen, and substantial
  protocol/flash orchestration are implemented and testable with fixtures and
  mocks.
- Offline analysis now has a canonical corpus inventory with architecture,
  source substitutions, and explicit exclusions. It also has complete LF9
  baselines, a recovered VB checksum invariant across 16 images, and repaired
  analyses for INFO-wrapped, incomplete, PowerPC, and M32R sources. See
  `docs/56-findings-rollup-2026-08-21.md`.
- Atlas material is useful as analyst-side metadata and QA input. It must not
  be promoted wholesale into the public product or used as silent authority.

### Recovery decision already made

- `[x]` The no-CAN/no-UDS-after-power-cycle state is classified as an external
  recovery state. Blind repeated OBD writes are not a viable recovery strategy
  for the current ECU.
- `[x]` `findings/decompile/VA_RECOVERY_RESEARCH.md` records the evidence and
  the software policy: preserve logs and any readable image, then stop writes.
- `[x]` Retire the E2-Lite procedure for the current SuperH/H-UDI target.
  `docs/43-jtag-recovery.md` is historical only. Prefer an exact-CID donor;
  keep SCI boot mode as an exact-board research path.
- `[x]` The flash preflight pipeline now models `Reachable`,
  `IdentifiedButWriteRejected`, and `SilentBootFailure`; the latter two are
  hard blockers with external-recovery guidance.
- `[x]` The canonical flash preflight now requires positive proof for exact ECU
  identity, definition match, source-image verification, recovery-image
  presence, and interrupted-journal safety.
- `[~]` The canonical flash preflight now checks every planned address range
  against a caller-supplied approved flash-region allow-list, including
  overflow and region-boundary failures. The distinct exact-CID
  `[[calibration_region]]` model now validates provenance, status, bounds,
  inheritance, and duplicate `(cid,name)` keys. Offline Flash Review now
  selects approved ranges from a source-ROM match while keeping the live
  observed-CID blocker visible.

## Status legend

- `[x]` complete or evidenced
- `[~]` active / partially complete
- `[ ]` queued
- `[!]` blocked by the bricked ECU or another explicit dependency

## Current application sprint - offline usability

Work this queue top-to-bottom while the bench rig is unavailable:

1. `[x]` Finish Log Explorer: unit metadata, calculated channels, session notes,
   event markers, saved selections/profiles, range statistics, and findings
   export. Units, calculated delta/ratio/percent-error channels, notes, and
   sample-index markers, durable TOML sessions, selected-range statistics,
   plotted-range CSV, and Markdown findings export are implemented.
2. `[x]` Trace imported RPM/load samples through the active table axes and show
   the nearest cell without mutating the calibration. The explorer validates
   named RPM/load axes and switches the active map to its grid selection.
3. `[x]` Finish Map Explorer field notes: purpose, provenance, confidence,
   exact-CID coverage, sibling variance, and related safety/co-edit tables.
4. `[x]` Add guided task cards for identity/baseline, fueling, boost,
   ignition/knock, cold start, FA24 preparation, and final review. Cards link
   to existing tools and persist local completion notes per project while
   keeping hardware verification explicitly separate.
5. `[x]` Turn pre-flash review into a plain-language narrative with one action
   for every blocker and a separate local-plan versus live-ECU truth state.
6. `[x]` Add keyboard navigation, accessibility labels, useful empty states,
   and fixture-driven UI smoke coverage for the primary offline workflows.
   Welcome, Tune, Log Explorer, Features, and the command palette now have
   normal/compact/minimum live coverage; keyboard workspace switching and
   Escape dismissal are exercised without dialogs or hardware.
7. `[x]` Stabilize Windows build presets and run the full suite. The MSVC build
   is clean, and `tools/windows-build.ps1` now gives clang-cl one coherent
   Visual Studio/MSVC environment through configure, build, and test.

### Next queue - application hardening

1. `[x]` Add a fixture-driven offline workflow smoke test: open the demo
   project, validate identity/definition facts, exercise log-session state,
   and confirm missing live evidence remains a flash blocker.
2. `[x]` Audit the primary panels for stable visible labels, keyboard focus,
   Escape behavior, disabled-control explanations, and useful empty states.
   The audit now covers Welcome, Tune, Log Explorer, Features, and command
   palette states at 1400x880, 800x600, and the supported 640x400 floor.
3. `[x]` Add keyboard-first actions for Log Explorer range/marker navigation
   and guided-task traversal without introducing dialog shortcuts that can be
   triggered accidentally.
4. `[x]` Move guided-task persistence into a tested library serializer and
   migrate the initial sidecar format without losing existing notes.
5. `[x]` Add user-authored Log Explorer signal profiles and persist them with
   the same explicit schema/version posture as sessions.
6. `[x]` Add a headless application-startup smoke target that initializes the
   UI state against the demo project without requiring a GPU or ECU.
   `subuwutuner-gui --smoke-test [PROJECT.stune]` now validates settings,
   bundled resources, and real project loading before GLFW is initialized;
   CTest registers it as `gui_headless_startup_smoke`.
7. `[x]` Repair or replace the clang-cl Windows preset so it cannot silently
   combine incompatible compiler and standard-library toolsets. A new
   `tools/windows-build.ps1` launcher pins configure/build/test to one detected
   Visual Studio installation/toolset. A clean VS 2022 clang-cl 19.1.5 + MSVC
   14.44 build now links the CLI, GUI, and unit-test binaries successfully.
   `[x]` 2026-08-22 toolchain drift diagnosed and resolved: a newly-installed
   VS 18 ships MSVC 14.51, whose `<type_traits>` uses
   `__builtin_is_implicit_lifetime` that clang-cl 19.x cannot parse. This is
   NOT a preset break — `tools/windows-build.ps1 -Compiler clang` still works:
   its `vswhere` clang query resolves to VS 2022 (VS 18 lacks the LLVM
   component) and it pins `-vcvars_ver=14.44`, so clang-cl uses the 14.44 STL
   (verified: a C++23 `<type_traits>` TU compiles clean under that env). The
   failure only appears when the coherent preset is built with a bare
   `cmake --build` OUTSIDE the pinned vcvars env, which lets clang-cl
   auto-select VS 18's 14.51 headers. Always build the coherent preset through
   `windows-build.ps1` (it also writes to `build/win-clang-coherent-14.44`, not
   the stale unsuffixed `build/win-clang-coherent`). Separately, the MinGW/GCC
   toolchain updated to GCC 15.2, which raised six new `-Werror` diagnostics;
   those are fixed (see below) so `build/win-mingw` builds clean under
   `-Werror` as well.
8. `[x]` Establish a repeatable live GUI inspection loop. The Windows-only
   `tools/gui-visual-smoke.ps1` launcher opens the demo through the real GUI,
   visits Tune, Log, and Feature workspaces, captures OpenGL-safe screenshots,
   checks process responsiveness, and preserves stdout/stderr for review. Its
   `-Scenario Welcome` path separately covers the no-project entry experience.

### Next queue - iterative GUI usability

1. `[x]` Exercise Welcome and Demo workflows at normal, compact, and minimum
   supported window sizes; fix clipping, unreachable actions, and misleading
   empty states found by live screenshot inspection. Narrow windows now switch
   to compact dock trees that preserve the active workspace and tab secondary
   inspectors instead of squeezing the center out of view.
2. `[x]` Add safe command-palette and keyboard-workspace interactions to the
   visual smoke loop without opening native dialogs or touching transports.
   The Demo scenario now changes workspaces through Ctrl+2/Ctrl+3, captures the
   Ctrl+K palette, dismisses it with Escape, and verifies the resulting frame.
3. `[x]` Emit a durable machine-readable visual-smoke manifest containing
   process exit state, screenshot dimensions, and stdout/stderr summaries.
4. `[x]` Add lightweight screenshot sanity checks for black/blank captures and
   wrong-window contamination without introducing brittle pixel-golden tests.
   Passes are desktop-serialized and target one verified HWND; sampled color,
   non-black ratio, dimensions, and file-size checks reject empty GPU captures.
5. `[x]` Complete the remaining primary-panel accessibility audit and close the
   umbrella offline-workflow item once visible labels, focus, disabled reasons,
   and empty states are verified in the live loop.
6. `[x]` Add a release-package smoke pass that runs the same Welcome/Demo loop
   against installed assets rather than the source-tree build layout. Install
   rules now stage docs, changelog, the demo project and its demo-pack, plus
   synthetic log fixtures. An isolated flat Windows install passed headless
   startup and the full six-frame Demo visual/keyboard scenario with clean
   stderr.

### Next queue - ROM editing and verification

1. `[x]` Make project saves read-after-write verified. Every working-ROM save
   now reopens the project and compares source/working/additional ROM bytes,
   histories, active metadata, definition identity, and checkpoint integrity;
   the GUI remains dirty on failure and reports "Saved and verified" on success.
2. `[x]` Extend the live GUI loop with an isolated project copy, a real cell
   edit, undo/redo, save, close/reopen, and visible value/history confirmation.
   `gui-visual-smoke.ps1 -Scenario EditRoundTrip` copies the demo project and
   its referenced pack into an isolated `.cache/ui-smoke/` copy, opens the
   Boost table via the command palette, edits a cell (F2 inline editor),
   undoes and redoes (redo via the palette command), saves with read-after-
   write verification, closes, and reopens from recents, confirming the edited
   value and history persisted. Redo is driven through the palette command
   rather than a Ctrl+Shift+Z chord, which on some machines is a global
   overlay/driver hotkey (confirmed AMD overlay on the primary dev box) that
   pops a system panel and steals the keystroke. The screenshot sanity check
   also uses a denser grid + 5-bit quantization so low-chroma frames (e.g. the
   Log Explorer empty state) no longer false-fail.
3. `[x]` Add an in-app project verification report with individual checks and
   remediation links rather than only a single readiness badge. The Project
   Readiness panel now opens with a verification summary tally ("N of M local
   checks verified / K need attention") across ROM shape, definition, source
   CID, saved-state, flash-ready checksum, and checkpoint, above the existing
   per-check rows. Non-passing checks that are fixable in-app carry a
   remediation action: a "Save project" button on the pending-save row and a
   "Repair balance" button on a needs-repair checksum row (records the 2-byte
   0x1FFFFE fix as one undoable edit). Verified live via
   `gui-visual-smoke.ps1 -Scenario Readiness`.
4. `[x]` Verify checksum repair/validation behavior for edited ROM exports and
   clearly distinguish project integrity from flash-ready checksum state.
   `st::tune_export` gained `checksum_state` (FlashReady / NeedsRepair /
   NotApplicable), `flash_checksum`, and an idempotent `repair_balance` that
   re-tunes only the 0x1FFFFE balance word so the 2 MB app window sums to
   0x5AA5 — proven by tests (repair makes a perturbed ROM flash-ready, is
   idempotent, preserves every cal byte, and rejects non-2 MB images). The
   Project Readiness panel now shows a "Flash-ready checksum" row directly
   below the project-integrity "Saved-state verification" row, with copy that
   spells out the difference (a coherent project can still be off 0x5AA5).
   Verified live via `gui-visual-smoke.ps1 -Scenario Readiness`.
5. `[x]` Exercise CSV bulk edits, range operations, interpolation, and smoothing
   through save/reopen/diff round trips on representative fixture tables.
   `tests/unit/project/test_project.cpp` builds a self-contained pack with a
   3x4 uint8 cal table and drives parse_edit_csv bulk edits, percent_scale_cells
   (range), interpolate_cells, and smooth_cells through read_table_values ->
   op -> write_table_values -> save_all -> reopen -> read_table_values, asserting
   every reopened cell equals the op result quantized to uint8 storage (clamp +
   round half away from zero, the write_typed contract). 138 assertions.
6. `[x]` Add property coverage for edit → undo/redo → save → reopen invariants,
   including multi-ROM histories and scaling quantization boundaries.
   `tests/unit/project/test_project.cpp` drives eight byte edits with a partial
   undo + redo before save and asserts the reopened working ROM bytes match the
   persisted history cursor (not just record count), plus an independent
   per-ROM history test proving working and additional-ROM histories persist to
   separate files without leaking into each other. `tests/unit/defs/test_defs.cpp`
   covers write_typed's clamp-then-round-half-away contract and an
   engineering→invert→write→read→apply round trip that snaps to the nearest
   representable raw step at the quantization midpoints and clamps outside the
   range. Full suite 1941 pass / 0 fail / 2 skip.

### Next queue - Custom Feature Designer application pass

1. `[~]` Exercise the complete graph workflow in the live GUI. The autonomous
   Designer scenario now inserts a pack-backed Engine RPM hook and Add
   primitive, wires compatible float pins, opens the pin editor, commits a
   default, observes lint transition from one warning to OK, and captures node
   and edge context menus. The scenario now covers thirteen sanity-checked
   frames, including transient SH-2A compile preview plus dirty Load and Clear
   confirmations. Native-dialog save/load/clear round-trip equivalence remains.
2. `[~]` Add fixture-driven round-trip tests proving graph nodes, edges,
   defaults, labels, positions, and compile output survive save/reopen. Core
   `.stmod` file helpers now round-trip canonical graph content and cover
   missing/malformed files; the existing TOML suite covers graph shape,
   positions, labels, pins, defaults, and edges. Compile-output persistence and
   a checked-in representative graph fixture remain.
3. `[~]` Replace file-dialog-only persistence with project-aware feature
   assets, dirty-state handling, recent-file context, and explicit save/load
   success or failure feedback. The designer now tracks its document path and
   every graph mutation, defaults dialogs to `<project>/features`, validates
   completed writes, reports save/load results with toasts, and gates Clear
   behind visually exercised unsaved-work confirmation. Load has the equivalent
   gate and Save repeats directly once a document path exists. Recent-file
   context and automated native-dialog round trips remain.
4. `[~]` Turn validation and lint output into an actionable findings view with
   node/pin navigation, and distinguish structural errors, incomplete inputs,
   unsupported backend operations, and pack incompatibility. The designer
   status popup now separates structural errors (validate failures) from
   completeness warnings (lint), and each lint finding that carries a node id
   is a clickable row ("→ node 'Add' has no connections") that selects the
   node and recenters the canvas on it. Verified live. Distinguishing
   unsupported-backend vs pack-incompatibility categories remains.
5. `[~]` Expose the existing feature-codegen library as an actual in-canvas
   SH-2A/RH850 compile preview, then verify it against safe synthetic fixtures,
   malformed graphs, and code-generation failures. The 2026-08-21 autonomous
   interaction audit found that the panel copy claimed this preview existed
   although no compile control or output surface was present. Keep insertion
   and flashing visibly disabled while the hardware gate is closed. The canvas
   now lowers, selects the definition-backed ISA, compiles transiently, reports
   IR/hooks/code/RAM metrics, runs the address gate, exposes expandable IR, and
   marks stale previews. The live demo scenario caught and fixed canonical
   dotted platform selection (`subaru.va.*` / `subaru.vb.*`) and visually
   confirms SH-2A preview output. A patch-producing fixture, RH850 visual pass,
   and focused failure-state screenshots remain.
6. `[~]` Add designer undo/redo for graph mutations and keyboard-accessible
   node, edge, connection, deletion, and property-edit workflows. Graph
   undo/redo is implemented via diff-based snapshots (the graph is serialized
   at rest; a checkpoint fires when it differs from the baseline, capturing
   every mutation — add/delete/wire/pin-default/move — at one site and
   coalescing a drag into one checkpoint). Undo/Redo toolbar buttons + Ctrl+Z /
   Ctrl+Y (and Ctrl+Shift+Z) when the designer is focused and no text field is
   active; load resets the history. Verified live: undoing a pin-default edit
   reverts to a lint warning, redo restores it. Broader keyboard-accessible
   node/edge/connection workflows remain.

## Background track - VA STI DCCD and combination-meter integration

This is a background acquisition and passive-research track; it does not require
buying a complete running donor. The focused playbook and evidence boundaries
are in `docs/55-va-sti-dccd-integration.md`.

- `[x]` Confirm that the OBDX `sniff` command records unfiltered raw CAN in
  hardware listen-only mode.
- `[ ]` Add lossless v1 sniff-log ingestion to the `.asc`/`.cdb` CAN discovery
  pipeline.
- `[ ]` Capture the recipient VA WRX parked baseline using the documented
  action sequence.
- `[ ]` Obtain an owner-authorized stock VA STI capture when a suitable vehicle
  becomes available; record year/trim/ignition and relevant part numbers.
- `[ ]` Source authoritative year-specific wiring facts and classify direct
  versus CAN-carried DCCD inputs.
- `[ ]` Bench-identify a loose DCCD controller before considering live replay
  or injection.
- `[ ]` Keep combination-meter/key registration in the authorized replacement
  workflow; do not conflate it with DCCD firmware flashing.

## P0 - safety, recovery, and truth of state

These tasks protect the ECU, the user, and every later validation result.

- `[ ]` Freeze the current bricked ECU state. Record ECU label, CID, connector
  state, power-supply settings, last attempted operation, source ROM, modified
  block, tool version, and all logs.
- `[~]` Procure and qualify a matching spare ECU. The DIY SH-2A probe remains a
  later research track in `docs/49`; it is not the immediate bench recovery
  dependency. Record the exact CID, part number, board revision, source,
  condition, and whether it can be read before any write.
- `[ ]` Identify the exact ECU before selecting a recovery procedure: label/CID,
  PCB revision, MCU top-mark, debug/boot pads, and board-side power domains.
  Treat a generic “SH72543 JTAG/BOOT” tool listing as insufficient evidence for
  a pin-by-pin connection.
- `[ ]` Get tool/vendor confirmation that the selected probe can identify and
  read this exact MCU/board before erase. Record the supported target name and
  software version in the recovery log.
- `[ ]` If the ECU can still be read, capture a raw image and identity report
  before any write. Preserve the original image immutably and hash every copy.
- `[ ]` Recover or replace the ECU, then verify identity, communication,
  diagnostic session entry, security access, full readback, checksum status,
  and power-cycle persistence.
- `[ ]` After recovery, compare the restored image against the exact-CID stock
  reference and independently verify the boot-integrity contract. Do not use a
  neighboring CID merely because it has the same nominal size or engine family.
- `[ ]` Re-run the bench validation sequence from
  `docs/42-bench-rig-validation-runbook.md`: readback, authentication, one
  deliberate write, power-loss injection, recovery, and delta-flash checks.
- `[ ]` Complete the 100-cycle flash gate on a sacrificial ECU: zero bricks,
  zero unrecoverable images, byte-verified readback after every cycle.
- `[~]` Add a hard preflight stop for unknown CID, unknown definition,
  suspicious source image, unverified checksum, unsafe region, interrupted
  journal, or missing recovery image. Communication-state blocking is complete;
  identity/image/journal checks are unified and the GUI Flash Review now
  renders their blockers. Live observed-CID binding and backup/recovery
  attestation remain hardware-dependent; the definition-side calibration
  model and offline selection are implemented.
- `[ ]` Add a post-write durability gate: power-cycle, re-identify, read back,
  compare the intended sectors, and require an explicit operator acknowledgement
  before declaring success.

## P0 - hardware-independent product surfaces

These can be built and tested while the ECU is unavailable.

- `[~]` Build a Project Readiness screen with traffic lights for ROM identity,
  CID/definition match, source-image hash, checksum, policy profile, changed
  regions, risky tables, unresolved warnings, backup status, and recovery plan.
  The first offline-first panel is now available from `View -> Project
  Readiness`; it now surfaces source-CID matching and approved-versus-
  candidate calibration-region coverage. Live identity, backup attestation,
  and ECU eligibility remain explicit hardware-dependent states.
- `[~]` Add named checkpoints: Stock, Baseline, Current, Before Flash, and
  After Flash. The project model now persists immutable snapshots under
  `checkpoints/<id>.bin` with CRC32, size, timestamp, provenance notes, and
  load-drift warnings; Project Readiness can create a `Before Flash`
  checkpoint. Compare can use checkpoints directly, and `project-info --json`
  exposes their metadata for CI/handoff; `project-validate` fails on missing
  or drifted checkpoint evidence. `project-restore-checkpoint` restores one
  with `--yes` and records a single undoable byte transaction; automatic
  Stock/Current/After-Flash lifecycle capture remains next.
- `[ ]` Finish the pre-flash semantic review: grouped table changes, old/new
  values, affected regions, safety/emissions markers, checksum result, and a
  plain-language explanation of what can go wrong.
- `[ ]` Implement the tune-library index and attestation workflow: local tune
  identity, source/hash matching, AP `backupcksum` matching, duplicate
  detection, and explicit “unknown tune” handling.
- `[ ]` Implement the pre-flash structured delta against the currently
  identified tune, not only against the project source ROM.
- `[x]` Build Map Explorer: the Tables sidebar now supports text, category,
  safety/emissions facets and richer role/axis/note hover metadata. Remaining
  purpose/unit/provenance/CID coverage, confidence, sibling variance, common
  co-edits, and related safety-pair views.
- `[~]` Build Log Explorer around imported CSV first: it is now a persistent
  docked Log-workspace surface with case-insensitive signal search,
  multi-channel overlays, selectable/automatic timestamp axes, per-channel
  statistics, workflow signal profiles, and visible import-fidelity diagnostics
  for irregular rows and invalid cells. Header units, generic delta/ratio/
  percent-error channels, session notes, and event markers are now implemented.
  Saved sessions, selected-range statistics/plots, CSV and Markdown findings
  export, and RPM/load-to-active-map cell tracing are implemented. User-authored
  signal profiles and live acquisition can plug into the same model later.
- `[~]` Add signal profiles for common workflows: built-in Fueling/MAF, FA24
  swap, Boost Control, Ignition/Knock, AVCS, and Cold Start profiles now select
  matching imported-log channels. User-authored/persisted profiles, FA20
  baseline, and drivability-specific mappings remain queued.
- `[ ]` Add external wideband and user-defined channels with calibration,
  provenance, and validation warnings.
- `[ ]` Make every dangerous action explain the reason for its gate and the
  exact next action. Preserve the calm, modern, non-intimidating visual system.

## P1 - definition and metadata foundation

- `[ ]` Quarantine VA/LF79 Wastegate Duty tables declared as 8x7. Firmware
  descriptor evidence resolves them as 14x17; require an exact-CID fixture,
  descriptor-backed lint, and read/edit/write round-trip before re-enabling.
- `[ ]` Make table-shape confidence independent from address confidence. Audit
  all generated VA 2-D table shapes rather than assuming an address match also
  validates dimensions.
- `[ ]` Extend the public Atlas adapter with provenance-aware metadata for
  `advanced`, `beta`, `dangerous`, `regulated`, memory section, execution
  context, and source confidence. Keep these distinct from calibration-value
  variance.
- `[ ]` Define a confidence model with separate dimensions for address,
  shape/dimensions, units/scaling, family alignment, and provenance. Never turn
  cross-sibling byte differences into a false definition failure.
- `[ ]` Add definition lint for duplicate addresses, overlapping regions,
  invalid axes, suspicious scaling, missing units, unsupported flags, and
  tables whose exact CID coverage is not declared.
- `[ ]` Add a visible provenance trail to every definition fact: public XML,
  clean-room inference, analyst comparison, user confirmation, or unverified.
- `[ ]` Keep commercial Atlas runtime/decompiled artifacts, protected classes,
  cipher material, and proprietary expressions out of public packs. Generate
  only scrubbed facts and independently authored metadata.
- `[ ]` Add exact-CID pack selection and a coverage report so a sibling pack is
  never silently used as an exact match.
- `[ ]` Add safety-pair and co-edit relationships to the table editor, with
  review prompts rather than automatic edits.

## P1 - offline reverse engineering queue

The goal is reusable, byte-verified knowledge rather than a large pile of
unnamed decompiler output.

- `[x]` EP5G600A/601A: full Ghidra analysis/decompilation and public XML fact
  import completed analyst-side.
- `[x]` EZ1GB10G/10H: full decompilation and sibling body comparison completed.
- `[x]` LF79100P: canonical plaintext source identified; 7,335/7,340 named
  decompilations succeeded; 480 function names applied from byte-verified
  LF79103P evidence.
- `[~]` LF79: manually review the seven size-boundary mismatches, clean up the
  40-name provenance exceptions, and investigate a permitted plaintext source
  for LF79101P. Keep high-entropy/encrypted blobs quarantined.
- `[~]` A8DH101I: source/architecture provenance manifest and 23-file sibling
  fingerprint completed (eight unique variants); 439 public table labels were
  imported analyst-side and all unique variants were localized against the
  316 addressed definition tables. Review the 21-28 unlabeled clusters per
  variant and promote only facts stable across exact family/CID evidence.
- `[~]` A8DH201X/202X: independently imported and fully decompiled through
  matched headless pipelines; all 3,148 discovered function bodies are
  byte-identical. The 16-longword reset/exception vector area and chain
  `0xB68 -> {0xB7C,0xBAA} -> [0xCA8]=0x6FC`
  promoted with provenance manifests. Classify 22 non-code sibling-difference
  clusters and reconstruct hardware-register/runtime initialization. Keep
  A8DH101I separate (542,060 differing bytes; no blind name transfer).
- `[~]` LF9C/LF9D/LF9G/LF9L: exact plaintext hashes and complete Ghidra
  function-index baselines are verified in `docs/53` (7,101-7,385 functions
  per CID; all inside the source mapping). Continue semantic naming,
  byte/structure-verified cross-CID comparison, subsystem reconstruction,
  public-fact import, and exact-CID coverage summaries.
- `[~]` VB LHB/RH850: the canonical inventory, architecture-correct decompile,
  diagnostics structure, and 16-image checksum invariant are complete. Next,
  implement offline checksum repair and recover enforcement/bank behavior.
- `[~]` Build a repeatable Ghidra MCP/agent pipeline: import, processor
  selection, reset-vector seed, public-label import, decompile, sibling
  fingerprint, name application, mismatch report, and provenance manifest.
  Canonical overrides/exclusions and transactional output publication now
  exist; wire them into the repo-side manifest and regression checks.
- `[ ]` Add analyst-side regression checks that fail on accidental source-image
  substitution, address drift, or name transfer across incompatible siblings.
- `[ ]` Convert only independently verified, policy-cleared facts into public
  definitions and tests.

## P1 - FA24 swap workflow

The OEM-ECU chassis/authorization workstream is tracked in
`docs/52-immobilizer-swap-integration.md`. Its purpose is to make newer Subaru
engine/ECU packages usable in older chassis without requiring a standalone.
The initial 2017 keyed/push-button profiles and registration-readiness audit
are implemented; connector topology, live state, CAN gateway, and hardware
registration validation remain open.

- `[ ]` Finish the hardware-independent FA20-to-FA24 workflow as a guided plan:
  project setup, required tables, dependencies, logging profile, review, and
  rollback checkpoint.
- `[ ]` Encode the currently supported five-table baseline as explicit roles,
  with required/optional status and an evidence link for each table.
- `[ ]` Add swap-specific validation for VVT/cam-angle behavior, HPFP/fueling,
  VE/load modeling, torque/throttle coordination, boost control, and cold-start
  behavior as evidence becomes available.
- `[ ]` Add “not yet verified on hardware” labels to every swap function that
  depends on live ECU behavior.
- `[ ]` When the rig is recovered, validate one change at a time with logs and
  a reversible checkpoint. Do not combine swap calibration, feature patches,
  and flash-protocol experiments in the first run.

## P2 - transport, flashing, and bench close-out

- `[ ]` Close Phase D implementation gaps: durable commit/readback, `0x37` /
  `0xB7` transfer-exit behavior, resume journal behavior, and delta-only
  execution against a real ECU.
- `[ ]` Byte-validate `subaru_std`, `subaru_alt`, and `subaru_alt2` checksum
  repairs against known-good stock images.
- `[ ]` Finish platform J2534 dynamic loading and real-adapter validation;
  keep MockTransport tests as the deterministic baseline.
- `[ ]` Validate sustained logging and gauge behavior on the bench/car path,
  then validate signal timing and loss handling under load.
- `[ ]` Validate AP/library attestation and the `.ptm` round-trip against the
  recovered ECU's actual installed state where applicable.
- `[ ]` Create a recovery drill: interrupted flash, lost power, corrupt
  journal, wrong CID, and deliberate unsafe-region request. Each must end in a
  safe, understandable state.

## P2 - testing and release quality

- `[ ]` Add fixture-driven regression packs for every promoted CID, including
  ROM identity, table reads, axis decoding, scaling, checksum, and diff output.
- `[ ]` Add golden tests for readiness status and pre-flash review decisions.
- `[ ]` Add property/fuzz coverage for definition parsing, scaling, table edits,
  journal recovery, and imported log parsing.
- `[ ]` Add HIL tests only after the ECU recovery gate: read, authenticate,
  write one approved region, power-cycle, read back, and restore stock.
- `[ ]` Run a clean-room/provenance audit before publishing any new pack or
  analyst-derived naming set.
- `[ ]` Complete accessibility, keyboard navigation, onboarding, installer,
  documentation, and crash-report-only opt-in work for the 1.0 surface.

## P3 - after the safety gate

- `[ ]` Patch insertion and end-to-end custom-feature flashing on real vector
  tables, beginning with the smallest reversible sample.
- `[ ]` Live tuning through RAM-shadow/UDS only after the 100-cycle gate and
  per-write safety model are proven.
- `[ ]` Differential flashing and composable patch sets with conflict-aware UI.
- `[ ]` VA/VB AT coverage, EJ-era families, BRZ/86, and broader current Subaru
  families in that order unless evidence changes the priority.
- `[ ]` CAN reverse-engineering toolkit: replay mode, statistical discovery,
  labeled events, draft DBC export, and optional advisory interpretation.
- `[ ]` Optional local-LLM advisory layer for logs and definitions. It remains
  read-only and cannot enter the flash/write path.

## Recommended next ten actions

1. Freeze and document the bricked ECU state.
2. Secure boot/JTAG recovery or a matching spare ECU.
3. Finish the readiness/checkpoint data model in the application.
4. Quarantine the VA Wastegate Duty 8x7 definitions and add descriptor lint.
5. Implement and regression-test the recovered VB checksum profile.
6. Build imported-log Map Explorer and signal profiles.
7. Encode the FA24 five-table guided workflow with “unverified on hardware”
   states.
8. Finish LF79 mismatch review and A8DH101I QA.
9. Recover the ECU and execute the bench runbook one gate at a time.
10. Start the 100-cycle flash validation only after all preceding gates pass.

## Definition of “ready to ship a flash”

The application should refuse to flash unless it can answer, visibly and
unambiguously: what ECU this is, which exact definition applies, what changed,
why each change is allowed, how the image was checked, where the recovery image
is, how the operation can resume, and how success will be verified after a
power-cycle. That is the product's central promise.
