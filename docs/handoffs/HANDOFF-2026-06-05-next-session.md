# Handoff — for the 2026-06-05 session (or whenever next opens)

**`origin/main = 9de554d`** — written at the end of an extended
2026-06-04 night session that ran across four sprints. Tree clean
except the five HANDOFF files from this session arc. Tests green;
1374 cases / ~159k assertions on the final run. Both binaries
link clean.

This is the consolidated entry point. The four sprint-level
handoffs (`HANDOFF-2026-06-04-night{,-2,-3,-4}.md`) carry the
detailed per-commit breakdowns; this doc gives the bird's-eye
view + the gotchas that cut across sprints. **Read this one
first.**

---

## What shipped (entire 2026-06-04 night arc)

22 feature/test/CI commits across four sprints, all green at
each checkpoint. Range `b648e7f..c324628` (the 4 handoffs sit
at `1ce0a4d`, `e9fdf84`, `dee988d`, `9de554d`).

### Sprint 1 — quick wins (4 commits)

```
b648e7f refactor(project): per-ROM histories under histories/ subdir
037bbd0 feat(ui): persist pack-lint snapshot to <project>/pack-lint.toml
cb921ef feat(ui): sidebar per-category Hide/Show
005fb9a feat(cli): rom-diff --json emits subuwutuner.rom-diff.v1 envelope
```

### Sprint 2 — mid-size UX + CI (5 commits)

```
32167f0 feat(ui): tabify Settings modal (Paths / Theme / Profile / Project / Pack)
cf0e15b feat(ui): extend glossary tooltips to log/ROM panels + modals
dcfee4a feat(ui): F1 per-panel help context routing (closes analyst QW-I)
187d6db feat(ui): accessibility first pass — modal initial focus + Enter shortcut
0f8f171 ci(mutation): advisory mutation-testing lane over st::flash
```

### Sprint 3 — mutation kills + harness fix + welcome polish (5 commits)

```
92d6d2b test(flash): kill mutation survivor at flash.cpp:502 (integrity-offset bounds)
23d7175 feat(ui): accessibility second pass — Settings / Flash / CSV Import / Autotune
a8b6a2c feat(ui): welcome chip → click to re-validate pack
1acdacc feat(ui): first-run wizard per-step keyboard focus
078911c tools(mutation): skip operator matches inside C++ string literals
```

### Sprint 4 — CLI JSON + mutation lane expansion + UX polish (7 commits)

```
6e84268 feat(cli): dump-table --json emits subuwutuner.dump-table.v1 envelope
fdd858a test(flash): kill 3 mutation survivors in BackupStore::filename_stem slugifier
f086387 feat(ui): surface hidden sidebar categories in Settings → Project
87d9cc0 feat(cli): dump-axis --json emits subuwutuner.dump-axis.v1 envelope
488419d test(policy): kill 2 mutation survivors at flash_preflight battery thresholds
e02c48c ci(mutation): expand advisory lane to 4 ranges (3 more files, ~80 lines)
e378ec9 feat(ui): Compare panel chip tooltips explain Safety / Emissions / Flagged
```

### Sprint 5 — UDS boundary kill + UX expansion (4 commits)

```
fe77dc2 test(uds): kill mutation survivor at OBD2 Mode 09 size-3 boundary
8d0f295 feat(ui): help modal table-of-contents for long topics
79cef37 feat(ui): sidebar footer restore-chips per hidden category
c324628 feat(ui): Compare panel pin/star changed-tables rows
```

---

## Cumulative outcomes

### Tests

- 1361 cases at session start → **1374 at session end** (+13 new
  cases). Assertion count varies with Catch2 randomization but
  was at ~159k for the final run.
- New tests cover: per-ROM history migration (+3), legacy
  fallback (+1), CLI integration for 3 new JSON envelopes (+5),
  flash integrity-check-offset boundaries (+2), BackupStore slug
  classifier boundaries (+1), flash_preflight battery thresholds
  (+1), UDS OBD2 Mode 09 size-3 boundary (+1).

### Mutation coverage

**9 safety-critical files at 100% on actionable mutants**:

  | File                                       | Range   | Notes                                  |
  |--------------------------------------------|---------|----------------------------------------|
  | `src/flash/src/flash.cpp`                  | 400-749 | full Flasher::execute body             |
  | `src/flash/src/flash.cpp`                  |  67-215 | read_full_rom (swap_eq only)           |
  | `src/flash/src/backup_store.cpp`           |  30-40  | slug-char classifier                   |
  | `src/flash/src/checksum.cpp`               |   1-123 |                                        |
  | `src/policy/src/flash_preflight.cpp`       |  65-90  | battery thresholds                     |
  | `src/ecu/src/uds.cpp`                      |   1-600 | OBD2 + UDS framers                     |
  | `src/ecu/src/ssm.cpp`                      |   1-200 |                                        |
  | `src/ecu/src/subaru_security.cpp`          |   1-499 | SA Feistel + variants                  |
  | `src/transport/src/obdx_dvi.cpp`           |   1-162 |                                        |
  | `src/transport/src/native_transport.cpp`   |   1-335 | one practically-equivalent mutant      |

Three documented equivalent / practically-equivalent mutants
remain (not killable by any test):

- `flash.cpp:178` — `remaining < max_chunk_size ? remaining : max_chunk_size`
- `flash.cpp:632` — `remaining < block_payload ? remaining : block_payload`
- `native_transport.cpp:51` — `remaining_ms.count() <= 0` (steady_clock makes it practically equivalent)

### CI mutation lane

`mutation-flash` advisory job now scans **4 ranges in series**
(was 1 at start of session):

  1. `flash.cpp:500-560` — brick-safe sector ordering
  2. `flash.cpp:400-499` — plan validation
  3. `backup_store.cpp:30-40` — slug-char classifier
  4. `flash_preflight.cpp:65-90` — battery thresholds

`continue-on-error: true` remains; quarter-clock for "flip to
ship gate" started on range 1 at `0f8f171`. **Don't flip
sooner.**

Mutation harness improvements:

- `--build-dir` flag (was hardcoded to `win-mingw`); CI passes
  `build/` on Linux
- `--verbose` flag dumps last 20 lines of compiler stderr on
  BUILD_FAIL — diagnosable from job log
- String-literal skip — `!=` / `>` inside `"..."` no longer
  reported as survivors

### CLI

`--json` envelope coverage now includes:

  - `subuwutuner.rom-diff.v1` (sprint 1)
  - `subuwutuner.dump-table.v1` (sprint 4)
  - `subuwutuner.dump-axis.v1` (sprint 4)

All three reject `--json --csv` combos with exit 2. Plus the
pre-existing `pack-info`, `pack-lint`, `project-list-roms`,
`stats`, `audit stats`, `knock-snapshot`, `coldstart-analyze`,
`ai-drift` envelopes from prior sessions.

### GUI

Substantial UX polish across the surface; no architectural
work. Highlights:

- **Settings modal tabified** — 5 tabs (Paths/Theme/Profile/Project/Pack)
- **F1 per-panel help routing** — each panel sets a context
  enum; F1 lands on the relevant `docs/` topic
- **Help modal Sections TOC** — collapsing nav for topics with
  3+ `## ` headings
- **Welcome panel pack-lint chip** — per-recent verdict, click
  to re-validate headlessly
- **Sidebar Hide/Show + footer restore chips** — per-category
  visibility toggles, persisted to `<project>/sidebar_hidden.txt`
- **Compare panel pin/star + Pinned-only filter** — mirrors the
  audit panel pattern with `<project>/compare.pinned`
- **Accessibility — modal initial focus + Enter** — Open
  Project / Read ROM / Settings / Flash / CSV Import / Autotune-MAF
  / Autotune-Knock / first-run wizard all set initial keyboard
  focus on open
- **Glossary tooltips extended** — adaptive_history, coldstart,
  ebcs, compare, flash, read_rom, Compare chip filters

---

## File-level state

### New on-disk sidecars (per-project, in `.stune/`)

  | File                         | Source commit | What                                |
  |------------------------------|---------------|-------------------------------------|
  | `histories/<id>.toml`        | `b648e7f`     | per-additional-ROM edit history     |
  | `pack-lint.toml`             | `037bbd0`     | last-validation snapshot            |
  | `sidebar_hidden.txt`         | `cb921ef`     | hidden category list                |
  | `compare.pinned`             | `c324628`     | starred Compare table_ids           |

All four follow the "empty-set-removes-file" pattern. Combined
with the prior sidecars (`edits.toml`, `audit.log`,
`audit.pinned`, `sidebar_order.txt`, `flash.journal`), a
heavily-used project root now has up to ~8 sidecars. Watch-list
item: consider a `state/` subdirectory if it grows past ~10.

### New AppState fields

About 15 new fields across the session. Notable groupings:

- Focus tracking: 7 `focus_pending_*` bools (new_project, read_rom,
  csv_import, settings, flash, autotune_maf, autotune_knock) +
  `first_run_focused_step`
- Help routing: `HelpContext` enum, `help_context` field,
  `help_initial_topic_id`, `help_scroll_to_heading`
- Pack-lint persistence: `settings_pack_lint_validated_at`,
  `recents_pack_lint` vector
- Sidebar Hide/Show: `sidebar_hidden_categories`
- Compare pin: `compare_pinned_table_ids`,
  `compare_pinned_only`

If the next session adds 3+ more focus_pending bools, consider
collapsing into a single `focus_pending_modal` enum.

---

## Notes / gotchas the next session MUST know

Carries forward only the load-bearing items from the four sprint
handoffs — the items where misunderstanding the design could
introduce regressions or wasted work.

### Per-ROM history legacy-read fallback stays for 3+ months

Loader prefers `<project>/histories/<id>.toml`, falls back to
the pre-2026-06-04 `<id>.edits.toml` at the root, and removes
the legacy file after a successful write. **Don't remove the
legacy-read fallback** until every shipped project has migrated
through a save cycle. Until then, pre-2026-06-04 projects
would lose their additional-ROM histories on first open.

### Equivalent mutants are not coverage gaps

The mutation harness will report `flash.cpp:178`, `flash.cpp:632`,
and `native_transport.cpp:51` as surviving every run. These are
**equivalent or practically-equivalent mutants** — no test
distinguishes them. Don't write tests trying to kill them.

### CI mutation lane stays advisory

`continue-on-error: true` + `|| true` after each invocation.
Surviving mutants surface in the job log, not the exit code.
Quarter-clock for "flip to ship gate" started at `0f8f171`
on the first range. **Don't flip sooner than ~September 2026
without a specific signal that the gate is ready.**

### F1 routing depends on every panel calling `track_help_context`

Adding a new panel without that call leaves `state.help_context`
unchanged from whatever was set last; F1 lands on the previous
panel's topic. The single source of truth for the mapping is
the switch in `main.cpp`'s F1 handler.

### Compare pin is composite-keyed by `table_id` only

Not `table_id + ROM_B`. The pin marks "this table is
interesting to me", not "this comparison is interesting." A
future "annotate per-comparison" feature should use a separate
sidecar, not bolt onto `compare.pinned`.

### Welcome chip silently no-ops on project-open failure

Deliberate — a regular click on the same recent would surface
the error visibly. The chip-click path stays quiet to avoid
duplicating error surfaces.

### Compare pin filter composes AND, not OR

The "Pinned only" Checkbox stacks ON TOP of the
Safety/Emissions/Either-flag chip filter (AND semantics). If a
future filter dimension joins, mirror this pattern — don't
introduce an OR mode.

### Settings modal tabs use unsuffixed IDs

`BeginTabItem("Paths")` etc. Adding a sixth tab with a duplicate
display name would collide silently. Use `Display##stable_id`
form if any two tabs ever need the same label.

### Mutation harness `--tag-filter` is exact substring

`[native]` matches the codec tests; `[native_transport]` matches
the transport tests. A scan with the wrong tag will report
100% BUILD_FAIL or low coverage that LOOKS like a real gap.
Always check the test file's TEST_CASE tags before adding a
new mutation lane entry.

### `pack-lint.toml` is keyed by `pack_id`

Repointing a project at a different pack drops the cached
verdict — the chip flips to "not validated" rather than
showing a misleading "OK from a different pack". This is by
design.

### Help TOC scroll-request is one-frame-only

`state.help_scroll_to_heading` is consumed by `render_markdown`
and cleared at end of render. If a future refactor splits the
click → render path across multiple frames, the field will need
a "consumed this turn" sentinel.

### Mutation harness string-literal skip doesn't cover everything

Handles `"..."` and `"\"..."` escapes. Does NOT handle raw
strings `R"(...)..."`, char literals `'\''`, or multi-line
strings. Currently fine for `src/flash` + `src/policy`. If the
lane expands to `src/feature` (raw strings) or
`src/transport/serial_*` (char literals), the heuristic needs
strengthening.

### Audit / Compare / Sidebar all use the same sidecar shape

`audit.pinned` (one composite key per line), `compare.pinned`
(one `table_id` per line), `sidebar_hidden.txt` (one category
name per line). Empty set → file removed. Consistent pattern;
mirror it for any future "user-selected set persists across
sessions" feature.

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)

No change from start of session. The mutation lane is now
broader (4 ranges) and the safety-critical surface is closer
to ship-gate status across the board, but `continue-on-error`
hasn't been flipped.

### Real-world unblockers (unchanged)

1. **Bench-rig hardware** — junkyard 2017 WRX ECU. Last memory
   entry expected arrival 2026-05-27/28; if it's here, open the
   Findings/ HANDOFF and walk `docs/28`.
2. **RH850 codegen bench validation** — same.

### Big architectural items (want green light before starting)

None touched this session; same list across all 4 parts:

- **`src/diff/` + Compare panel rebuild (#4)** — `st::diff` +
  CLI `diff` verb already exist. Gap is structured/persistent
  GUI Compare panel. Multi-day, UX-heavy.
- **`VehicleProfile` (#7)** — unified anchor over Project +
  Settings + jurisdiction. Touches a lot; needs a call on shape.
- **Cross-session `AuditLog` (#8)** — per-ECU touch log
  spanning projects. Subscribers already emit events; sink
  is missing.
- **AI Tier 2 LLM narration (`docs/20`)** — `ai-drift` parser
  has headroom; engine call site unstubbed. Needs network +
  advisory boundary preservation.
- **Multi-ROM cross-ROM diff** — side-by-side view; pairs with
  Compare panel rebuild.
- **Live-to-table cross-reference + knock overlay (#15/#16)** —
  depends on live gauge cluster + editor "jump-to-table".

### Smaller items the next session could pick up

Filtered to items still actionable without architectural
sign-off:

- **CI mutation lane expansion** — `uds.cpp:1-600`,
  `subaru_security.cpp:1-499`, `ssm.cpp:1-200`,
  `obdx_dvi.cpp:1-162`, `flash_preflight.cpp:1-180` all sit
  at 100% locally but aren't in CI yet. Same pattern as the
  `e02c48c` expansion. Watch job time budget (currently ~5
  min per range × 4 = 20 min, max-parallel would help if
  ranges add up past 30 min).
- **Compare panel bulk pin/unpin popup** — mirrors `5797fc3`'s
  audit panel bulk-pin pattern. "Pin N visible / Unpin N
  visible / Clear all pins" against the current filter view.
- **NDJSON export of pinned compare entries** — analogous to
  `a2ebdb4`'s audit pinned-only export. Workflow:
  star the relevant comparisons, export, hand to e-tuner.
- **Help modal persistent active-topic** — currently resets to
  topic 0 on each open. Persisting to `settings.txt` lets the
  user re-find their place.
- **`compare.pinned` GUI surface in Settings** — Settings →
  Project tab could surface "Pinned compare tables: N · Reset"
  parallel to `f086387`'s hidden-cats line.
- **Sidebar drag-reorder bulk operations** — categories can
  be reordered (07c9d61) and hidden (cb921ef); no "alphabetize"
  / "group by safety" affordance yet.
- **Mutation harness: raw-string / char-literal skip** —
  documented gap in the string-literal heuristic. Low priority
  until the lane expands to a file that uses them.

### Watch-list for cumulative complexity

- **AppState size** — ~800 lines, +15 fields this session. If
  the next session adds another ~10-field cluster, consider
  splitting into per-domain headers.
- **Sidecar count per `.stune/`** — now up to ~8. Consider a
  `state/` subdir before adding a 9th.
- **CI mutation runtime** — 4 ranges × ~5 min = 20 min on
  Linux runner. Past 30 min the lane needs parallelization.
- **CLI integration test count** — subprocess-launch the
  binary; Windows process-launch cost makes these slow as
  more verbs join. Was <5 s at session start; revisit if
  past 30 s.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# Sanity-check the new CLI JSON envelopes
./build/win-mingw/bin/subuwutuner-cli.exe rom-diff \
    --def fixtures/demo-pack/pack.toml --json \
    fixtures/demo.stune/source.bin fixtures/demo.stune/working.bin
./build/win-mingw/bin/subuwutuner-cli.exe dump-table \
    --def fixtures/demo-pack/pack.toml \
    --table boost_target_high_octane --json \
    fixtures/demo.stune/source.bin
./build/win-mingw/bin/subuwutuner-cli.exe dump-axis \
    --def fixtures/demo-pack/pack.toml --axis rpm_axis --json \
    fixtures/demo.stune/source.bin

# Sanity-check the new GUI surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   File → New Project / Open Project / Read ROM — Tab nav from focus
#   Tools → Settings — 5 tabs (Paths/Theme/Profile/Project/Pack)
#     Project tab: hidden-cats count + Reset
#     Pack tab: validate-pack chip with timestamp
#   View → Compare — chip filter tooltips + per-row star + Pinned only
#   View → Sidebar right-click — Hide / Show submenu
#   F1 anywhere — context-routed help topic + Sections TOC on long ones
#   Welcome panel recents — pack-lint chip → click to re-validate

# Re-run the broadened mutation surface for parity
python tools/mutation_test.py --file src/flash/src/flash.cpp \
    --line-start 500 --line-end 560 \
    --target st_unit_tests --tag-filter "[flash]" \
    --max-mutants 20 --build-dir build/win-mingw \
    --mutations swap_eq,swap_lt,swap_gt
# Expected: 100% (was 66.7% at session start)

# Check today's CI runs for the expanded mutation lane
# https://github.com/BuffJesus/SubuwuTuner/actions
# Expected: 4 named steps under "mutation testing (advisory) — st::flash"
#           all reporting 0 SURVIVED
```

---

## Deeper-dive references

The four sprint-level handoffs from this session each carry
the detailed per-commit breakdowns, gotchas, and watch-list
items for their respective sprints. If something in this
doc's "What shipped" table is unclear, the sprint-level
handoff for that commit will have more context:

- `HANDOFF-2026-06-04-night.md` — sprint 1 (4 commits, polish + CI sprint setup)
- `HANDOFF-2026-06-04-night-2.md` — sprint 2-3 boundary (mutation kill + acc 2nd pass + chip click)
- `HANDOFF-2026-06-04-night-3.md` — sprint 4 (CLI --json + mutation expansion + UX polish)
- `HANDOFF-2026-06-04-night-4.md` — sprint 5 (UDS boundary + help TOC + sidebar chips + Compare pin)

Plus the pre-session handoff:

- `HANDOFF-2026-06-04-session.md` — the daytime polish + CI
  session that fed into this night's continuation.

---

## TL;DR

> 22 feature/test/CI commits + 5 handoff commits across an
> extended 2026-06-04 night session. `origin/main = 9de554d`.
>
> **CLI**: 3 new `--json` envelopes (`rom-diff.v1`,
> `dump-table.v1`, `dump-axis.v1`).
>
> **GUI**: Settings tabified, F1 per-panel routing with TOC for
> long topics, accessibility first + second pass across 7
> modals, welcome chip click→revalidate, sidebar Hide/Show
> with footer restore chips, Compare pin/star with Pinned-only
> filter, glossary tooltips extended.
>
> **Safety-critical mutation coverage**: 9 files at 100% on
> actionable mutants. CI advisory lane expanded from 1 to 4
> ranges. Quarter-clock for "ship gate" runs from `0f8f171`.
>
> **Tests**: 1361 → 1374 cases, +13 new. All green.
>
> **Untouched, want green light**: `src/diff/` Compare panel
> rebuild, VehicleProfile, cross-session AuditLog, AI Tier 2
> narration, multi-ROM diff, live-to-table cross-reference.
>
> **Untouched, hardware-gated**: bench rig, RH850 codegen
> validation.
>
> Off you go.
