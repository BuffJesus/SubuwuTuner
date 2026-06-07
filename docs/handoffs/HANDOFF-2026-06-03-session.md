# Handoff — 2026-06-03 (long autonomous session)

**`origin/main` = `b661c88`**. 22 commits pushed in four sprints across
the day (range `4d0cefb..b661c88`). Tree clean except the two prior
HANDOFF-2026-06-0{1,2}-*.md files and this one (all untracked).

Tests stayed green throughout (1296+ cases at first checkpoint, more
added through the day — every commit-claim of "suite green" held).
Both binaries link clean (`subuwutuner-gui.exe`, `subuwutuner-cli.exe`).

---

## How the session ran

Started against `HANDOFF-2026-06-02-session.md`. That handoff's "still
open" list had two big architectural items (#10 deep multi-ROM, #8
deep transport audit), eight medium polish items, and a CLI test-
coverage gap. **All eleven are closed today.** The session also
opened a new front — AI integration Tier 1 from `docs/20` — and shipped
it end-to-end (engine + CLI + GUI surface).

Four distinct sprints:

1. **Morning (5 commits)** — Issue #10 deep multi-ROM, working down
   from the read-side identifier through the GUI picker, status-bar
   chip, stats relabel, and finally per-ROM `edit::History`.
2. **Midday (7 commits)** — Issue #8 deep audit subscribers
   (transport/UDS/flash), the #10 modal/CSV/audit sweep, integration
   smoke for the 12 new verbs (closes yesterday's coverage gap),
   inverse pid-table lookup (#15), UDS state-machine property test,
   OFL-1.1 + defgen spec + roadmap, welcome pin/star + CLI flag
   completion.
3. **Evening (6 commits)** — every remaining "medium polish" item
   from yesterday's list: pack-lint verb, audit pin/star with sidecar,
   Compare Copy B→A, audit stats subcommand, help find-in-topic,
   sidebar drag-reorder.
4. **Late evening (5 commits)** — AI Tier 1 drift classifier landed
   end-to-end: `st::ai::drift` engine → `ai-drift` CLI verb (with
   `--json`) → adaptive-history panel surfaces it inline. Plus
   settings-modal theme picker with live swatch preview, and `--json`
   on knock-snapshot + coldstart-analyze.

---

## What's on `origin/main` that wasn't there at session start

### Issue #10 — multi-ROM refactor, now deep (5 commits)
| Commit | What |
|---|---|
| `646374e` | `Project::active_rom_id` + read-side wiring; `project-set-active-rom` CLI; reserved-id rejection |
| `eb1747c` | GUI picker (View → Active ROM), table view routes through `view_rom()`, defense-in-depth guards inside `apply_op` / paste / reset / undo / redo |
| `0835dc6` | Status-bar Active-ROM chip (hidden when working), command-palette `View: Switch to <slot>`, DTC display flows through `view_rom()` |
| `134ce34` | Stats panel relabels for non-working (`vs source` + `vs working`); Compare panel pre-fills slot B with `<project:rom_id>` |
| `c4611a3` | **Per-ROM `edit::History`** — additionals get `<id>.edits.toml`, source falls back to working's history, `active_rom_mut()` is the nullable contract for "this slot is read-only" |
| `5b37bae` | Sweep — flash modal banner when active is non-working, CSV export through `view_rom()`, audit entries annotate `rom` field for additionals |

### Issue #8 — deep audit subscribers (1 commit)
- `e70c40e` — `UdsClient::set_audit_log()` + `Flasher::set_audit_log()` seams. Emits `SecurityAccessUnlocked` (UDS), and `FlashStarted` / `FlashSectorWritten` / `FlashCompleted` / `FlashFailed` / `FlashCancelled` (Flasher). Null pointer is no-op for existing callers. Six subscriber tests.

### AI integration Tier 1 (docs/20) — new (3 commits)
- `5daafc7` — `st::ai::drift` engine. Pure-function rules over an
  adaptive-history snapshot → `DriftDiagnosis` (cause + confidence +
  evidence + alternatives + recommended_checks). 13 synthetic-snapshot
  tests walking every rule path. **No LLM, no I/O, no allocations
  beyond the returned struct.** Safety-first rule ordering: DAM-below
  → knock_correction first, then fuel-quality / fuel-system /
  idle-air. **No auto-apply channel — output is advisory text the user
  reads, never an executor input.** Thresholds parametrizable.
- `244437e` — `subuwutuner-cli ai-drift` verb. Loads adaptive-history
  CSV, runs `st::ai::drift::classify`, prints diagnosis. `--json`
  emits `subuwutuner.ai-drift.v1` records. Deliberate parallel parser
  vs `adaptive-history` so the two evolve independently.
- `a76365a` — Adaptive-history panel renders the diagnosis inline:
  confidence-colored cause + chip, collapsible evidence/alternatives/
  recommended-checks pane (defaults open for Likely/Ambiguous, closed
  for Possible/NoSignal). Classifier is pure → per-frame call is
  microseconds, no caching.

### CLI surface gains
| Verb | What |
|---|---|
| `pack-lint` | `Definition::validate()` smoke for pack authors; `--json` → `subuwutuner.pack-lint.v1` |
| `audit stats` | Per-kind counts + first/last timestamp + bad-checksum count; `--json` → `subuwutuner.audit-stats.v1` |
| `ai-drift` | (above) |
| `knock-snapshot --json` | `subuwutuner.knock-snapshot.v1` |
| `coldstart-analyze --json` | `subuwutuner.coldstart-analyze.v1` |
| `project-set-active-rom` | (Issue #10 read-side) |
| `stats --rom` | Honor active-rom; `project-info` shows active slot |
| `completion bash` / `completion zsh` | Now also offer ~30 common flags when current word starts with `-` (was subcommands-only) |

### GUI polish closing yesterday's list
- Audit panel: pin/star with leftmost ★/☆ column + "Pinned" toolbar
  filter (composite `<ts_ns>:<crc32>` key → sidecar at
  `<project>/audit.pinned`)
- Welcome panel: pin/star recents (3rd tab-delimited field; legacy
  2-field entries still parse)
- Sidebar: drag-to-reorder category headers; persists per-project at
  `<project>/sidebar_order.txt`
- Settings modal: theme picker with side-by-side accent-triple swatches
  for Dark/Light, live `apply_theme` + same-frame `save_settings`
- Help modal: find-in-topic input (Ctrl+G), case-insensitive substring,
  "X of Y lines match 'needle'" header, flat per-line rendering in
  find mode
- Compare panel: Copy B→A workflow (closes the "Apply A→B" item)
- Table view: "Logged by: <pid>" chip via `Definition::find_pids_producing()`
  — closes inverse direction of the gauge↔table link

### Tests
- `c691200` — **Integration smoke for 12 new CLI verbs** (closes
  yesterday's coverage gap). Subprocess-based Catch2 fixture invokes
  the real `subuwutuner-cli` binary against `fixtures/demo.stune`.
  `tests/unit/_helpers/cli_runner.hpp` wraps popen (Windows
  double-quote workaround + POSIX `WEXITSTATUS`). CMake injects
  `ST_CLI_BINARY_PATH` + `ST_FIXTURE_DEMO_STUNE`; absent binary →
  skip not fail.
- `e6112f6` — UDS state-machine sequence property test, 80 iterations
  over random plan shapes. Closes `docs/04` pre-1.0 blocker #11.
  Invariants: DSC(programming) first, CC(off) iff `silence_bus`,
  per-sector ordering, `dry_run` emits zero sector PDUs,
  between-sectors cancel emits DSC(default) last. Mid-sector cancel
  coverage stays in `test_cancellation_invariants.cpp`.
- `f5c6ac6` — `tests/unit/audit/test_apply_op_audit_contract.cpp` —
  4-case fixture locking the `EditCommitted` wire shape
  (table/cells/rom fields).

### Docs / licensing
- `f13ccd4` — `THIRD_PARTY_NOTICES.md` gains §"Bundled fonts" with
  full SIL Open Font License v1.1 text (closes `docs/04` pre-1.0
  blocker #9). `tools/defgen/defgen.spec` scaffolds the PyInstaller
  single-file build (closes blocker #7 — **CI integration still
  outstanding**). Roadmap reflects new state.

---

## Test state at end-of-session

- Build: both binaries link clean
- Tests: green per every commit's check; suite grew today by ~30
  cases (5 project + 6 audit-subscriber + 4 apply-op contract +
  13 drift-classifier + 4 ai-drift integration + 4 pack-lint
  integration + ~80-iter property test + others). I did not run
  one final post-`b661c88` full pass — recommend a fresh
  `st_unit_tests.exe` run as first action.
- `subarutuner-cli.exe` (the old-name stub binary) is still in
  `build/win-mingw/bin/` alongside `subuwutuner-cli.exe` — cosmetic,
  pre-existing.

---

## Notes / gotchas the next session should know

### Active-rom contract is `active_rom_mut() → nullable`
The single invariant the whole edit path leans on. `nullptr` means
"this slot is read-only" (currently: source). Every new write call
site must check this and bail with a status-line message — there are
defense-in-depth guards in `apply_op` / paste / reset / undo / redo
because the menubar can be bypassed via command-palette or keyboard
shortcut. Pattern: gate the UI affordance AND check the contract
inside the action. See `eb1747c` for the canonical examples.

### Per-ROM `edits.toml` filename collision risk
Working keeps the v1 shape (`edits.toml` at project root). Additionals
get `<id>.edits.toml`. An additional with id `edits` would collide;
`add_additional_rom` doesn't currently reject that. Low priority — the
id is user-typed and obvious — but worth a validate guard if it comes
up.

### `audit.pinned` sidecar is best-effort
`audit.log` itself stays append-only with per-entry CRCs (can't mutate
for flags), so pin state lives in a sidecar. Disk failure on save
loses cross-session pin state but never the audit log itself. Key is
`<ts_ns>:<crc32>` which is collision-proof under `audit::Entry`'s
uniqueness invariant — don't switch to a single-field key.

### AI drift classifier — advisory only
Per `docs/20` and the commit body for `5daafc7`: this is an advisory
text path the user reads. **There is no executor / auto-apply channel
from drift → table writes.** If a future request asks for
"auto-correct based on drift," push back — the design boundary is
deliberate (knock_correction can be a false signal; LTFT can mean
fuel quality OR injector flow OR sensor drift; the diagnosis includes
alternatives precisely because the rules engine can't always pick).

### `ai-drift` and `adaptive-history` deliberately don't share a parser
Both CLI verbs accept similar CSVs but have parallel option parsers.
The note in the `244437e` commit body: they will evolve independently
(`adaptive-history` toward viz knobs, `ai-drift` toward LLM-narration
flags when Tier 2 lands). Don't refactor to a shared parser yet.

### CI gate for `defgen` PyInstaller build
`tools/defgen/defgen.spec` is in tree but CI doesn't run it. Pre-1.0
blocker #7 is therefore "partial" not "done" in `docs/04`. When wiring
the CI step, the spec produces a single-file binary — sanity-test on
all three OSes since PyInstaller fragments per-platform.

### `BeginPopupModal` Z-order pathology (still relevant)
Carried over from yesterday's handoff. Memory:
`project_imgui_popupmodal_invisible.md`. Startup-fired modals need
plain `ImGui::Begin` + manual dim overlay. The settings-theme-picker
shipped today uses the normal modal path (user-triggered, fine).

### Help modal find-mode degrades structure
`d3a6835` — when find input is non-empty, the markdown renderer
switches to flat per-line rendering (no tables / lists / headings).
That's deliberate (line-based filter is the cheap path) but if anyone
adds richer block formatting to help content later, the find-mode
view will lose more than it does today.

### Sidebar drag-reorder mutation is deferred to post-loop
`07c9d61` — drop semantics happen after the render loop completes so
the tree render is consistent across the drop frame. If you add more
drag affordances to the sidebar, mirror that deferral.

---

## What's still open

### Real-world unblockers (unchanged from yesterday)
1. **Bench-rig hardware** — junkyard 2017 WRX ECU still pending per
   memory `project_bench_rig_awaiting_ecu.md`. ETA was 2026-05-27/28
   in that memory — if the user has it now (it's 2026-06-03), opening
   the Findings/ HANDOFF and walking `docs/28` unblocks Phase 4 HIL.
2. **RH850 codegen bench validation** — still gated on the bench rig.
   No change today.

### Pre-1.0 ship blockers (`docs/04`)
- #7 (defgen frozen) — **partial**: spec in tree, CI step missing
- #8 (audit subscribers) — **done** today (`e70c40e`)
- #9 (OFL-1.1 text) — **done** today (`f13ccd4`)
- #11 (UDS state-machine property test) — **done** today (`e6112f6`)
- Other entries in `docs/04` should be re-read against today's state
  — the roadmap was touched in `f13ccd4` but only for #7/#8/#9.

### Things to ship next (no specific user direction)
- `pack-lint` is text-and-JSON but the GUI has no surface for it yet
  — a "Validate pack" button in Settings or the welcome panel would
  match the existing pattern (`project-validate` → settings dialog
  "Health check" button shipped earlier this week).
- AI Tier 2 from `docs/20` (LLM narration on top of the drift
  classifier output) — `ai-drift` verb has the parser headroom for
  `--narrate` / model flags but the engine call site is not stubbed.
- Audit panel: bulk pin/unpin (currently per-row only) — would match
  the welcome-recents bulk-clear pattern.
- Sidebar order: there's no "Reset to default" affordance yet. Drag a
  category, change your mind, delete `sidebar_order.txt` manually.
- Knock-snapshot / coldstart-analyze JSON ship today but no CSV — if
  CI / dyno automation prefers tabular, that's a cheap follow-up.
- The "Old name" stub binary `subarutuner-cli.exe` could be cleaned
  out of CMake — pre-existing, not from today.

### Watch-list for cumulative complexity
- Per-ROM history saved files now scale with the number of additional
  ROMs (one `<id>.edits.toml` per slot with non-empty history). At >5
  additionals the project root gets cluttered — a `histories/`
  subdirectory is the obvious next step but not urgent.
- `apply_op`'s audit hook now also reads `view_rom()` for the `rom`
  field on additionals. Per-cell-edit cost still cheap.
- The CLI integration tests subprocess-launch the binary — Catch2's
  parallelism + Windows process-launch cost mean these will get
  noticeably slower as more verbs join. Today's 12-verb fixture runs
  in under 5 s; revisit if it climbs past ~30.
- Help modal find-mode rebuilds the line list per frame from the body.
  Cheap on the existing doc content (Overview / Glossary / Roadmap)
  but if many more docs join, cache the parsed line list.

---

## Commands the next session might want first

```sh
# Confirm everything's green from a clean state
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# What CLI surface exists now (12 verbs added recently)
./build/win-mingw/bin/subuwutuner-cli.exe --help | head -80

# Today's new CLI verbs against the demo project
./build/win-mingw/bin/subuwutuner-cli.exe pack-lint fixtures/demo-pack
./build/win-mingw/bin/subuwutuner-cli.exe audit stats fixtures/demo.stune
./build/win-mingw/bin/subuwutuner-cli.exe project-set-active-rom fixtures/demo.stune working
./build/win-mingw/bin/subuwutuner-cli.exe ai-drift fixtures/samples/adaptive_history_demo.csv

# GUI visual check of today's new surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   View → Active ROM           → working / source / additionals
#   View → Audit                → ★/☆ pin column + Pinned filter
#   View → Compare              → Copy B→A
#   View → Adaptive History     → drift diagnosis card inline
#   Tools → Settings            → theme picker with swatches
#   Help (F1) → Ctrl+G          → find-in-topic
#   Sidebar                     → drag category headers to reorder
```

---

## TL;DR for the next Claude

> 22 commits today; everything on `origin/main = b661c88`. The two
> deep architectural items from yesterday (#10 multi-ROM, #8 transport
> audit) and every medium-polish item are closed. Pre-1.0 blockers
> #8/#9/#11 are done; #7 is partial pending CI.
>
> The new front this session: **AI Tier 1 drift classifier shipped
> end-to-end** (`st::ai::drift` engine → `ai-drift` CLI → inline panel
> chip). Pure-function rules engine, advisory-only by design — no
> auto-apply channel, ever.
>
> Open items: bench-rig hardware (carried over), RH850 bench
> validation (gated on rig), CI step for `defgen` PyInstaller spec.
> Real-world unblocker if the junkyard ECU arrived: open the Findings
> HANDOFF and walk `docs/28`.
>
> Watch the `active_rom_mut() → nullable` contract on any new edit
> path. Watch the deliberate parallel parsers on `ai-drift` vs
> `adaptive-history` (don't unify them — `docs/20` Tier 2 wants the
> headroom).
>
> Off you go.
