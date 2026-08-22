# Product direction: the tuning instrument

SubuwuTuner should feel like a trusted instrument: calm before it is clever,
legible before it is dense, and beautiful because every visual decision has a
job. The design target is the meeting of the classical and the romantic:
classical hierarchy, restraint, proportion, and repeatability; romantic
warmth, craft, character, and respect for the person using it.

**Current implementation note (2026-08-02):** the offline-first direction is
active while the LF79002P bench ECU remains silent. Readiness and Flash Review
now expose approved exact-CID calibration coverage without treating it as live
ECU proof, and the Tables sidebar has the first Map Explorer facets for
category and safety/emissions discovery.

## What is already working

The current UI has unusually good foundations for this direction:

- Tune, Log, and Features are distinct workspaces rather than a wall of tabs.
- The welcome panel gives one primary action, sensible secondary actions, and
  a demo path without pretending hardware is present.
- The table editor has keyboard navigation, heatmap/grid views, undo/redo,
  history, statistics, clipboard interop, and source/working-ROM awareness.
- Safety is expressed in plain language: policy profiles, engine-safety blocks,
  typed flash confirmation, audit entries, and a read-only verification path.
- The command palette, contextual help, glossary tooltips, and F1 routing make
  the dense parts discoverable without making the first screen dense.

The product should preserve those qualities while making the user's intent and
the consequences of an edit visible earlier.

## UX principles

### Intent before mechanism

Users should be able to say “I want to correct fueling,” “I want to understand
this knock,” or “I want to review what will be flashed.” They should not have
to begin by knowing a table ID, an ECU protocol, or a menu location.

### Calm density

Numerical work deserves density; orientation does not. Use generous spacing,
short labels, and progressive disclosure around the grid. Reserve saturated
color for meaning: selected, changed, caution, blocked, or verified.

### Every mutation has a story

Before and after values, source, reason, affected tables, policy status, and
reversal must be available from the same surface. A tune is not merely a
modified binary; it is a sequence of understandable decisions.

### Confidence is part of the data

A table name, scaling, address, live signal, or AI suggestion should carry a
visible provenance/confidence state. “Known,” “inferred,” “candidate,” and
“unverified” are different user experiences and must not look identical.

### Hardware absence remains a productive mode

The entire inspect → compare → edit → validate journey should remain useful
with no ECU connected. Hardware-dependent actions should be explicit endpoints,
not the organizing metaphor of the application.

## Highest-value missing functionality

### P0 — Project overview / readiness surface

When a project opens, provide a quiet overview card or “Project” panel showing:

- vehicle profile and calibration ID;
- definition-pack identity, version, and lint status;
- source/working ROM identity and last-save state;
- changed-table count and byte/sector count;
- checksum status and whether the family strategy is implemented;
- policy profile and any hard blockers;
- one recommended next action: Inspect, Compare, Edit, Validate, or Export.

This is the missing orientation layer between the welcoming launch screen and
the expert Tune workspace. It should not compete with the table editor; it
should answer “where am I and what is safe to do next?”

### P0 — Semantic pre-flash review

The flash review now includes a named changed-table preview with cell counts,
maximum engineering-unit delta, safety/emissions markers, and click-through to
the table editor. The next increment is a compact before/after summary for the
currently attested ROM, checksum result, and a clear distinction between:

- “this project is internally valid”;
- “this plan is policy-approved”; and
- “a connected ECU has been identified and is eligible.”

These are separate claims and should never collapse into one green checkmark.

### P1 — Guided tuning tasks

Add task cards that compose existing panels without hiding the expert surfaces:

1. Baseline and identity check.
2. Fueling / MAF.
3. Boost response.
4. Ignition and knock.
5. Cold start and drivability.
6. Review, compare, export.

Each task should show inputs, expected evidence, a reversible suggested edit,
and a completion note. It should link to table roles when the pack knows them;
otherwise it should say that the mapping is not yet verified.

### P1 — Table context and confidence

The table header should answer four questions without opening Help:

- What does this control?
- What are the axes and units?
- What is the source of this definition?
- What is the risk of changing it?

This is where the classical/romantic balance matters: a small, beautifully
typeset “field note” beside the grid is more useful than another toolbar icon.

The analyst-side Atlas table export confirms the metadata shape this surface
needs: domain hierarchy, unit/scaling, axis meaning, memory region, execution
metadata, and explicit advanced/beta/dangerous/regulated flags. These should be
adapted through a provenance-aware QA layer, never treated as silent authority.

### P1 — Named checkpoints

Add lightweight project checkpoints such as `stock`, `baseline`, `after-maf`,
and `ready-to-flash`. A checkpoint is a content hash plus a human note, not a
second mutable ROM. It makes Compare and Undo feel intentional and gives the
tuner a recoverable narrative across a long session.

### P1 — Tune-library attestation

Finish the local tune-library index and AP-side attestation so the user can
answer “what is currently installed?” from a backup checksum or ROM identity,
then compare it against named local artifacts. This is more valuable than a
generic file browser because it converts a folder of tunes into a trustworthy
history.

### P2 — Close the loop after hardware returns

Once the rig is repaired, prioritize read-back identity, checksum repair,
delta-flash, power-loss recovery, and live signal validation before live
editing. Live tuning and custom patch insertion remain separate gates; neither
should be allowed to borrow confidence from static decompilation.

## Offline ROM / RE expansion

The current supported research corpus covers the VA SH-2A and VB RH850 WRX
families, with named Ghidra/index/decompilation artifacts for the existing
family passes. The private plaintext corpus is much larger. The current
inventory matched **126 plaintext image files across 28 exact-CID definition
packs**; the complete queue is generated at:

`D:\Subuwu\findings\decompile\ROM_CORPUS_RESEARCH_QUEUE.md`

Recommended order:

1. **EP5G600A** — finish the existing older-family analyst seed and create the
   first reusable non-WRX baseline.
2. **EZ1G / A8DH** — A8DH101I has 24 sibling images and EZ1G has a strong
   multi-image NA Forester corpus. These are ideal for signature naming,
   address drift, and definition confidence work.
3. **LF79100P / LF79101P** — use the 25-image LF79 subset to resolve the
   remaining VA-family identity, flash-gate, and live-signal seams without
   projecting names into RH850.
4. **LF9C / LF9D / LF9G / LF9L** — continue only where the result improves a
   tuning workflow, checksum implementation, or table-role confidence.

The rule for expansion is simple: a family may be decompiled and named as
research, but it does not become a writable tuning pack until architecture,
checksum, table bytes, writable regions, and provenance are independently
verified.

## Visual language to protect

- Keep the purple accent as a restrained thread, not a neon theme.
- Let typography and spacing establish hierarchy before adding more icons.
- Prefer labels such as “Review changes” and “Inspect table” over “Diff” and
  “Edit” when the user's intent is clearer in plain language.
- Use animation only for state transitions or progress; never for decoration
  during numerical work.
- Make every disabled action explain itself in-place, especially when the
  reason is “not verified yet” rather than “not implemented.”
- Treat audit/history as part of the beauty of the tool: a clear record is a
  form of craftsmanship, not bureaucratic chrome.

## Near-term implementation sequence

1. Semantic pre-flash preview — landed in the flash review modal.
2. Project overview/readiness panel — hardware-independent and next.
3. Definition confidence/provenance field note in the table header.
4. Named checkpoints and “compare to checkpoint.”
5. Tune-library attestation and pre-flash identity matching.
6. EP5G, EZ1G, and the byte-verified LF79100P analyst baseline are complete;
   A8DH101I is now the active cross-sibling baseline, with LF79 source cleanup
   continuing separately.
