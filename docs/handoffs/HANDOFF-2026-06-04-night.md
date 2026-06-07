# Handoff — 2026-06-04 (night session — polish + UX sprint)

**`origin/main = 0f8f171`** — nine commits pushed across two
sprints (range `b648e7f..0f8f171`). Tree clean except the four
prior HANDOFF-2026-06-0{1..4}*.md files. Tests stayed green
throughout; 1365 cases / ~159k assertions on the final run.
Both binaries link clean.

```
0f8f171 ci(mutation): advisory mutation-testing lane over st::flash
187d6db feat(ui): accessibility first pass — modal initial focus + Enter shortcut
dcfee4a feat(ui): F1 per-panel help context routing (closes analyst QW-I)
cf0e15b feat(ui): extend glossary tooltips to log/ROM panels + modals
32167f0 feat(ui): tabify Settings modal (Paths / Theme / Profile / Project / Pack)
005fb9a feat(cli): rom-diff --json emits subuwutuner.rom-diff.v1 envelope
cb921ef feat(ui): sidebar per-category Hide/Show
037bbd0 feat(ui): persist pack-lint snapshot to <project>/pack-lint.toml
b648e7f refactor(project): per-ROM histories under histories/ subdir
```

---

## How the session ran

Started against `HANDOFF-2026-06-04-session.md`. That handoff's
"things to ship next" list had four cheap quick wins; this session
walked them end-to-end, then pivoted to a mid-size UX sprint
covering five more items from the project-wide backlog.

Two sprints, no spec drift:

1. **Quick wins (4 commits)** — per-ROM histories subdir + pack-lint
   persistence + sidebar Hide/Show + `rom-diff --json`.
2. **Mid-size UX + CI (5 commits)** — Settings tabs + glossary
   tooltip expansion + F1 context routing + accessibility first
   pass + mutation-testing CI lane.

---

## What's on `origin/main` that wasn't there at session start

### Quick-wins sprint

**`b648e7f` — `histories/` subdir for per-ROM edit files**
Per-additional-ROM edit history moved from `<project>/<id>.edits.toml`
to `<project>/histories/<id>.toml`. Working's history stays at
`<project>/edits.toml` (the v1 `docs/21` shape). Loader still reads
the legacy root-level path so existing projects open without manual
migration; the first save through `save_active_rom` / `save_all`
rewrites under `histories/` and removes the legacy file.

Also reserves `id == "edits"` against `add_additional_rom` — would
otherwise collide with working's `edits.toml` under any future
tooling regression that re-derived history paths from the slot id
without the `histories/` prefix.

Closes the cumulative-complexity watch-list item carried over from
HANDOFF-2026-06-{03,04}: per-ROM history files no longer scale into
project-root clutter as the additional-ROM count grows.

**`037bbd0` — `<project>/pack-lint.toml` snapshot persistence**
Settings → Validate pack used to drop its result the moment the modal
closed. Now persists verdict + ISO 8601 timestamp + pack_id +
violation message to `<project>/pack-lint.toml`, loads on project
open, surfaces in two places:

- Settings chip picks up the prior verdict immediately + a
  relative-time suffix ("12 minutes ago")
- Welcome panel recents grow a sub-chip pulled from each recent
  project's `pack-lint.toml` if one exists ("Pack OK · 2 hours ago"
  or "Pack: 3 issues · yesterday"). Cache fills lazily on first
  paint after a recents-list change, one disk read per recent.

Snapshot is keyed by `pack_id`; repointing a project at a different
pack drops the cached verdict so a stale chip can't shadow a real
regression.

**`cb921ef` — sidebar per-category Hide/Show**
Right-click a category header for "Hide" — drops out of the
sidebar entirely until restored. Right-click empty panel area for
the existing menu, now extended with a "Show hidden category ▸"
submenu listing what's currently hidden plus "Show all categories"
reset. Both routes persist to `<project>/sidebar_hidden.txt`; the
file is removed when the hidden set goes empty.

Footer line under the tree reports "N categories hidden ·
right-click panel to restore" any time the set is non-empty, so a
user returning to a project they configured weeks ago can't be
puzzled by what looks like missing data. Hide check runs before
the filter check — a substring search for some other table won't
drag the suppressed category back into view.

**`005fb9a` — CLI `rom-diff --json` envelope**
`rom-diff` was text-summary-only; the newer `diff` verb covers
text|csv|json against `st::diff` but ships the full per-cell
DiffSet. Dyno scripts that just want "which tables moved and by
how much" don't need the full backend — adds the summary surface
in JSON form via `subuwutuner.rom-diff.v1`:

  - `rom_a` / `rom_b`: path, crc32, size, match (CID or null)
  - `pack.id`
  - `tables_compared` / `tables_changed` / `tables_skipped`
  - `changes[]`: table id, cells_changed/total, max_abs_delta,
    mean_abs_delta, unit

Sort matches text mode (descending max-Δ) so the first array
element is the loudest delta — a CI script reading rows[0] gets
the stable contract. `--verbose` still goes to stderr only so it
composes with `--json` without polluting the structured stream.
+1 integration test pins the v1 schema keys.

### Mid-size UX + CI sprint

**`32167f0` — Settings modal tabified**
Five sections (Paths / Theme / Profile / Project / Pack) wrapped
in ImGui tabs. Caps the modal height on a small display before
`AlwaysAutoResize` could push Save off-screen — flagged on the
06-04 watch-list. Config-file header stays above the bar (it's
universal), Save / Restore Defaults / Close stays below (commits
paths AND project metadata in one transaction). Theme + Profile
widgets still self-persist on click, so tab-flipping without
hitting Save doesn't lose those preferences.

**`cf0e15b` — glossary tooltip coverage expansion**
`glossary_tooltip_for` was already shipped (widgets.cpp:412) and
called from sidebar / welcome / dtcs / knock_dashboard. Extended
to the panels + modals where the tuning jargon actually leads the
content:

- `adaptive_history` / `coldstart` / `ebcs` — "Datalog" on Log:
- `compare` — "ROM" on ROM A + ROM B
- flash modal — "Flash" on plan header
- read_rom modal — "ROM" + "J2534" on intro

**`dcfee4a` — F1 per-panel help context routing (closes analyst QW-I)**
F1 still toggles the help modal, but the topic it lands on now
depends on what the user was looking at:

  Flash modal               → 31-brick-protection-by-isa
  Read ROM modal            → 23-security-access
  Audit panel               → 08-testing-strategy
  Compare panel             → 21-stune-format
  Features designer         → 16-custom-features
  Adaptive History panel    → 20-ai-integration
  Cold-Start / EBCS / Knock → 05-improvements (§11)
  Sidebar / Table editor    → 11-definition-format
  Settings modal            → 25-config-system
  First-Run wizard / blank  → 00-overview

Implementation: each panel calls `track_help_context(state, ctx)`
after Begin (no-op when its window isn't focused, so overlapping
panels don't fight each other). Modals own focus while shown and
write the state field directly. The F1 handler reads
`state.help_context` + seeds `state.help_initial_topic_id`; the
help modal consumes the id on its next open and clears it.
`kTopicSources` expanded from 3 → 14 entries so the routing
targets actually exist in the sidebar.

Closes analyst Issues #12 + #22 / `docs/33` QW-I.

**`187d6db` — accessibility first pass — modal initial focus + Enter**
Open Project / Read ROM modals previously required a mouse-click
into the dialog before keyboard input was usable. Now each modal
sets initial keyboard focus on its first interactive control the
frame it opens, using a per-modal `focus_pending_*` one-shot flag.

New Project also picks up Enter as the default-Create shortcut
(gated on `!ImGui::IsAnyItemActive()` so Enter inside the
display-name field still commits the input, not the modal).
Tooltip on Create advertises the new (Enter) shortcut.

CSV Import + Flash + Settings already had Enter / Esc defaults
and are skipped this pass. AppState carries `focus_pending_*`
stubs for them so the next pass has a uniform per-modal entry.

**`0f8f171` — mutation-testing CI lane over `st::flash` (advisory)**
Wires the existing `tools/mutation_test.py` driver into CI as an
advisory (`continue-on-error: true`) job that runs on push to main
and manual dispatch only — skipped on PRs to keep reviewer signal
high. Targets a 60-line window inside `Flasher::execute`, capped
at 20 mutants with `swap_eq` / `swap_lt` / `swap_gt` enabled.

Driver changes (back-compat):

- `--build-dir` (Path) — defaults to local `win-mingw` checkout;
  CI passes `build/` on the Linux runner so the win-mingw hardcode
  doesn't break cross-platform invocation
- `--verbose` — prints the last 20 lines of compiler stderr on
  BUILD_FAIL so -Werror cascades are debuggable from the job log

Local run on the same window:

```
KILLED     2
SURVIVED   1   line 502: w.sector.address <= off  →  ... < off
                (a real coverage gap — the integrity-check-offset
                 sector-pick test doesn't exercise the boundary)
Score      66.7%
```

Closes the partial side of analyst Issue #19 / `docs/33` issue draft #19.

---

## Test state at end-of-session

- Build: both binaries link clean
- Tests: 1365 cases / ~159k assertions, all pass (was 1361 at
  session start; +4 new — 3 project tests covering the histories/
  migration + 1 CLI integration test pinning the rom-diff v1
  schema)
- Live CI run for `0f8f171` not yet inspected — `gh` CLI isn't on
  PATH on this machine. Visit
  https://github.com/BuffJesus/SubuwuTuner/actions for status.
  The new `mutation-flash` advisory lane will appear on the run.

---

## Notes / gotchas the next session should know

### Per-ROM history legacy path is still load-bearing for one cycle

Loader prefers `<project>/histories/<id>.toml`, falls back to the
legacy `<project>/<id>.edits.toml` at the root, and removes the
legacy file after a successful write under the new path. **Don't
remove the legacy-read fallback** until every shipped project has
migrated through a save cycle (rough heuristic: 3 months after
the first 1.0 release on the wild). Until then, pre-2026-06-04
projects would lose their additional-ROM histories on first open.

### `id == "edits"` is now reserved for additional ROMs

`add_additional_rom` rejects this slug with InvalidArgument. Memory
of the legacy collision risk (HANDOFF-2026-06-03 gotchas) — kept
even though the `histories/` subdir defuses the immediate hit,
because a future regression that re-derived per-ROM history paths
from the slot id without the `histories/` prefix would resurrect
the collision.

### `pack-lint.toml` is keyed by `pack_id`, not just presence

Project opens loads the snapshot but only honors it when
`snap.pack_id == project->definition().pack().id`. If a user
repoints the `.stune` at a different pack, the cached verdict drops
to "not validated" rather than show a misleading "OK from a
different pack". A future tooling change that bumps a pack id
under the user's feet (e.g. defgen regeneration with a new
naming convention) will look like every project just lost its
lint cache — that's the right answer, not a bug.

### Welcome panel pack-lint chip cache is invalidated by size mismatch

`state.recents_pack_lint.size() != state.recents.size()` triggers
a full refill on the next welcome paint. Mutating individual
recents in place without changing the vector's size (e.g. pin
toggle) leaves the cache stale until the user navigates away and
back. Acceptable for now (pin toggle doesn't affect pack-lint
state); revisit if a per-recent mutation grows that interacts
with the lint cache.

### Sidebar hidden-set hide-check runs before filter-check

A category in `sidebar_hidden_categories` stays hidden even if a
text filter matches a table inside it. Intentional — the user
opted out; a substring search shouldn't drag the suppressed
category back into view. Don't reverse this; the footer line
("N hidden, right-click to restore") is the discoverable
escape hatch.

### `rom-diff --json` envelope is the summary shape, not the full DiffSet

For per-cell structured output, use the `diff` verb (which feeds
`st::diff` and supports `--format json` + `--save .stcompare`).
The two are intentionally split: rom-diff is the cheap "which
tables moved" surface for dyno scripts; diff is the structured
backend for the Compare panel + CI-grade reproducible diffs.
**Don't merge them.** Scripts depending on `rom-diff.v1` shape
shouldn't get hit by a Compare-panel feature add.

### Settings modal tab IDs are bare strings

`BeginTabItem("Paths")` etc. use unsuffixed names so the ImGui
ID hash is built from the string alone. Adding a sixth tab with a
duplicate name would collide silently — pick distinct labels. If
two tabs ever need the same display string (e.g. Project + Project
Metadata), use the `Display##stable_id` form.

### F1 help-routing depends on each panel calling `track_help_context`

Adding a new panel without that call leaves `state.help_context`
unchanged from whatever was set last — F1 lands on the previous
panel's topic. The single source of truth for the mapping is the
switch in `main.cpp`'s F1 handler; if you add a new HelpContext
enum value, update both the panel render that sets it AND the
switch that consumes it.

### Mutation lane is advisory only — never a ship gate

`continue-on-error: true` + `|| true` after the mutation invocation
means the lane reports surviving mutants in the job log but never
blocks a merge. The plan in the commit body: when the lane reports
zero surviving mutants in the targeted window for a quarter,
flip continue-on-error off and make it a ship gate. **Don't flip
it sooner** — current run already surfaced one real surviving
mutant at flash.cpp:502, and the test-suite-hardening work to
kill it hasn't landed yet.

### Mutation harness BUILD_FAIL is "uninteresting", not "missed"

`-Werror=nodiscard` means a mutation that swaps `==` to `!=` in an
early-return chain may cascade into a `[[nodiscard]] log()` warning
that fails the build. The driver's mutation score is `killed /
(killed + survived)` — BUILD_FAIL drops out of the denominator.
If the BUILD_FAIL rate ever climbs above ~30% for a window, that's
a sign the line range needs to be picked away from early-return
hot paths, not a bug in the harness.

### Existing carry-over notes still apply

From HANDOFF-2026-06-04-session.md:
- `--json --csv` precedence on analyze verbs (reject exit 2)
- Coldstart `--csv` flat `phase_<name>_samples=` keys
- `BeginPopupModal` Z-order pathology for startup-fired modals
- `active_rom_mut() → nullable` contract on edit paths
- Per-ROM `<id>.edits.toml` filename collision risk → now formalized as reserved id
- AI drift classifier is advisory-only, no auto-apply channel ever

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)

- Every entry on the blocker list at session start remains either
  closed or hardware-gated (no change today). #10 (CI binary-size
  gate) still has the cold-start + idle-RAM thresholds unmet —
  same situation as 06-04 EOD.

### Real-world unblockers (unchanged from 06-04)

1. **Bench-rig hardware** — junkyard 2017 WRX ECU still pending
   per memory `project_bench_rig_awaiting_ecu.md`. ETA was
   2026-05-27/28; it's now 2026-06-04. If it's arrived, open the
   Findings/ HANDOFF and walk `docs/28`.
2. **RH850 codegen bench validation** — still gated on the rig.

### Things to ship next (no specific user direction)

These are the bigger items from the original task list this session
drew from. Each wants a green-light call before starting:

- **`src/diff/` + Compare panel rebuild (#4)** — `st::diff` + the
  `diff` CLI verb already exist (analyst was wrong about that one).
  The remaining gap is the GUI Compare panel growing a structured
  DiffSet view, sortable/filterable/persistent (analyst #5). Big
  feature — multi-day, UX-heavy.
- **`VehicleProfile` (#7)** — unified anchor over Project +
  Settings + jurisdiction. Touches a lot of code; needs a call on
  whether it absorbs the existing settings profile or sits
  alongside.
- **Cross-session `AuditLog` (#8)** — per-ECU touch log spanning
  projects. Subscribers in transport/UDS/flash already emit the
  events (`e70c40e`); the cross-session sink is what's missing.
- **AI Tier 2 LLM narration (`docs/20`)** — parser headroom exists
  in `ai-drift`; engine call site unstubbed. Needs network +
  careful advisory-boundary preservation. The Tier 1 classifier's
  "advisory only, no auto-apply" contract MUST carry forward to
  any narrator.
- **Multi-ROM cross-ROM diff view** — currently the user can
  switch active ROM but can't see two side-by-side at once. Pairs
  with the Compare panel rebuild.
- **Live-to-table cross-reference (#15) + knock overlay on edited
  table (#16)** — depends on the live gauge cluster + the editor
  growing a "jump-to-table" + "scatter overlay" affordance.

### Accessibility second-pass candidates

The first pass landed initial-focus for New Project + Read ROM
modals and Enter→Create on New Project. Next pass should cover:

- CSV Import / Flash / Settings — the stub flags exist on AppState,
  no implementation yet
- First-Run wizard — Tab nav between wizard steps probably needs
  explicit SetKeyboardFocusHere on step transitions
- Autotune-MAF / Autotune-Knock — log path picker is the natural
  first focus
- Sidebar / Help modal — keyboard nav already strong; Tab order
  audit only

### Mutation-lane expansion candidates

Surviving mutant at `flash.cpp:502` is the obvious first
"harden the test suite" target — add a test for the
integrity-check-offset boundary condition. Once that mutant is
killed, expand the line range upward (e.g. 345-749 covers all of
`Flasher::execute`). Then consider widening the lane to
`src/policy` (the Flash gate) and `st::ecu::uds` (the protocol
parser).

### Watch-list for cumulative complexity

- AppState grew several modal-focus stubs (`focus_pending_*`) +
  the HelpContext enum + the recents_pack_lint cache. The class
  is now ~800 lines; if the next session adds another 100-line
  field cluster, consider splitting `app_state.hpp` into per-domain
  headers (autotune state, features-designer state, etc.).
- `kTopicSources` grew from 3 → 14 entries. The help sidebar is
  still scannable but a 25-entry list would start to overwhelm
  on a small viewport. If routing wants new targets, prefer
  promoting existing topics over adding new ones (e.g.
  "20-ai-integration" already covers the AI drift surface).
- Mutation lane runs at ~60 s per mutant on the Linux runner; the
  20-mutant cap means a single run is ~20 minutes. Don't widen
  past 30 mutants without parallelizing or moving to a nightly
  schedule.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# Try today's new surfaces on the demo project
./build/win-mingw/bin/subuwutuner-cli.exe rom-diff \
    --def fixtures/demo-pack/pack.toml --json \
    fixtures/demo.stune/source.bin fixtures/demo.stune/working.bin

./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   View → Audit               → unchanged from 06-04
#   Tools → Settings           → 5-tab layout (Paths / Theme / Profile / Project / Pack)
#                                Pack tab carries timestamp + welcome chip
#   Welcome panel (no project) → recents chip with "Pack OK · <date>"
#   Sidebar right-click header → "Hide <category>" + footer hint
#   F1 from Flash modal        → docs/31 brick protection
#   F1 from Audit panel        → docs/08 testing strategy

# Check today's CI run for the new mutation lane
# https://github.com/BuffJesus/SubuwuTuner/actions
# Look for: "mutation testing (advisory) — st::flash"
# Expected verdict: one SURVIVED at flash.cpp:502
```

---

## TL;DR

> Nine commits today; everything on `origin/main = 0f8f171`.
> Two sprints: quick-wins (per-ROM histories subdir, pack-lint
> persistence, sidebar Hide/Show, `rom-diff --json`) and mid-size
> UX + CI (Settings tabs, glossary expansion, F1 context routing,
> accessibility first pass, mutation CI lane).
>
> Analyst QW-I (per-panel help routing) and the partial side of
> analyst Issue #19 (mutation testing in CI) both close today.
> Mutation lane found a real coverage gap on first run at
> `flash.cpp:502` — integrity-check-offset boundary condition
> isn't tested. Surfacing it counts as the lane doing its job;
> killing it is a future test-suite-hardening task.
>
> Real-world unblocker if the junkyard ECU arrived: open the
> Findings/ HANDOFF and walk `docs/28`.
>
> Watch the per-ROM history legacy-read fallback (don't remove
> until 3+ months of shipping under the new path). Watch the
> `pack_id`-keyed pack-lint cache (intentional drop on pack
> repoint, not a bug). Watch the F1 routing's `track_help_context`
> contract on any new panel.
>
> Off you go.
