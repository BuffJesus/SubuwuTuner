# Your first tune

A scripted end-to-end walkthrough of the demo project. Touches every
core surface — load → inspect → edit → undo → save → review the diff —
without any hardware or risk.

!!! note "Page in progress"
    The walkthrough text below is the v0 outline. Screenshot pass and
    deeper per-step commentary land in a follow-up. The
    [Quickstart](quickstart.md) covers the same path in shorter form.

## What you'll do

1. Open the demo project in `subuwutuner-gui`.
2. Inspect the **Fuel Main** table from the sidebar.
3. Apply a 2% scale to a selected region.
4. Diff your project against its source ROM.
5. Persist to disk and inspect the resulting `edits.toml`.

## Step 1 — Open the demo

Launch the GUI and click **Try the demo project** on the welcome panel.
The repo's `fixtures/demo.stune/` opens with the synthetic
`fixtures/demo-pack/` definition pack already wired in.

## Step 2 — Inspect Fuel Main

Use the sidebar's **Fuel** group to find **Fuel Main**. The table view
opens with the heatmap renderer.

Hover any cell to see raw and scaled values, the axis labels (RPM × MAP
on this synthetic table), and the unit (AFR target).

## Step 3 — Scale a region by 2%

1. Drag-select a rectangular region in the table.
2. Right-click → **Scale → Multiply by → 1.02**.
3. The selection lights up as edited; the History panel records a
   single `ScaleMul(1.02)` operation with the rect bounds.

## Step 4 — Diff against the source

Workspace → **Compare**. The Compare panel renders:

- **Per-cell delta** as a heatmap.
- **Aggregate stats** — mean / min / max / sigma of the delta across
  the changed cells.
- A toggle to display the diff in raw bytes or scaled units.

## Step 5 — Save and inspect the TOML

Press <kbd>Ctrl</kbd>+<kbd>S</kbd>. Open
`fixtures/demo.stune/edits.toml` in any editor.

```toml
[[edit]]
op    = "scale_mul"
table = "fuel_main"
rect  = { row_start = 3, row_end = 6, col_start = 4, col_end = 9 }
value = 1.02
tag   = "user"
ts    = "2026-06-19T18:42:11Z"
```

Each edit is one TOML record. They replay deterministically on project
open, which is why you can hand-edit this file and the GUI picks the
result up cleanly.

## What this proves

- Edits go through `st::edit::History` (undoable, tagged, persisted).
- The diff is structural — per-cell delta, not byte delta — because
  the definition tells the table editor what each cell means.
- The flow is identical against a real ROM. The only thing that changes
  is which definition pack and which `source.bin` you point the project
  at.

## Next

→ [Concepts → Overview](../concepts/overview.md) for the mental model
behind the surfaces you just touched.

→ [Workflows → Editing a table](../workflows/editing-tables.md) for the
table-editor operations not covered in this walkthrough (smooth,
interpolate, paste, CSV import/export, etc.).
