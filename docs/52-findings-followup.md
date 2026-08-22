# 52 - Findings follow-up: UI / UX / functionality

Updated 2026-08-22. Actionable items mined from the analyst findings tree
(chiefly `findings/tuning-knowledge-2026-06-13/` and
`findings/def-corrections-2026-08-21/`) that improve the tuning app. Status
reflects what is wired in the current tree.

## Status legend

- `[x]` shipped / mechanism in tree
- `[~]` partially done, or mechanism shipped but needs off-tree data to show
- `[ ]` queued

## Correctness

- `[x]` **Firmware-true axis lengths.** ~15 VA axis classes (wastegate, boost,
  AVCS ignition, requested torque, injectors, EGR, ...) are under-declared in
  the RomRaider XML by an arithmetic bug that halves `sizex`/`sizey`, so the
  editor renders half the cells of the most-tuned tables. `tools/defgen`
  gained `axis_corrections.py` + a bundled
  `corrections/axis_length_corrections.tsv` (16 firmware-verified axis
  lengths) + a `defgen --axis-corrections [TSV]` flag that overrides axis
  lengths by address at generation time (correcting an axis auto-fixes every
  table that shares it). 8 unit tests. **To apply to real VA packs, regenerate
  them with the flag** — the source XML + packs are off-tree (Path B).
  - `[ ]` Axis *orientation* (which axis is X vs Y) is also wrong on some core
    tables per `def-corrections-2026-08-21/README.md`; not yet addressed.

## UI / UX

- `[x]` **Dead-weight / factory-immutable collapse.** The corpus shows a
  118-table set no tuner ever edits, plus 12 AVCS baro-comp tables (~7 kB) that
  are pure dead weight. Added `AtlasTable.factory_immutable` (+ loader + test);
  the Tables sidebar hides these by default (panel-menu toggle "Hide
  factory-immutable tables", a hidden-count footer, never hides the selected
  row). Cuts the surface from hundreds of rows to the tuning-relevant core.
  - `[~]` Needs the atlas file to actually mark tables `factory_immutable`
    (populate from the corpus 118-table list in the atlas generator). Inert on
    packs without an atlas (e.g. the demo), by design.
- `[x]` **Common-core surfacing.** Already shipped: the sidebar draws a purple
  ★ next to `common_core` tables (the 50-table minimum tuning surface) via the
  Atlas.
- `[x]` **Safety co-edit pairs.** Already shipped: the table editor surfaces
  `Atlas::safety_pairs_for_table` with severity color + rationale (boost-target
  / wastegate, HPFP valve-close-limit-without-base, etc.).
- `[ ]` **Marketing-label dedup.** COBB `+Redline` ≡ `+SF` are byte-identical
  at Stage 1+ (diverge only in one 256-byte MAF-scale region). The Tune Library
  could collapse byte-identical variants so they don't read as distinct tunes.
- `[~]` **"Currently flashed" pin + tune-shaped panels** (findings A1/L2). The
  AP Browser / Library "currently flashed" indicator and the tune-shaped (vs
  filesystem-shaped) panel direction are reported shipped in the findings; not
  re-verified here.

## Functionality

- `[~]` **Live tuning (422-DID WDBI catalog).** SID 0x2E write-DID catalog
  (round-63) is exposed through the CLI (`subaru-live-tune`). A GUI live-edit /
  Gauge Cluster consumer is future work.
