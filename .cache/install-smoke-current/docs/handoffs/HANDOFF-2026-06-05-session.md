# Handoff — 2026-06-05 session

**`origin/main = 2633a17`** — eleven commits across four sprints,
all green at each checkpoint. Tree clean except this handoff +
the five WIP files already modified at session start
(`.claude/HANDOFF.md`, `.gitignore`, six `.idea/runConfigurations/`,
`CHANGELOG.md`, `CLAUDE.md`, `THIRD-PARTY-INSPIRATIONS.md`,
`definitions/README.md`, `docs/{08,15,17,28}*.md`,
`docs/analyst-mode-prompt.md`, `src/ui/src/modals/settings.cpp`
hidden-cats addition, `tests/private/{README,example_private_test
.cpp.template}`, `tests/unit/flash/test_read_full_rom_sa_v4_port
.cpp`). **None of the working-tree WIP was touched this session.**

The session ran four sprints across IDE-config, polish, the open
small-items list, and three of the four green-lit architectural
items.

---

## What shipped

### Sprint 1 — IDE-config + dead-code drop (1 commit)

```
7abcf28 build: unbreak clang-cl preset, drop dead implot_demo
```

CMakeUserPresets.json (gitignored, local-only) added with
captured vcvars64 env so the IDE's `win-msvc` / `win-clang`
presets stop failing on PATH-less `cl.exe` / `clang-cl.exe`.
`/Zc:preprocessor` gated to real cl (clang-cl rejects as
-Wunused-command-line-argument; under /WX → hard error).
`implot_demo.cpp` dropped from `implot_lib` — confirmed
unreferenced (only `ImGui::ShowDemoWindow` is wired, ImPlot's
demo entry was dead code emitting -Wdeprecated-enum-enum-
conversion noise under GCC 15).

### Sprint 2 — small handoff items (2 commits)

```
b6cc9f1 feat(ui): persist help-modal active topic across sessions
6e66293 feat(ui): Compare panel bulk pin/unpin popup
```

Help modal now restores active topic from `settings.txt`
(`help_active_topic_id` field, mirror of the topic id from
TopicSource::filename). F1 per-panel routing still overrides
— context wins over memory. Compare panel gains `Bulk pins ▾`
button (mirrors audit's 5797fc3 pattern) with Pin N visible /
Unpin N visible / Clear all pins. Scope = chip filter +
include_identical, excludes Pinned-only (acting on that set
would make "Pin N visible" always 0).

### Sprint 3 — share-with-tuner + CI mutation + harness (4 commits)

```
26bec45 feat(ui): Compare Markdown export — All / Pinned scopes
a8be408 feat(ui): sidebar bulk reorder — Alphabetize + Group by safety
0842c4d feat(ui): Compare NDJSON export — All / Pinned scopes
203d5aa ci(mutation): add subaru_security:1-499 to advisory lane
079e985 tools(mutation): skip raw-string + char-literal operators
```

Compare's Export Markdown… reshaped to Export Markdown ▾ with
All/Pinned scopes (mirror of a2ebdb4 audit pattern); render_diff
_markdown gained optional `pinned_filter` param so Copy MD path
is unchanged. Stale-pin gap surfaced via subtle text when the
pinned set references tables not in the current diff.

NDJSON export added as a peer — per-line `subuwutuner.diff.table
.v1` schema (superset of `.diff.v1`'s `tables[]` entries, each
self-contained with pack_id + rom CRCs). Inline `json_escape_
inline` duplicates `src/diff/src/diff.cpp:101`'s helper —
follow-up to hoist into st::diff public API.

Sidebar got a "Reorder categories" submenu with Alphabetize (A-Z)
and "Group by safety (N first)" actions. Both rewrite sidebar
_category_order from scratch + persist via save_sidebar_category
_order. Group-by-safety disables on 0-or-all-flagged no-op cases.

CI mutation advisory lane added `subaru_security.cpp:1-499`
(`[sa]` tag-filter, max 20 mutants, ~5 min). Total lane is now
~25 min — under the 30-min budget but the next addition needs
either parallelization or trimming an existing range.

Mutation harness's `_is_in_string_literal` extended to skip raw
strings (`R"<delim>(...)<delim>"`, only at word boundary —
`someR"abc"` identifier won't false-trigger) and char literals
(`'<'`, `'>'`, escape-aware). 18 unit tests under
`tools/tests/test_mutation_string_skip.py` lock in regression
vectors. Documented gaps carried forward: multi-line raw
strings (need state threaded across lines + inline-comment
awareness), wide/utf raw-string prefixes (LR"/uR"/u8R"/UR"/
U8R" — SubuwuTuner's src/ doesn't use them).

### Sprint 4 — 3 of the 4 green-lit architectural items (3 commits)

```
116a586 feat(ui): persist Compare panel last-comparison setup
339a174 feat(ui): VehicleProfile status-bar chip + quick-switch popup
2633a17 feat(ui): AI Tier 2 narration settings — provider + key plumbing
```

Per the user's green-light decisions:
- **Compare rebuild → incremental** (not full rewrite)
- **VehicleProfile shape → Claude's call** — landed minimal,
  surface existing pieces
- **Cross-session AuditLog → per-CID** (NOT per-VIN). Deferred
  to next sprint (bigger scope: needs Entry schema change +
  wiring every append call site)
- **AI Tier 2 → user supplies API key, Claude preferred but
  OpenAI offered, opt-in toggle**. Plumbing only this commit
  — no engine call site yet
- **Multi-ROM diff → after Compare rebuild** (deferred — the
  Compare incremental polish is mostly done, structural blocker
  cleared)
- **Live-to-table → Claude's call** (deferred this sprint)

**Compare config persistence**: new `<project>/compare.config`
sidecar (key=value) for rom_a_path / rom_b_path / epsilon /
include_identical / filter_chip. Saves on Run Compare click +
chip toggle (pre-Run tweaks don't save until commit). app_state
.cpp resets the in-memory buffers to defaults BEFORE the load
attempt so a project-switch with no sidecar doesn't leak the
previous project's picker values.

**Profile status-bar chip**: "Profile: <id> ▾" after the
jurisdiction chip. Muted color when none set, accent when active.
Click → popup combo listing every profile under `st::profile::
default_profile_dir()`. Selection updates Settings.active_vehicle
_profile_id + save_settings. Directory re-read on each popup
open (no cache — typically 1-5 cars on file).

**AI Tier 2 plumbing**: new AiProvider enum {Anthropic, OpenAI}.
Settings gains `ai_narration_enabled` / `ai_provider` /
`ai_api_key` / `ai_model`. New Settings → AI tab with toggle +
provider radio + masked key input + model hint (claude-opus-4-7
/ gpt-4o per provider). API key stored plaintext in settings.txt
per user direction (local-user trust model). NO engine call
site — only the settings + GUI plumbing.

---

## Cumulative outcomes

### Tests

- 1369 cases (no new C++ tests added — most work was incremental
  GUI / persistence)
- 18 new Python unit tests for the mutation harness skip logic
  (`tools/tests/test_mutation_string_skip.py`)
- Test count unchanged from session start; all green at each
  checkpoint

### CI mutation lane

5 ranges now in the advisory lane:

  | File                                       | Range   | Tag       |
  |--------------------------------------------|---------|-----------|
  | `src/flash/src/flash.cpp`                  | 500-560 | [flash]   |
  | `src/flash/src/flash.cpp`                  | 400-499 | [flash]   |
  | `src/flash/src/backup_store.cpp`           |  30-40  |[backup_store]|
  | `src/policy/src/flash_preflight.cpp`       |  65-90  | [policy]  |
  | `src/ecu/src/subaru_security.cpp`          |   1-499 | [sa]      |

Budget: ~25 min total. Past 30 min the lane needs parallelization
(GitHub Actions matrix on the mutation steps) before a sixth
range can land.

### CLI

No CLI changes this session — all work was GUI / harness / CI.
JSON envelopes unchanged.

### GUI surfaces touched

Substantial polish + three architectural surfaces:

- **Help modal**: persistent active topic across sessions
- **Compare panel**:
  - Bulk pin/unpin popup (Pin N / Unpin N / Clear all)
  - Export Markdown ▾ with All / Pinned scopes
  - Export NDJSON ▾ with All / Pinned scopes
  - Last-comparison setup persistence (rom A/B, epsilon, chip)
- **Sidebar**: bulk reorder (Alphabetize / Group by safety)
- **Status bar**: VehicleProfile chip + quick-switch popup
- **Settings modal**: new AI tab (provider config + key)

---

## File-level state

### New on-disk sidecars (per-project)

  | File                  | Source commit | What                            |
  |-----------------------|---------------|---------------------------------|
  | `compare.config`      | `116a586`     | last-comparison picker setup    |

Combined with prior sidecars (`edits.toml`, `audit.log`,
`audit.pinned`, `sidebar_order.txt`, `sidebar_hidden.txt`,
`compare.pinned`, `flash.journal`, `pack-lint.toml`,
`histories/<id>.toml`), `.stune/` now has ~10 possible sidecars.
**Already past the watch-list threshold** that the prior handoff
flagged: "consider a state/ subdirectory if it grows past ~10."
Worth picking up.

### New global settings.txt fields

  - `help_active_topic_id` (`b6cc9f1`)
  - `ai_narration_enabled` / `ai_provider` / `ai_api_key` /
    `ai_model` (`2633a17`)

### Settings struct grew

```cpp
struct Settings {
    // ...existing fields...
    std::string help_active_topic_id;          // b6cc9f1
    bool        ai_narration_enabled{false};   // 2633a17
    AiProvider  ai_provider{AiProvider::Anthropic};
    std::string ai_api_key;
    std::string ai_model;
};
```

New helpers in persistence.cpp / .hpp:

- `enum class AiProvider { Anthropic, OpenAI }`
- `char const *ai_provider_name(AiProvider) noexcept`
- `std::optional<AiProvider> parse_ai_provider(string_view) noexcept`
- `struct CompareConfig`
- `load_compare_config(project_dir) -> optional<CompareConfig>`
- `save_compare_config(project_dir, CompareConfig)`

### CMakeUserPresets.json (gitignored)

Local file at `D:\Subuwu\code\CMakeUserPresets.json` carries the
captured vcvars64 env for `win-msvc-local` / `win-clang-local`
presets. **Tied to this developer's VS install version
(14.38.33130).** If VS is updated, re-run the capture flow:

```sh
cmd.exe /c "D:\Subuwu\code\.cache\vcvars_dump.bat" > /tmp/vc_env.txt
# (then the PowerShell ConvertTo-Json flow from the session log;
#  or just delete CMakeUserPresets.json and ask Claude to regenerate)
```

### .cache/vcvars_dump.bat

Helper file at `.cache/vcvars_dump.bat` is the source for the
vcvars capture. Stays in tree — small, useful for the regenerate
flow, gitignored under the existing `.cache/` rule.

---

## Notes / gotchas the next session MUST know

### Compare panel rebuild is mostly done as incremental polish

The handoff §"Big architectural items" listed Compare panel
rebuild as a structural item. As of this session, the
incremental path has shipped: bulk-pin, MD/NDJSON scope exports,
last-comparison persistence. **Structural rewrite is no longer
needed** — what's left is point-feature work (annotations
per-diff, compare history, side-by-side multi-ROM view) that
can be done as small commits. Don't open up a "Compare panel
rebuild" branch.

### Multi-ROM diff is unblocked

The handoff said "Multi-ROM cross-ROM diff" pairs with Compare
rebuild. That pairing is satisfied — start work whenever the
user asks. Likely shape: extend `st::diff::DiffSet` to carry
N ROMs (or compose two-way diffs in the GUI layer), add a
"third ROM" picker to the Compare toolbar.

### Cross-session AuditLog sink is the next architectural item

Per the user's per-CID direction:
1. Add `cid` (and optionally `vin`) field to `st::audit::Entry`
   (`src/audit/include/st/audit.hpp:94-107`)
2. Wire CID source — likely from active VehicleProfile
   (`profile.ecus[0].cal_id`) AND/OR the project's ROM cal-id
   slot if read at flash time
3. Wire CID-on-emit at every append call site listed in the
   sub-agent scoping report — most are in `src/ui/`,
   `src/flash/`, `src/ecu/`. 9 sites total per the scoping pass.
4. Add a centralized reader/writer at `config_dir_root()/
   audit-by-cid/<CID>.log` that appends every entry alongside
   the per-project `audit.log`
5. New GUI surface: a Help → Audit (per-vehicle history) panel
   that reads from the per-CID file

Schema change to Entry needs a serialization version bump (or
new keys with backward-compat default). Wire format example
already in `src/audit/include/st/audit.hpp:104` — add `cid`
field after `description`.

### AI Tier 2 engine is the next architectural item after AuditLog

Plumbing shipped; engine site is the next step. Per docs/20:
1. New `st::ai::Backend` trait at `src/ai/include/st/ai/
   backend.hpp` (sketched in docs/20 line 200-216)
2. `st::ai::make_anthropic_backend(api_key)` impl — minimal
   HTTPS client to api.anthropic.com /v1/messages, default
   model `claude-opus-4-7`
3. `st::ai::make_openai_backend(api_key)` impl — same shape
   to api.openai.com /v1/chat/completions, default `gpt-4o`
4. Call site in `src/ui/src/panels/adaptive_history.cpp`
   (after the rules-based DriftDiagnosis) — opt-in gated by
   `state.settings.ai_narration_enabled`
5. Confirm dialog showing the exact prompt before transmission
   per docs/20 §"Privacy and safety posture" item 2
6. CMake flag `-DST_AI=ON` per docs/20 architectural-fit note
7. `BackendInfo::name()` surfaces in the GUI badge per docs/20
   line 218

For the HTTPS client: rather than bringing in a heavy dep,
shelling out to `curl` is acceptable for v1 (cross-platform,
already on the user's path for prod, sidesteps cert-bundle
questions on Windows). Document the choice.

### settings.txt grew API key field — but no read-locking

`ai_api_key` is stored plaintext in `settings.txt`. The
read-on-app-start path doesn't lock the file or check
permissions. On Windows the LOCALAPPDATA dir is per-user by
default but the file itself isn't ACL'd. Documented as user-
direction; not adding crypto is an explicit choice. If a future
sprint adds telemetry, **double-check it doesn't slurp
`settings.txt` into a crash dump.**

### Compare.config buffer reset is load-bearing

`src/ui/src/app_state.cpp` resets `compare_rom_a_path` /
`compare_rom_b_path` / `compare_epsilon` /
`compare_include_identical` / `compare_filter_chip` to defaults
BEFORE the `load_compare_config` attempt. **Do not refactor the
load to a "load OR keep current" pattern** — that re-introduces
the project-switch leak the review caught in the original
diff.

### Mutation harness skip is single-line

`_is_in_string_literal` handles raw strings + char literals
on a single line. Multi-line raw strings (R"( on one line, )"
on another) are NOT threaded. If CI starts surfacing
SURVIVED-but-actually-in-raw-string mutants from src/feature/
codegen tables, the fix is to refactor find_candidates to walk
from line 1, threading raw-string state across iterations,
emitting candidates only when ln ∈ [line_start, line_end].
Sketched in the session conversation under
"Approach 2: Refactor to a function that returns the line-end
state."

### Profile chip lists from default_profile_dir per popup-open

`src/ui/src/panels/status_bar.cpp` re-reads
`st::profile::list(default_profile_dir())` every popup open.
This is fine for 1-5 cars; if a user has 50 profiles the popup
will feel sluggish. Cache + invalidate on save_settings if it
becomes a problem.

### Orphan active_vehicle_profile_id is silently surfaced

If the `.stprofile` file backing
`Settings.active_vehicle_profile_id` is deleted out from under
the GUI, the chip still shows the orphan name in accent color
and the popup doesn't surface the mismatch. User can click
`(none)` to clear. Soft pointer; fix with a "missing — click
to clear" affordance when the dropdown can't resolve the id.

### Settings → AI tab model field doesn't reset on provider switch

A user who types `claude-opus-4-7` then switches the radio to
OpenAI keeps the Anthropic model id in their model field, which
would be sent to the OpenAI API. Caught in review; user accepted.
Document or auto-clear on provider radio change.

### CMakeUserPresets.json is VS-install-version-bound

The presets bake `14.38.33130` paths (the user's installed MSVC
toolset). VS auto-updates can change this. If the IDE starts
failing again on configure with "cannot open …14.38.33130\
include\…", regenerate via the .cache/vcvars_dump.bat helper.

### Mutation lane is at 25/30 min budget

Adding any sixth range without parallelization will push past
the 30-min advisory-lane cap. Parallelization options:
1. GitHub Actions matrix on the mutation-test steps (5 jobs in
   parallel, ~5 min each)
2. Trim ranges to tighter windows — e.g. flash.cpp:500-560 +
   400-499 could be split + run on different jobs
3. Drop the slowest range (subaru_security:1-499 is the new
   biggest — 499-line window, max 20 mutants)

---

## What's still open

### Pre-1.0 ship blockers (`docs/04`)

No change from start of session. Mutation lane now broader (5
ranges, ~25 min), `continue-on-error: true` still in effect.

### Real-world unblockers

1. **Bench-rig hardware** — junkyard 2017 WRX ECU. Last memory
   entry said expected 2026-05-27/28; **still not in tree as
   of this session**. Memory note `project_bench_rig_awaiting
   _ecu.md` carries the walk-the-docs/28 instructions.
2. **RH850 codegen bench validation** — same.

### Big architectural items (green-lit but not done)

After this session's deliveries:

- **Cross-session AuditLog (per-CID)** — described above. Highest
  priority of the remaining items per user's "fix everything"
  scope.
- **AI Tier 2 engine** — described above. Plumbing done; needs
  the Backend trait + per-provider HTTPS client + opt-in
  confirm dialog.
- **Multi-ROM cross-ROM diff** — described above. Compare
  rebuild "pair" is satisfied; structural blocker gone.
- **Live-to-table cross-reference + knock overlay (#15/#16)**
  — still wants user sign-off on the integration shape.

### Smaller items the next session could pick up

Filtered to items still actionable without architectural
sign-off:

- **Hoist `json_escape` into `st::diff` public API** — flagged
  in commit `0842c4d`'s body; current inline duplicate of
  `src/diff/src/diff.cpp:101` is a follow-up. Tiny commit.
- **`.stune/state/` subdir for sidecars** — past the 10-sidecar
  threshold the prior handoff flagged. Migration: move
  existing sidecars into a new `state/` subdirectory with a
  legacy-read fallback (mirror the histories/ migration pattern
  in `b648e7f`).
- **Orphan profile id affordance** — when
  `active_vehicle_profile_id` doesn't resolve in the popup,
  surface "missing — click to clear" inline.
- **AI tab model auto-clear on provider switch** — flagged in
  review; one-liner.
- **CI mutation parallelization** — needed before lane can
  expand to a sixth range. Matrix-job approach.
- **`Help → Settings → search`** — search across settings
  tabs. Some Settings fields are buried two clicks deep; the
  user occasionally has to remember which tab.
- **`subuwutuner-cli ai-narrate`** — CLI surface for the same
  narration backend (when the engine lands). Useful for batch
  log processing.

### Watch-list for cumulative complexity

- **AppState size** — getting larger; no new clusters this
  session but the 7 `focus_pending_*` bools call from the prior
  handoff still hasn't been collapsed into an enum.
- **Sidecar count per `.stune/`** — past 10. State-subdir
  migration is now the natural fix.
- **CI mutation runtime** — 25/30 min. One more range +
  parallelization.
- **`settings.txt` growth** — now ~10 keys, includes secrets
  (api_key). At 15+ keys with a couple of multi-line values
  consider switching from key=value to TOML.

---

## Commands the next session might want first

```sh
# Confirm test state hasn't drifted
cmake --build --preset win-mingw -j 14
./build/win-mingw/bin/st_unit_tests.exe   # from source root
# Expected: All tests passed (1369 cases, ~159k assertions)

# Run the new Python harness tests
python -m unittest discover -s tools/tests
# Expected: 18 tests in test_mutation_string_skip.py pass

# Sanity-check the new GUI surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   Help → ?  Open then close. Re-open. Topic position carries.
#   Tools → Settings → AI tab. Toggle, pick Anthropic, paste a
#     fake key, switch provider radio, observe model hint flip.
#   View → Compare → Run. Star a few rows. Bulk pins ▾.
#     Export Markdown ▾ → Pinned only.
#     Export NDJSON ▾ → Pinned only.
#   Close project, reopen. Compare picker remembers your last
#     ROM A/B + epsilon + chip.
#   Bottom status bar: "Profile: none ▾" chip. Click. Empty
#     popup with a hint to import via CLI. (Or your existing
#     profiles if you've imported any.)
#   View → Sidebar → right-click. Reorder categories →
#     Alphabetize. Then Group by safety.

# Check the CI run for the new mutation lane step
# https://github.com/BuffJesus/SubuwuTuner/actions
# Expected: "Mutation pass — SubaruSecurity SA Feistel +
# variants" step. 0 SURVIVED would be ideal (matches local).
# 20 BUILD_FAIL would mean Linux-side toolchain mismatch (likely
# a -Werror cascade on a header included by st_unit_tests).

# Regenerate CMakeUserPresets.json if MSVC was updated
cmd.exe /c ".cache\vcvars_dump.bat" > .cache/vc_env.txt
# Then re-run the PowerShell ConvertTo-Json flow from the
# session log (or delete CMakeUserPresets.json and ask Claude
# to regenerate).
```

---

## Deeper-dive references

- **Today's commits**: `git log f402a83..2633a17 --oneline`
- **Prior consolidated handoff**:
  `HANDOFF-2026-06-05-next-session.md` (the previous "next
  session" doc — this one supersedes it for ongoing work)
- **Architectural items doc trail**:
  - `docs/04-roadmap.md` for the ship-gate items
  - `docs/20-ai-integration.md` for AI Tier 2 (matches the
    plumbing landed today)
  - `docs/15-clean-room-engineering.md` if the AI engine work
    raises IP questions about training-data lineage

---

## TL;DR

> 11 commits across four sprints. `origin/main = 2633a17`.
>
> **Sprint 1 (IDE-config)**: clang-cl preset unblocked,
> implot_demo dropped.
>
> **Sprint 2 (small handoff items)**: help-topic persistence,
> Compare bulk pin/unpin.
>
> **Sprint 3 (share-with-tuner + CI)**: Compare MD/NDJSON
> scope exports, sidebar bulk reorder, SA range added to CI
> mutation lane (25/30 min budget), harness skip extended to
> raw-strings + char-literals with 18 Python unit tests.
>
> **Sprint 4 (3 of 4 green-lit architectural items)**: Compare
> last-comparison persistence (compare.config), VehicleProfile
> status-bar chip + quick-switch popup, AI Tier 2 settings
> plumbing (toggle + Anthropic/OpenAI radio + masked key +
> model — NO engine call site yet).
>
> **Architectural next-up**: Cross-session AuditLog (per-CID,
> per user's call), then AI Tier 2 engine (Backend trait +
> per-provider HTTPS clients + confirm dialog).
>
> **Tests**: 1369 / ~159k assertions, all green. +18 Python
> unit tests for the harness skip logic.
>
> **Untouched, hardware-gated**: bench rig (still not in tree),
> RH850 codegen validation.
>
> Off you go.
