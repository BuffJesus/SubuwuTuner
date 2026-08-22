# 48 - UI/UX and reverse-engineering plan

Updated 2026-08-21. This plan follows `docs/43-jtag-recovery.md`,
`docs/45-product-direction-and-coverage.md`, `docs/46-tuning-software-user-needs.md`,
and `docs/47-task-list.md`.

The product should become a calm calibration notebook that can grow into a
full tuning instrument. The central promise is not “more maps” or “more
protocols”; it is that the user can always understand what they are looking
at, what changed, what evidence supports it, and what remains unsafe or
unverified.

## Non-negotiable boundary

The current ECU has no CAN/UDS response after power-cycle. Do not use repeated
OBD writes as an experiment, and do not apply the SH7058/E2-Lite details in
`docs/43` to an unverified SH72543-style board. Recovery requires exact ECU
identity, board/MCU confirmation, tool support, and preservation of any
readable image before erase or programming.

Everything below the Hardware Return Gate is designed to work with ROM files,
fixtures, imported logs, mocks, and analyst-side Ghidra projects.

## Product structure: three levels of truth

The UI should make three different claims visually distinct:

1. **Project truth** - the local ROM, definition, edits, hashes, checkpoints,
   and policy state are internally coherent.
2. **Plan truth** - the proposed changes are bounded, understood, linted,
   checksum-aware, and policy-eligible.
3. **ECU truth** - a live ECU has been identified, backed up, authenticated,
   written, read back, and power-cycle verified.

Never show one green “ready” badge when only the first claim is true.

## Phase A - make the current product feel finished without hardware

### A1. Project Readiness cockpit

**Progress:** the first offline-first panel is implemented and available from
`View -> Project Readiness`. It now reports ROM shape, CRC32 identities,
definition lint, checksum declaration, edit footprint, persistent checkpoint
presence/drift, recovery evidence, the live-ECU boundary, and the flash
preflight blockers. It can create a `Before Flash` checkpoint without
hardware, and now shows the source-ROM CID plus approved/candidate
calibration-region coverage. The next increment is attested live identity and
live calibration-region evidence.

Add a first-class `Readiness` panel and make it the default post-open
orientation surface. It should show a quiet vertical sequence of cards:

- **Identity**: vehicle profile, source CID, expected pack, source hash, ROM
  size, and exact-CID coverage.
- **Definition**: pack version, lint result, checksum strategy, provenance
  coverage, unresolved mappings, and confidence summary.
- **Changes**: changed table count, changed bytes/sectors, risky tables,
  affected regions, and “open review” action.
- **Recovery**: source backup, exact-CID recovery image, checkpoint age, and
  the current flash-journal state.
- **Hardware**: disconnected, reachable, identified, authenticated, or
  external-recovery-required.
- **Next action**: exactly one recommended action such as `Inspect changes`,
  `Compare to baseline`, `Import a log`, `Run definition lint`, or `Recover ECU`.

Each card needs a short reason and one action. Avoid a dashboard full of
numbers with no decision attached.

### A2. Calibration Compare and checkpoints

Implement named, immutable-ish project checkpoints as hashes plus notes. The
first persistence slice is complete: checkpoints are copied into the project
and revalidated on open, with drift warnings instead of silent trust:

- Stock
- Baseline
- After MAF/fueling
- After boost
- After ignition/knock
- Ready to flash
- Before flash
- After flash

The Compare surface should support:

- ROM-to-ROM semantic comparison;
- checkpoint-to-working comparison;
- table and cell counts;
- engineering-unit deltas;
- heatmap and side-by-side views;
- “why this changed” notes from edit history;
- explicit sibling-CID alignment only when coverage is attested.

The first checkpoint comparison path is now wired into the existing Compare
panel: saved checkpoints appear beside source/working/additional ROMs and can
be sent to either compare slot without leaving the project or reloading a
binary from disk. A headless `project-restore-checkpoint --yes` path now
restores a checkpoint as one undoable byte transaction, making the local
workflow reversible without introducing a hidden destructive UI action.

The user should be able to answer “what did I do since baseline?” in one
screen, without opening a raw binary diff.

### A3. Map Explorer

Add a search-first map browser that sits between the welcome surface and the
expert grid. Search facets should include:

- purpose and domain;
- table role and aliases;
- category, unit, axis, and dimensions;
- safety/emissions/regulated status;
- CID coverage;
- confidence and provenance;
- related tables, safety pairs, and co-edits;
- beginner/intermediate/advanced learning level.

Every result should show a small field note: what it controls, what evidence
supports that statement, what changes commonly accompany it, and what can go
wrong. The expert table editor remains one click away.

**Progress:** Atlas-backed field notes now expose purpose, unit, provenance,
address-match confidence, exact-CID bindings, sibling variance, tuner clusters,
common co-edits, and matched safety-pair severity/rationale.

### A4. Guided tuning tasks

Add task cards that compose existing functionality rather than hiding it:

1. Establish identity and baseline.
2. Fueling and MAF.
3. Boost response.
4. Ignition and knock.
5. Cold start and drivability.
6. FA20-to-FA24 swap preparation.
7. Review, compare, export, and recovery planning.

Each task needs inputs, expected evidence, reversible edits, linked tables,
log channels, completion notes, and a clear “not verified on hardware” state.

**Progress:** Project Readiness now contains all seven cards with inputs,
expected evidence, links into the relevant map/log/review surfaces, persistent
per-project local completion notes, and an explicit hardware-unverified badge.

### A5. Pre-flash review as a narrative

Keep the current changed-table review, but organize it as:

`What will change -> Why it changed -> What evidence supports it -> What is
risky -> How to undo it -> What must be verified after power-cycle`.

Show local plan readiness separately from ECU readiness. Surface all blockers
in one place, including the new communication, identity, source-image,
recovery-image, journal, checksum, and approved-region diagnostics.

**Progress:** Flash Review now separates local-plan and live-ECU truth, renders
the six-part plain-language narrative, and gives each preflight blocker a
specific next action.

### A6. Log Explorer, imported-first

**Progress:** the imported-data viewer is now a persistent docked Log Explorer
available from the Log workspace, Tools, View, and the command palette. It has
searchable signals, multi-channel overlays, automatic or explicit axis
selection, channel statistics, six built-in workflow signal profiles, UTF-8 BOM
handling, strict numeric-cell parsing, explicit irregular-row/invalid-cell
counts, parsed header units, generic calculated channels, session notes, and
sample-index event markers, durable TOML sessions, selected-range plots and
statistics, plotted-range CSV export, Markdown findings reports, and RPM/load
sample tracing to the nearest cell of the active table. It does not yet persist
user-authored profiles, attach provenance mappings, or acquire live data.

Build the data workflow without waiting for the ECU:

- import CSV and common tuner log formats;
- map columns to named signals with units and calibration provenance;
- synchronized multi-axis plots;
- event markers and session notes;
- map-cell tracing when RPM/load axes are available;
- derived channels such as lambda error, boost error, DAM/knock summaries,
  injector duty, and AVCS error;
- external wideband and user-defined channels;
- saved signal profiles for FA20, FA24, fueling, boost, ignition, AVCS, and
  cold start.

Live acquisition should later use the same model, not a second UI architecture.

## Phase B - trust, polish, and daily usability

- Add a compact provenance/confidence chip system: Known, Attested, Inferred,
  Candidate, and Unverified. Tooltips must explain the evidence, not just show
  a color.
- Add an “explain this” side panel for tables, diagnostics, policy blockers,
  and decompiler-derived facts. Explanations are advisory and read-only.
- Improve empty states so every unavailable feature says why and gives the next
  useful offline action.
- Add beginner/expert density presets. Beginner mode hides implementation
  detail but never hides safety status; expert mode exposes addresses,
  scaling, execution context, and provenance.
- Add keyboard-first navigation for Map Explorer, Compare, and Readiness.
- Add a persistent project breadcrumb: vehicle -> CID -> checkpoint -> active
  table -> unsaved edits.
- Make destructive actions use the same language everywhere: `Review changes`,
  `Create recovery checkpoint`, `Verify policy`, `Write to ECU`.
- Keep the visual language restrained: purple for identity/selection, amber for
  attention, red only for a real stop, and green only for an evidenced pass.

## Phase C - reverse engineering that pays back into the product

The RE queue should be measured by product outputs. Each family pass must
produce a provenance manifest, architecture notes, sibling comparison, named
facts, definition candidates, confidence grades, and regression tests.

### C1. Harden the Ghidra MCP/agent pipeline

Make the existing analyst workflow repeatable:

1. verify image source and entropy/plaintext status;
2. select processor and endianness;
3. seed reset vector and boot anchors;
4. import only permitted public labels/facts;
5. run analysis and decompilation;
6. fingerprint function spans across siblings;
7. apply names only on byte/shape-verified matches;
8. emit function index, mismatch report, and provenance manifest;
9. run a regression check against accidental source substitution or cross-family
   name transfer.

The agent should produce evidence bundles, not silently edit public definitions.

### C2. Finish the active VA work

- **LF79**: review the seven size-boundary mismatches, clean up the 40 naming
  exceptions, and investigate a permitted plaintext LF79101P source. Keep
  high-entropy/encrypted blobs quarantined.
- **A8DH101I**: finish public XML label QA and promote only stable address/shape
  facts. Keep value variance separate from definition confidence.
- **A8DH201X/Z**: analyze as distinct variants; do not transfer A8DH101I names
  from weak span overlap.
- **LF9C/LF9D/LF9G/LF9L**: prioritize boot integrity, checksum, table-role,
  and live-signal seams that improve the VA tuning workflow.

### C3. Expand by usefulness, not curiosity

Next families worth working on:

- **EP5G**: older family baseline and reusable boot/UDS architecture patterns.
- **EZ1G**: strong sibling corpus for address drift and definition confidence.
- **A8DH**: non-WRX/NA family comparison and table-label methodology.
- **VB LHB/RH850**: exact-CID inventory, boot-bank model, and definition QA;
  keep separate from SH-2A assumptions.
- **Additional VA LF/LH families** only when they improve checksum handling,
  table roles, logging, swap support, or coverage.

Do not spend the next cycle generating thousands of unnamed decompilations.
Spend it on the functions and structures that become one of:

- a verified table or axis;
- a checksum/integrity rule;
- a boot/recovery state;
- a UDS/transport behavior;
- a RAM signal anchor;
- a family/variant discriminator;
- a test fixture or definition-lint rule.

### C4. Specific RE targets

For each family, prioritize these surfaces:

1. reset/startup and boot-integrity path;
2. flash erase/write/commit and checksum routines;
3. UDS/SSM dispatch and negative-response behavior;
4. CID/vehicle identity and variant-selection logic;
5. RAM signal tables and live-datalog address joins;
6. table/axis descriptors and scaling helpers;
7. torque/throttle, fueling, ignition, AVCS, boost, and safety-limit call sites;
8. calibration bank boundaries and protected/ignored regions;
9. sibling deltas that reveal variant switches rather than calibration values;
10. failure loops and watchdog/recovery behavior.

Static decompilation should be paired with byte-level evidence and sibling
comparison. A plausible decompiler name alone is not a promotion reason.

## Phase D - Atlas and definition integration

Use Atlas-derived material as an analyst-side metadata source and QA aid only.
The public adapter may consume scrubbed, independently authored facts such as:

- domain hierarchy;
- purpose and aliases;
- units/scaling;
- axis meaning;
- memory section;
- advanced/beta/dangerous/regulated review flags;
- execution context when independently supported;
- exact-CID coverage and confidence.

Do not promote commercial runtime code, protected class hierarchies, cipher
material, proprietary expressions, or silent authority claims.

Add definition metadata in layers:

1. provenance and confidence;
2. exact-CID coverage;
3. role and related-table links;
4. review/risk flags;
5. family-specific calibration-region model;
6. live-signal mappings.

The calibration-region model must remain distinct from the existing codegen
`[[writable_region]]` model until the semantics are deliberately unified.

## Phase E - hardware return gate

When the ECU is recovered or replaced, execute only this order:

1. exact board/MCU/tool identification;
2. raw read and hash;
3. exact-CID comparison and definition match;
4. security access;
5. readback stability across power cycles;
6. one smallest reversible write;
7. readback and power-cycle verification;
8. deliberate power-loss recovery;
9. delta-flash states;
10. custom-feature patch insertion;
11. only then, broader flash cycling and live logging.

Static RE confidence must never substitute for this HIL evidence.

## Recommended execution order

### Now, with no ECU

1. Implement the Readiness panel using offline project facts and explicit
   “hardware proof unavailable” states.
2. Add named checkpoints and checkpoint comparison.
3. Finish LF79 mismatch cleanup and A8DH101I QA.
4. Harden the Ghidra evidence pipeline and manifests.
5. Build Map Explorer and provenance/confidence field notes. The first
   sidebar slice now provides text, category, safety/emissions facets and
   richer role/axis/note hover metadata.
6. Build imported-log profiles and Log Explorer foundations.
7. Encode the FA24 swap task workflow and five-table evidence baseline.
8. Populate and independently review exact-CID calibration regions, then
   bind live observed-CID evidence into the preflight path.

### After ECU recovery

1. Validate exact identity and full readback.
2. Close checksum and durable-commit gaps.
3. Validate recovery and power-loss paths.
4. Validate live signals and logging.
5. Begin 100-cycle flash gate.
6. Validate patch insertion and FA24 behavior one reversible change at a time.

## Definition of success

The next major release should let a beginner open a ROM and know what to do,
let an expert inspect every address and evidence trail, let a tuner compare
changes and logs without leaving the project, and refuse unsafe writes without
making the user feel lost. The reverse-engineering system should make coverage
broader while making confidence more honest.
