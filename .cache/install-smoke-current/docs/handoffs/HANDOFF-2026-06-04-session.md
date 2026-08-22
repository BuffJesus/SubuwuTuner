# Handoff — 2026-06-04 (polish + CI sprint)

**`origin/main = 8a55667`** — six commits pushed today (range
`b661c88..8a55667`). Tree clean except the HANDOFF-2026-06-0{1..4}*.md
files. Tests green at every commit; ~159k assertions across 1361
test cases. Both binaries link clean.

```
8a55667 ci(defgen): freeze matrix job (Win/macOS/Linux) — closes blocker #7
a1a5f42 fix(cli): reject --json --csv; flat phase keys in coldstart --csv
5b70cb6 feat(ui): settings modal validate-pack button
33f167f feat(ui): sidebar reset-category-order via right-click
5797fc3 feat(ui): audit panel bulk pin/unpin popup
2e643ea feat(cli): --csv on knock-snapshot and coldstart-analyze
```

---

## What's on `origin/main` that wasn't there at session start

Picked up "Things to ship next" from `HANDOFF-2026-06-03-session.md`
and walked the low-risk items end-to-end.

### CLI — `--csv` envelope on the two analyze verbs
- `knock-snapshot --csv` — `#`-prefixed metadata header (schema /
  log / window / sample-rate / samples / gate / cylinder_count)
  followed by per-cyl rows. Mirrors the `--json` envelope.
- `coldstart-analyze --csv` — same shape. Meta includes ECT span,
  sample counts, mean lambda dev, plus flat `phase_<name>_samples=`
  / `phase_<name>_seconds=` keys (was originally bracketed
  `phase[<name>].samples=` but `[…]` is a section marker in
  configparser/dotenv/shell-source readers — caveman review caught
  it before push).
- `--json --csv` together is rejected with exit 2 + "mutually
  exclusive" stderr message (also a review catch — earlier-return
  ordering was an implementation accident, not precedence).
- `--csv` joins `kCommonFlags` for bash/zsh completion.
- Integration tests for both `--csv` shapes via the existing
  subprocess fixture.

### GUI — audit panel bulk pin/unpin
- New "Bulk pins ▾" button next to the existing Pinned filter
  toggle opens a popup with **Pin N visible / Unpin N visible /
  Clear all pins (N)**. Counts snapshot at popup open and rendered
  into labels.
- Scope = current text + chip filter only (intentionally ignores
  the time-range slider which lives below the toolbar — toolbar
  context = text/chip only).
- Each item disabled when its target set is empty so the popup is
  discoverable even on a fresh log.

### GUI — sidebar reset-category-order
- Right-click anywhere in the Tables panel opens a context menu
  with **"Reset category order"** + a subtle "(no custom order
  saved)" hint when there's nothing to reset.
- Clears `state.sidebar_category_order` and removes
  `<project>/sidebar_order.txt` via `std::filesystem::remove`
  (errors non-fatal — in-memory clear is what drives live UI).
- `ImGuiPopupFlags_NoOpenOverItems` scopes the popup to the
  empty area + header strip, leaving room for future per-row
  context menus on table rows.

### GUI — Settings → Validate pack
- New "Active pack" section above the Save row. Runs
  `Definition::validate()` on the currently loaded project's pack
  and renders the result inline (per the modal-inline-errors
  convention — never the bottom status bar).
- Cached result in `AppState` (status / pack id / message body) so
  flipping the modal closed + open keeps the last verdict visible
  until the user re-validates.
- Bounded scrollable child renders the full violation list so a
  60-line dump doesn't push Save off-screen.
- Equivalent to `subuwutuner-cli pack-lint <pack>` — same logic,
  GUI surface for tuners who don't drop to a terminal.

### CI — `defgen-freeze` matrix job (closes pre-1.0 blocker #7)
- New `.github/workflows/ci.yml` job alongside the existing
  `defgen` (source-mode tests). Runs on Windows / macOS / Linux,
  each producing one frozen binary via `tools/defgen/defgen.spec`.
- Smoke step mirrors the source-mode `defgen` test: convert
  `minimal_rom.xml` through the frozen binary, re-parse with
  `tomllib`. Catches both spawn failures (bad PyInstaller bundling
  / missing data/ dir) and silent dep drops (malformed TOML).
- Artifacts uploaded as `defgen-{windows-x64,macos-arm64,
  linux-x64}` with 14-day retention. Release pipeline can grab
  the three artifacts and bundle each into its installer.
- Locally verified end-to-end against PyInstaller 6.19.0 on
  Python 3.14: dist/defgen.exe + minimal_rom.xml → valid TOML.
- `tools/defgen/build/` + `tools/defgen/dist/` added to
  `.gitignore` so devs running pyinstaller locally don't leave
  a dirty tree.
- `docs/04-roadmap.md` blocker #7 flipped 🟡 → ✅.

### Housekeeping
- Removed stale `build/win-mingw/bin/subarutuner-cli.exe` — leftover
  artifact from before the rename. Confirmed there's no CMake
  target with the old name, so no source changes needed.

---

## Test state at end-of-session

- Build: both binaries link clean
- Tests: 1361 cases / ~159k assertions, all pass (was 1359 at
  session start; +2 new `[cli][integration][csv]` cases)
- Local CI sanity: yaml syntax check on the new `ci.yml` passes;
  the `defgen-freeze` smoke step was verified against the frozen
  binary locally (Python 3.14 + PyInstaller 6.19.0)
- Live CI run for `8a55667` not yet inspected — `gh` CLI isn't on
  PATH on this machine. Visit
  https://github.com/BuffJesus/SubuwuTuner/actions for status.

---

## Notes / gotchas the next session should know

### `--json --csv` precedence
Both `knock-snapshot` and `coldstart-analyze` reject the combo at
exit 2 with a "mutually exclusive" message. Don't add a "pick one
silently" code path back in — the rejection is the contract.

### Coldstart `--csv` phase meta uses flat keys
`phase_PreCrank_samples=` / `phase_PreCrank_seconds=`, not the
bracketed `phase[…]` form. configparser-style readers treat `[…]`
as section headers; the underscore form survives round-trip
through every CSV-meta consumer I tested. Keep this shape stable
unless we ship a v2 schema.

### Settings → Validate pack uses `Project::definition()`
Because the project model holds a resolved `Definition`, the GUI
button doesn't need a separate file picker — it acts on whatever
pack the current project loaded. If a future feature wants to
lint an arbitrary off-disk pack (without opening it as a project),
reuse the `pack-lint` CLI verb path instead.

### Audit bulk-pin scope = text/chip filter only
Intentional — the time-range slider lives below the toolbar where
"Bulk pins" lives. The toolbar's mental model is "what the toolbar
above is filtering by." If a future range-aware bulk pin is wanted,
move the range slider into the toolbar first.

### Sidebar reset uses `BeginPopupContextWindow` + `NoOpenOverItems`
This means the popup only fires on right-click over the empty area
+ TreeNode headers. Per-row context menus on table rows would use
their own `BeginPopupContextItem` calls and stay unblocked. Don't
remove the flag without auditing whether anything per-row would
collide.

### `defgen-freeze` job depends on `defgen` (source tests)
`needs: defgen` keeps freeze serial. Don't parallelize — pointless
to freeze a binary whose source tests are broken.

### PyInstaller binary writes output relative to its CWD
Smoke step in CI uses `-o syn.toml` (CWD-relative) on purpose.
`-o /tmp/syn.toml` works on POSIX runners but breaks on the Windows
runner because the frozen .exe resolves `/tmp/` as a Windows path
inside Git Bash, while the follow-up Python call sees the MSYS
mount-mapped path. CWD-relative dodges both views.

### Drift trap in Settings → Validate pack violation counter
The `int violations` counter in `settings.cpp` line-counts the
multi-line message from `Status::to_string()`. Currently safe
because `defs.cpp` joins violations with `\n` without a trailing
newline. If the validator ever grows a trailing newline the count
will be off-by-one. Worth a guard if anyone refactors the message
shape.

### Existing carry-over notes from 2026-06-03 still apply
- `active_rom_mut() → nullable` contract on edit paths
- Per-ROM `<id>.edits.toml` filename collision risk (low priority)
- `audit.pinned` sidecar is best-effort
- AI drift classifier — advisory only, no auto-apply channel ever
- `ai-drift` and `adaptive-history` deliberately don't share a
  CSV parser
- `BeginPopupModal` Z-order pathology for startup-fired modals
- Help modal find-mode degrades structure

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)
- #7 (defgen frozen) — ✅ done today
- All other previously-blocking entries from yesterday's list
  remain closed
- Re-read `docs/04` against today's state if you're about to ship
  the v1.0 cut

### Real-world unblockers (unchanged)
1. **Bench-rig hardware** — junkyard 2017 WRX ECU still pending
   per memory `project_bench_rig_awaiting_ecu.md`. If it's arrived
   (it's now 2026-06-04, ETA was 2026-05-27/28), opening the
   Findings/ HANDOFF and walking `docs/28` unblocks Phase 4 HIL.
2. **RH850 codegen bench validation** — still gated on the rig.

### Things to ship next (no specific user direction)
- **AI Tier 2 LLM narration** (`docs/20`). `ai-drift --json`
  parser has headroom for `--narrate` / model flags; engine call
  site is unstubbed. Larger lift — needs network + advisory
  boundary care. The Tier 1 classifier's "advisory only, no
  auto-apply" contract MUST carry forward to any narrator — an
  LLM that suggests writes through the same surface would
  collapse the distinction.
- **`histories/` subdirectory for per-ROM edit files** — still
  loose at project root (one `<id>.edits.toml` per slot). Not
  urgent until >5 additional ROMs but the cleanup is cheap.
- **Sidebar drag-reorder** could grow per-category visibility
  toggles (hide a category outright rather than collapse) — would
  cap the long-tail of rarely-touched categories cleanly.
- **Audit pin export** — pinned-entry NDJSON export (subset of the
  existing "Export NDJSON…" button) would round out the bulk-pin
  workflow for "I starred these for the e-tuner."
- **GUI pack-lint persistence** — the cached validate result in
  Settings is session-only. Persisting to `<project>/pack-lint.toml`
  with last-validated-at + last-validated-by would let the welcome
  panel surface "Pack validated ✓ <date>" on project open without
  the user clicking through Settings.

### Watch-list for cumulative complexity
- Per-ROM history files scale with N additional ROMs (still ≤2
  in any project I've seen). `histories/` subdir is the obvious
  next step but not urgent.
- CLI integration tests subprocess-launch the binary; Windows
  process-launch cost will make these slower as more verbs join.
  Today's +2 `[csv]` cases keep us well under 5s; revisit if it
  climbs past 30s.
- Help modal find-mode rebuilds the line list per frame from the
  body — cheap on existing doc content, would warrant a cache if
  many more docs join.
- Settings modal is growing tall (now: paths / theme / profile /
  project metadata / pack-lint). If it hits the
  `AlwaysAutoResize` ceiling on small displays, consider tabbed
  sections.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# Try today's new CLI surfaces
./build/win-mingw/bin/subuwutuner-cli.exe knock-snapshot \
  --log fixtures/demo-knock-log.csv \
  --flkc-cols flkc1,flkc2,flkc3,flkc4 \
  --fbkc-cols fbkc1,fbkc2,fbkc3,fbkc4 \
  --csv

./build/win-mingw/bin/subuwutuner-cli.exe coldstart-analyze \
  --log fixtures/demo-coldstart-log.csv \
  --timestamp-col ts --ect-col ect --rpm-col rpm \
  --observed-lambda-col obs --csv

# Verify the rejection branch
./build/win-mingw/bin/subuwutuner-cli.exe knock-snapshot \
  --log fixtures/demo-knock-log.csv \
  --flkc-cols flkc1,flkc2,flkc3,flkc4 --json --csv
# expect: exit 2 + "knock-snapshot: --json and --csv are mutually exclusive"

# GUI visual check of today's new surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   Right-click in Tables sidebar      → "Reset category order"
#   View → Audit, click "Bulk pins ▾"  → Pin N / Unpin N / Clear all
#   Tools → Settings → "Validate pack" → inline OK chip or violations

# Local PyInstaller smoke (mirrors CI)
pip install pyinstaller
cd tools/defgen && pyinstaller defgen.spec --clean --noconfirm
./dist/defgen.exe tests/fixtures/minimal_rom.xml -o /tmp/syn.toml
python -c "import tomllib; print(list(tomllib.loads(open('/tmp/syn.toml').read()).keys())[:5])"

# Check today's CI run for the new freeze job
# https://github.com/BuffJesus/SubuwuTuner/actions
```

---

## TL;DR

> Six commits today; everything pushed to `origin/main = 8a55667`.
> Closes pre-1.0 ship blocker #7 (defgen frozen) and the remaining
> five "things to ship next" polish items from yesterday's handoff
> (--csv outputs, audit bulk pin, sidebar reset, settings validate,
> stub binary cleanup).
>
> Two items survived the trim: AI Tier 2 LLM narration (bigger
> lift, needs network + advisory boundary care) and the
> `histories/` subdirectory for per-ROM edits (not urgent).
>
> Real-world unblocker if the junkyard ECU arrived: open the
> Findings/ HANDOFF and walk `docs/28`.
>
> Watch `--json --csv` precedence on any future analyze verb (reject
> exit 2, never silently prefer one). Watch the deliberate
> `phase_<name>_samples=` flat-key convention on coldstart-analyze
> CSV meta — bracket form breaks configparser-style consumers.
>
> Off you go.
