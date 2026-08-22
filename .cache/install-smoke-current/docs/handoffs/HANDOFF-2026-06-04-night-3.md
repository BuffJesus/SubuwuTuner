# Handoff — 2026-06-04 (night session, part 3)

**`origin/main = e378ec9`** — seven commits added since the
part-2 handoff at `e9fdf84`. Tree clean except the prior
HANDOFF files. Tests stayed green throughout; 1373 cases /
~159k assertions on the final run. Both binaries link clean.

```
e378ec9 feat(ui): Compare panel chip tooltips explain Safety / Emissions / Flagged
e02c48c ci(mutation): expand advisory lane to 4 ranges (3 more files, ~80 lines)
488419d test(policy): kill 2 mutation survivors at flash_preflight battery thresholds
87d9cc0 feat(cli): dump-axis --json emits subuwutuner.dump-axis.v1 envelope
f086387 feat(ui): surface hidden sidebar categories in Settings → Project
fdd858a test(flash): kill 3 mutation survivors in BackupStore::filename_stem slugifier
6e84268 feat(cli): dump-table --json emits subuwutuner.dump-table.v1 envelope
```

---

## How the session ran

Part 2 closed at "keep going" → picked up the well-scoped items
from the part-2 handoff's "smaller items" list:

1. **CLI `dump-table --json`** — follow-up to the v1 envelope
   pattern from `rom-diff.v1` / `knock-snapshot.v1` etc.
2. **Mutation lane sweep across safety-critical modules** —
   `Flasher::read_full_rom`, `backup_store.cpp`, `checksum.cpp`,
   `flash_preflight.cpp`. Killed every actionable survivor.
3. **Settings → Project hidden-categories surface** — added a
   discoverable breadcrumb back to the sidebar Hide affordance.
4. **CLI `dump-axis --json`** — sister to dump-table.
5. **CI mutation lane expansion** — locks in the 4 ranges that
   the safety-test additions brought to 100%.
6. **Compare panel chip-filter tooltips** — the holdout from the
   `cf0e15b` glossary expansion.

No architectural sign-off was needed for any of these; the
multi-day items from the part-1 handoff remain untouched and
still want a direction call.

---

## What's on `origin/main` that wasn't there at part 2

### `6e84268` — CLI `dump-table --json`

Sister to the `--csv` mode. Envelope:

  - `table`: id, name, dimensions, address, unit, precision,
    `engine_safety_critical` (only when true),
    `emissions_relevant` (only when true)
  - `axis_x` / `axis_y` / `axis_z`: axis label arrays
  - `values`: 2D grid (1D + 2D tables), OR
  - `slices`: 3D array of 2D grids (3D tables)

Numbers at the table's scaling precision so JSON consumers see
the engineering-unit grid. `--json` + `--csv` mutex (exit 2),
matching the uniform pattern across `knock-snapshot` /
`coldstart-analyze` / `rom-diff`.

+2 integration tests pin the v1 schema keys + the mutex rejection.

### `fdd858a` — kill 3 slugifier mutation survivors

Scanning `src/flash/src/backup_store.cpp` 1-383 surfaced three
real survivors on the same slug-character classifier at lines
32 and 34. Upper-bound chars `'Z'`, `'z'`, `'9'` were never
exercised — existing tests used `"Before Stage 1!"` which
doesn't contain any character on the upper boundary of any
range.

New boundary test:

  ```
  "AZ" → "az"   (kills `c <= 'Z'` mutant)
  "az" → "az"   (kills `c <= 'z'` mutant)
  "09" → "09"   (kills `c <= '9'` mutant)
  "z{" → "z"    (sanity: '{' past 'z' still excluded)
  ```

  | Before                | After                  |
  |-----------------------|------------------------|
  | KILLED   = 12         | KILLED   = 10          |
  | SURVIVED = 3          | SURVIVED = 0           |
  | Score   = 80.0%       | Score   = 100%         |

### `f086387` — Settings → Project: hidden-categories surface

`cb921ef` shipped the sidebar Hide/Show data layer; the only
discovery route was right-clicking the panel. Settings →
Project now carries:

  - `Hidden sidebar categories: N` count line
  - Per-project `Reset` SmallButton (disabled when N == 0)
  - Tooltip explaining the Reset gesture
  - Comma-joined preview of the hidden category names underneath

Reset clears the in-memory set + persists via
`save_sidebar_hidden_categories` — the file is removed when the
set goes empty, matching the same "clean state = no file" shape
`sidebar_order.txt` uses.

Sidebar footer hint stays for bottom-of-panel discovery; the
Settings surface adds a top-of-panel one. Both routes share the
same persistence path.

### `87d9cc0` — CLI `dump-axis --json`

Same envelope shape as dump-table. Adds a `monotonic` field
mirroring the text-mode summary's `strictly increasing` /
`strictly decreasing` / `NOT monotonic — axis unusable for
lookup` check:

  ```
  "axis":{"id":"rpm_axis","unit":"rpm","precision":0,
          "length":4,"monotonic":"increasing"}
  ```

Programmatic consumers can gate on this field instead of
re-deriving it. Same `--json` + `--csv` mutex.

+2 integration tests.

### `488419d` — kill 2 battery-threshold mutation survivors

Scanning `src/policy/src/flash_preflight.cpp` 1-180 found two
real survivors on the BatteryVoltageOk validator's thresholds
at lines 71 and 79. Existing test covered three voltages but
none exactly equal to either threshold:

  ```cpp
  if (v < block_below) { ... return blocker; }    // line 71
  if (v < warn_below)  { ... return warning; }    // line 79
  ```

New boundary test pins both:

  - `11.5` (= `block_below`) → ok=true, warning only, no blocker
  - `12.0` (= `warn_below`)  → ok=true, no diagnostics

A `<=` mutation on either would erroneously trigger at the
exact threshold.

Adjacent scans this session reported 100%:

  | File                                 | Range  | Result |
  |--------------------------------------|--------|--------|
  | src/flash/src/checksum.cpp           | 1-123  | 4/4    |
  | src/flash/src/flash.cpp swap_eq only | 67-215 | 4/4    |

### `e02c48c` — CI mutation lane expansion

The advisory lane now scans four windows in series instead of
one. All four report 100% on actionable mutants locally:

  | File                                 | Lines   | What                  |
  |--------------------------------------|---------|-----------------------|
  | src/flash/src/flash.cpp              | 500-560 | brick-safe ordering   |
  | src/flash/src/flash.cpp              | 400-499 | plan validation       |
  | src/flash/src/backup_store.cpp       |  30-40  | slug-char classifier  |
  | src/policy/src/flash_preflight.cpp   |  65-90  | battery thresholds    |

Job time budget unchanged (30 min). Each range capped at
10-20 mutants. Continue-on-error remains true; the
quarter-clock for "flip to ship gate" runs on the first range.

### `e378ec9` — Compare panel chip-filter tooltips

`cf0e15b` extended glossary tooltips to the ROM A / ROM B
pickers. The chip-filter row (Safety-critical / Emissions /
Either flag) was the holdout. Each chip now has a multi-line
tooltip explaining the tag + how it interacts with the
jurisdiction profile.

Tooltips render on each chip's own hover state (via the post-
button `IsItemHovered()` check) so they attach to the chip they
describe, not the All chip that precedes them.

---

## Mutation-lane state summary

Cumulative coverage across the safety-critical surface as of
end-of-session:

  | File                                 | Range   | Score | Notes                                    |
  |--------------------------------------|---------|-------|------------------------------------------|
  | src/flash/src/flash.cpp              | 400-499 | 100%  | plan validation                          |
  | src/flash/src/flash.cpp              | 500-560 | 100%  | brick-safe ordering (part 1 survivor killed) |
  | src/flash/src/flash.cpp              | 561-650 |  100%* | * line 632 equivalent mutant skipped     |
  | src/flash/src/flash.cpp              | 651-749 | 100%  |                                          |
  | src/flash/src/flash.cpp              |  67-215 |  100%* | * line 178 equivalent mutant skipped     |
  | src/flash/src/backup_store.cpp       |  30-40  | 100%  | slug classifier (3 survivors killed)     |
  | src/flash/src/checksum.cpp           |   1-123 | 100%  |                                          |
  | src/policy/src/flash_preflight.cpp   |  65-90  | 100%  | battery thresholds (2 survivors killed)  |

Equivalent mutants documented (cannot be killed by any test):

  - `flash.cpp:178` — `remaining < max_chunk_size ? remaining : max_chunk_size`
  - `flash.cpp:632` — `remaining < block_payload ? remaining : block_payload`

Both are the canonical `a < b ? a : b` pattern where `<` and
`<=` produce identical values at every input.

---

## Test state at end-of-session

- Build: both binaries link clean
- Tests: 1373 cases / ~159k assertions (was 1367 at part-2 EOD;
  +6 new cases — 4 boundary tests killing mutation survivors,
  +2 CLI integration tests for the new JSON envelopes)
- Catch2 randomization moves the assertion count between runs;
  the case count is the stable number.

---

## Notes / gotchas the next session should know

### Equivalent mutants are not a coverage gap

Both surviving mutants in the flash module — `flash.cpp:178`
and `flash.cpp:632` — are the canonical `a < b ? a : b` /
`a <= b ? a : b` equivalent-mutant pair. **Don't write tests
trying to kill them.** No input distinguishes the two
expressions; any test asserting on the output would pass for
both branches by definition. The mutation harness can't
detect equivalence and will report these every run; the right
response is "this is the documented equivalent mutant, ignore."

### CI mutation lane is now four ranges in series

`mutation-flash` job runs each range as its own named step.
A range failing to start (BUILD_FAIL before any mutant runs)
would short-circuit just that step thanks to the `|| true`
trailer at the end of the chain. The other ranges still
execute. **Don't add a top-level `set -e`** — it would defeat
this. Per-range exit codes are surfaced in the step's status,
not the job's.

### dump-table / dump-axis precision matches scaling, not raw

Both new JSON envelopes emit `values` at the scaling-applied
engineering-unit precision (e.g. `1.234` not the underlying
0..255 byte). The `precision` field on the envelope tells the
consumer how many fractional digits the values were rendered
with. Consumers that need byte-level fidelity should use
`rom-info` + an offset/length probe, not these verbs — those
are for inspecting calibration values as the tuner sees them.

### Settings hidden-cats Reset operates on the live project

The Settings → Project Reset button calls
`save_sidebar_hidden_categories` against `state.project->dir()`.
If no project is loaded, the entire hidden-cats section is
hidden (the wrapping `if (state.project.has_value())` is the
gate). Don't add a project-less path — without a project there's
no `<project>/sidebar_hidden.txt` to write to.

### Compare panel tooltip pattern: post-button IsItemHovered

`chip_button` is a closure that ends with `SameLine()`. To
attach a tooltip to a specific chip, the `IsItemHovered()`
check goes AFTER the chip's call but BEFORE the next chip's
call — `IsItemHovered` reads the most-recent item, which is
the chip just rendered. Putting the tooltip inside the closure
would attach it to the SameLine spacer, not the button. This
is a deliberate pattern; don't refactor `chip_button` to take
a tooltip arg.

### Existing carry-over notes still apply

From parts 1 + 2:

- Per-ROM history legacy-read fallback stays for 3+ months
- `pack_id`-keyed pack-lint cache drop on pack repoint is by design
- Sidebar hide-check runs before filter-check
- F1 routing depends on every panel calling `track_help_context`
- Welcome chip is silently no-op when project open fails
- First-Run wizard focus tracker is step-equality cache
- Mutation harness skips quoted strings; doesn't skip raw strings

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)

No change. The mutation lane is now broader (4 ranges) and the
safety-critical surface is closer to "ship gate" status across
the board.

### Real-world unblockers (unchanged)

1. Bench-rig hardware
2. RH850 codegen bench validation

### Things to ship next (no specific user direction)

Same big-architectural items as parts 1 + 2 — none touched this
session:

- `src/diff/` + Compare panel rebuild (#4)
- `VehicleProfile` (#7)
- Cross-session `AuditLog` (#8)
- AI Tier 2 LLM narration (`docs/20`)
- Multi-ROM cross-ROM diff
- Live-to-table cross-reference + knock overlay (#15/#16)

### Smaller items the next session could pick up

- **Mutation lane: more files** — `src/transport/` (DVI codec,
  SSM framer), `src/ecu/uds.cpp` (NRC dispatch). All have tight
  unit tests; the lane would likely find boundary-condition
  survivors similar to backup_store / battery thresholds.
- **More CLI `--json` envelopes** — `rom-info` is the next
  obvious candidate (currently text-only); `pack-info` already
  has `--json`.
- **Compare panel UX** — search/filter in the change list,
  pinned-table starring (matches audit-panel pin pattern).
- **Hidden-cats sidebar GUI** — Settings now surfaces the count
  + Reset; the sidebar's footer hint could grow click-to-restore
  per-category buttons.
- **Help modal table-of-contents** — for long topics like
  `08-testing-strategy`, a left-side TOC of `##`-headings
  would make navigation easier than the current Ctrl+G find.

### Watch-list

Same as parts 1 + 2, plus:

- **CI mutation job runtime** — 4 ranges × ~5 min each = ~20 min,
  still under the 30-min budget. Adding a fifth range pushes
  past 25 min and runs the risk of a timeout on slow runners.
  Consider parallelizing (matrix strategy with `max-parallel`)
  before adding a fifth.
- **Mutation harness `swap_eq` false-negative scope** — string-
  literal skip handles 90% of `!=` / `==` glyphs in error
  messages but the broader `==` in code comments still gets
  mutated (the `//` skip only covers leading-comment lines,
  not trailing `code; // comment` cases). Low priority but
  worth a future enhancement if reports get noisy.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# Try today's new JSON envelopes
./build/win-mingw/bin/subuwutuner-cli.exe dump-table \
    --def fixtures/demo-pack/pack.toml \
    --table boost_target_high_octane \
    --json fixtures/demo.stune/source.bin

./build/win-mingw/bin/subuwutuner-cli.exe dump-axis \
    --def fixtures/demo-pack/pack.toml \
    --axis rpm_axis \
    --json fixtures/demo.stune/source.bin

# Visit the GUI for the Settings hidden-cats + Compare tooltip surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   Sidebar → right-click a category → Hide
#   Tools → Settings → Project tab
#     "Hidden sidebar categories: 1  [Reset]"
#     <category name>
#   View → Compare → hover Safety-critical / Emissions / Either flag
#     tooltip explains the tag

# Re-run all four CI mutation ranges locally
for r in "400 499" "500 560"; do
  python tools/mutation_test.py --file src/flash/src/flash.cpp \
    --line-start ${r% *} --line-end ${r#* } \
    --target st_unit_tests --tag-filter "[flash]" \
    --max-mutants 20 --build-dir build/win-mingw \
    --mutations swap_eq,swap_lt,swap_gt
done
python tools/mutation_test.py --file src/flash/src/backup_store.cpp \
    --line-start 30 --line-end 40 \
    --target st_unit_tests --tag-filter "[backup_store]" \
    --max-mutants 15 --build-dir build/win-mingw \
    --mutations swap_eq,swap_lt,swap_gt
python tools/mutation_test.py --file src/policy/src/flash_preflight.cpp \
    --line-start 65 --line-end 90 \
    --target st_unit_tests --tag-filter "[policy]" \
    --max-mutants 10 --build-dir build/win-mingw \
    --mutations swap_eq,swap_lt,swap_gt
```

---

## TL;DR

> Seven commits past the part-2 handoff; everything on
> `origin/main = e378ec9`. Two new CLI JSON envelopes
> (`dump-table.v1`, `dump-axis.v1`); five real mutation
> survivors killed across `flash.cpp` / `backup_store.cpp` /
> `flash_preflight.cpp`; CI mutation lane expanded from 1
> range to 4 (all reporting 100% on actionable mutants);
> Settings → Project surfaces hidden-cats count + Reset;
> Compare chip filters get tooltip coverage.
>
> Cumulative safety-critical mutation coverage now spans
> `flash.cpp` `backup_store.cpp` `checksum.cpp` and
> `flash_preflight.cpp` at 100% on actionable mutants. Two
> documented equivalent mutants remain (`flash.cpp:178`,
> `flash.cpp:632`) — both the canonical `a < b ? a : b`
> pattern, not killable by any test.
>
> Big architectural items (`src/diff/` Compare panel,
> VehicleProfile, cross-session AuditLog, AI Tier 2 narration)
> still untouched and still want green light.
>
> Off you go.
