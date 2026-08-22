# Editing a table

The full table-editor operation set. Every operation goes through
`st::edit::History`, which means each operation is undoable, replayable,
git-diffable in `edits.toml`, and tagged so workflow batches collapse
into single "Revert all" actions.

## Selecting

| Action | How |
|---|---|
| Single cell | Click |
| Row / col | Click row/col header |
| Rect | Drag |
| Disjoint cells | <kbd>Ctrl</kbd>+click |
| All | <kbd>Ctrl</kbd>+<kbd>A</kbd> |

## Atomic operations

| Operation | What it does |
|---|---|
| **Set value** | Type a value into a cell, <kbd>Enter</kbd> |
| **Scale (×)** | Multiply every selected cell by a factor (e.g., 1.02 = +2%) |
| **Offset (+)** | Add a delta to every selected cell |
| **Smooth** | Gaussian smoothing across the selection (configurable kernel) |
| **Interpolate** | Linear interpolation between selected anchor cells |
| **Paste** | Paste from clipboard / another selection (shape-checked) |
| **CSV import / export** | Round-trip the selection through CSV |

Every operation lands in `edits.toml` as a structured record (`scale_mul`,
`offset_add`, `smooth`, etc.) with the rect bounds, parameters,
timestamp, and tag.

## Undo / redo

| Key | Action |
|---|---|
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> | Undo one step |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd> | Redo |
| History panel | Jump to any prior state |

The history is a tree, not a stack — branching off a prior state
creates a new branch and the old branch remains reachable until you
prune it.

## Workflow batches

Operations tagged with the same workflow tag undo as a single unit via
`History::undo_while_tag`. The FA24-swap modal is the canonical
example: a multi-table edit batch tagged `"fa24_swap"` reverts in one
action.

Tags also drive the audit log — every flash, every export, every
push-to-AP records which tag groups are in scope.

## Policy: edit-time warnings

Jurisdiction profile (set on first run, changeable in Settings) gates
edit-time warnings on emissions-flagged tables:

| Profile | Behavior |
|---|---|
| `motorsport-only` | No warnings — your shop, your car, your rules |
| `alberta-ca` | Warn on tables emissions-flagged at federal level |
| `eu-roadworthy` | Warn on EU-Euro-* flagged tables |
| `california-us` | Warn on CARB-flagged tables |

Profiles never **refuse** an edit — engine-safety refusals do, but
regulatory refusals don't. Reasoning:
[`docs/06-legal-ethics.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/06-legal-ethics.md){ target="_blank" }.

## CSV round-trip

```bash
# Export a table to CSV
subuwutuner-cli dump-table --def path/to/pack-dir/ \
                           --table fuel_main \
                           --format csv \
                           path/to/working.bin > fuel_main.csv

# Round-trip: edit in your spreadsheet, paste back into the GUI
```

The CSV layout matches the heatmap view — axes as header row / column,
cells as scaled values.

## Deeper detail

- [`docs/02-architecture.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/02-architecture.md){ target="_blank" }
  §`st::edit` — Rect, Snapshot, History semantics.
- [`docs/12-auto-tuning.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/12-auto-tuning.md){ target="_blank" }
  — auto-tune kernels (MAF, knock-pull) that emit `History` proposals
  you can preview before commit.
- [`docs/39-tuning-knowledge-atlas.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/39-tuning-knowledge-atlas.md){ target="_blank" }
  — tuner-atlas: corpus-derived tuning-knowledge surface for the GUI's
  Tuner Atlas sidebar group.
