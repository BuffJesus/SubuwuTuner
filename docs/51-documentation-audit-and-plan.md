# 51 - Documentation audit and execution plan

Updated 2026-08-02. This is the current navigation and sequencing document
after an audit of the numbered design docs, user-facing guides, concepts,
workflows, references, and session handoffs under `docs/`.

## Audit verdict

The documentation is substantial and contains the needed technical material,
but it had three competing status layers:

1. Foundational numbered design docs, many of which intentionally preserve
   historical implementation plans.
2. Current state documents `47` through `50`, which reflect the silent/bricked
   LF79002P bench ECU and the hardware-free product direction.
3. Dated handoffs, which are historical records and should not be rewritten as
   if they describe the present state.

The current source of truth for execution is now:

`51 -> 47 -> 48 -> 43/49/50 -> 42`

Use older design docs for architecture and evidence, but use `47` for task
priority and `43`, `49`, and `50` for the recovery decision. `42` is a
post-recovery runbook and is blocked until the hardware-return gate passes.

## Corrections made in this audit

- Withdrawn the unvalidated Renesas E2-Lite recommendation from current
  recovery guidance. The exact MCU, board revision, debug interface, and
  vendor-supported probe must be confirmed before any connection or erase.
- Marked the current LF79002P bench ECU as silent after power-cycle and the
  live validation runbook as blocked.
- Corrected the broken `docs/43` link to `49-diy-sh2a-recovery-probe.md`.
- Clarified that `docs/31`, `docs/42`, and `docs/concepts/brick-protection.md`
  contain historical recovery material, not a universal SH72543 procedure.
- Updated the root README, glossary, `CLAUDE.md`, roadmap, and docs index with
  the current recovery boundary.
- Added the exact-CID `[[calibration_region]]` schema, inheritance and
  duplicate validation, plus `pack-info` text/JSON reporting of address,
  status, and provenance. No unverified production ranges were added.
- Added checkpoint persistence, drift reporting, semantic compare integration,
  restore transactions, CLI metadata, clone preservation, and validation to
  the current product path in code; the related docs now describe those as
  implemented slices rather than future work.

## What remains intentionally historical

The dated files under `docs/handoffs/` are evidence of prior sessions. They
should remain immutable unless a new handoff explicitly supersedes them.
Likewise, firmware addresses, RAM markers, signature locations, and protocol
observations in `docs/31`, `docs/37`, `docs/40`, and `docs/43` are evidence
scoped to named CID/image families. They must not be generalized into a
different ECU family or silently promoted into a public definition pack.

## Ranked execution plan

### P0 - truth and safety boundary

1. Keep the current ECU in external-recovery state. Preserve labels, images,
   hashes, logs, power settings, and the last modified block. Do not retry OBD
   writes.
2. Procure and qualify an exact-CID replacement donor using `docs/50`, or use
   a recovery service that can document support for the exact board.
3. Before the next live write, populate and independently review approved
   exact-CID calibration regions. The schema and lint/reporting model now
   exist; existing `[[writable_region]]` entries remain codegen patch-
   insertion gates, not a universal calibration flash allow-list.
4. Bind live identity, source-image attestation, recovery-image presence,
   journal state, approved write regions, battery, ignition, checksum, and
   post-power-cycle readback into one flash-review report.
5. Resume `docs/42` only after read repeatability, exact-CID matching,
   boot-signature verification, and recovery evidence pass.

### P1 - hardware-free product quality

1. Finish Readiness as the project cockpit: definition provenance, exact-CID
   coverage, checkpoint age, changed regions, preflight blockers, and one next
   action.
2. Add a named-checkpoint browser with compare, restore, notes, provenance,
   and automatic `Stock`, `Current`, `Before Flash`, and `After Flash` flows.
3. Build the first Map Explorer slice: search, role/category, units, axes,
   risk, confidence, provenance, sibling variance, and related tables. The
   initial sidebar slice now supports text/category/safety/emissions facets
   and role/axis/notes hover metadata; richer provenance and sibling views
   remain queued.
4. Build imported-first Log Explorer: CSV profiles, signal aliases, derived
   channels, event markers, map-cell tracing, and findings export.
5. Encode the FA20-to-FA24 workflow as a staged, reversible plan. Keep the
   current five-table evidence baseline separate from hardware verification and
   from custom feature patches.

### P1 - reverse-engineering output that pays back

1. Harden the Ghidra MCP/agent pipeline around manifests, exact input hashes,
   processor selection, reset-vector seeds, public labels, decompile outputs,
   sibling fingerprints, name provenance, and mismatch reports.
2. Finish LF79 source cleanup and A8DH101I QA; analyze A8DH201X/Z and LF9C/D/G/L
   as separate families rather than transferring names by span overlap.
3. Generate scrubbed, independently authored definition candidates from only
   byte-verified facts. Keep Atlas runtime/decompiled artifacts and uncertain
   names analyst-side.
4. Add regression checks for source substitution, address drift, sibling
   mismatch, and accidental promotion of unverified metadata.

### P2 - hardware-return validation

1. Read a donor three times over a small immutable slice, then read the full
   image and hash every copy.
2. Compare exact CID, definition coverage, boot signatures, checksum facts,
   and project Stock checkpoint.
3. Run one deliberate write only after the new approved-region model and
   recovery evidence are satisfied.
4. Validate post-power-cycle durability before increasing write scope.
5. Complete the 100-cycle gate, power-loss injection, delta-flash checks, and
   patch-insertion HIL tests.

## Immediate next implementation slice

The calibration-region model, its definition lint/reporting surface, and its
offline Readiness/Flash Review binding are now implemented. The next
hardware-free code task is the richer Map Explorer metadata surface and
checkpoint lifecycle browser. Live observed-CID evidence remains a hardware
return gate.
Do not start DIY erase/program support until a known-good compatible target,
read-only H-UDI identity, and repeatable full-image read are available.
