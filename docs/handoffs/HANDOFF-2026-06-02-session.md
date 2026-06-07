# Handoff — 2026-06-02 (long autonomous session)

**`origin/main` = `4d0cefb`**. 66 commits pushed across 11 sprints + the
opening wizard fix + the pre-handoff caveman-review fixes. Tree clean
except this HANDOFF file. Tests green throughout (1292 cases, ~155k
assertions on the last full run).

---

## How the session ran

Started against the handoff at `HANDOFF-2026-06-01-night.md`. The first
priority was the unresolved first-run-wizard invisibility bug; that
landed in `48d16e5` and the workaround pattern (plain `ImGui::Begin` +
manual dim overlay instead of `BeginPopupModal`) is captured in memory
at `project_imgui_popupmodal_invisible.md`. From there the user
directed continuous shipping — 11 sprints, each ~4 tasks — punctuated
by spot-tests on the FehrTune project and the demo project.

The session's center of gravity was UX completion across already-shipped
modules: every panel that needed a chip filter got one, every shareable
data surface got a Copy + Export pair, every keyboard-driven workflow
got the obvious shortcut. The CLI surface roughly doubled.

---

## What's on `origin/main` that wasn't there at session start

### New CLI subcommands (12)
| | |
|---|---|
| `project-add-rom` | Register an additional ROM (Issue #10 read slice) |
| `project-list-roms` | List source / working / additional ROMs (text + JSON) |
| `project-validate` | Health check: project.toml, ROMs, pack, additional_roms, audit integrity |
| `project-clone` | Duplicate project to new dir; rewrites pack path to absolute |
| `audit append` | Write entries from scripts / CI / cron |
| `audit verify` | Walk CRC32 integrity; exit 1 on tamper |
| `stats` | Per-table min/max/mean/stddev/p10/p50/p90 (text + JSON) |
| `table-grep` | Case-insensitive substring search across id/name/category |
| `profile create` | Initialize a fresh `.stprofile` |
| `profile delete` | Refuses without `--yes` |
| `changelog show` | Print `[Unreleased]` section (`--all` for full file) |
| `completion bash` / `completion zsh` | Tab-completion script exporter |
| `project-info --audit-summary` | Adds a per-kind bucketed audit summary block |

### New GUI panels / modals
| | |
|---|---|
| Audit log panel | NDJSON viewer + kind chips + time-range slider + sparkline + Refresh / Reveal / Export NDJSON / per-row right-click |
| Help modal | Two-pane docs browser (Overview / Glossary / Roadmap), Ctrl+F focuses filter, ↑↓ topics, Home/End first/last, PgUp/PgDn content, F1 toggles open/close |
| Compare panel ROM picker | Project source / working / additional_roms with `→ A` / `→ B` slot buttons + chip filter (safety/emissions/category) + Jump-to-biggest (Ctrl+G) + Swap A↔B + Export CSV/MD + Copy MD |

### New audit hooks
project lifecycle (open, save), autotune commits (knock-pull, MAF), CSV
bulk import, every `apply_op` toolbar action (+5%, -5%, Smooth,
Interpolate, paste, reset, typed edits), Read ROM modal (started /
completed / failed / cancelled with CRC + byte count), DTC bulk toggles.

### Existing-panel polish
- Sidebar: glossary tooltips on category headers, keyboard `E` / `S` jumps
  to first emissions / safety table
- DTCs: bulk-toggle row (Disable all emissions / Enable all / Disable all)
  recorded as one ByteEdit per batch
- Table view: knock overlay on grid (Issue #16), per-cell hover with
  metrics; Copy table as TSV button
- Stats panel: p10/p50/p90 rows, Copy TSV / Copy MD buttons
- History panel: op-kind chip filter
- Knock dashboard: column auto-detect + save/load presets + Export CSV
- Read ROM modal: Size pre-fill from pack, SA-variant pre-fill from
  VehicleProfile.transport_hint, Add-to-project flow + timestamp slug
- Welcome panel: dynamic "What's new" parsed from `CHANGELOG.md`,
  recents filter (when ≥4 entries), arrow-key + Enter recents nav
- Settings modal: vehicle-profile picker, project metadata editor,
  Reveal config.toml
- Flash modal: vehicle-profile context line
- Glossary tooltips at strategic UI labels (welcome ECU, DTC count,
  knock datalog, sidebar category headers)
- Command palette entries for every new panel/modal + F1 / Ctrl+G /
  Ctrl+F bindings noted in the shortcuts modal

### The "your WIP" big ship
Commit `11e8bab` unifies BackupStore + FlashPreflight + plugin seams
(`st/core/ext/`) + diagnostic tier (`st/core/diagnostic.hpp`) +
CHANGELOG / SECURITY / THIRD-PARTY-INSPIRATIONS. That was the user's
in-flight WIP from the prior session — staged for them with a unified
commit message rather than left in working tree.

### Bug fixes mid-session
- `e089624` — wizard's "Open demo project on Finish" was calling
  `resolve_demo_project_path(nullptr)`; fixed to use the cached
  `state.demo_project_path`.
- `d05045f` — F1 help modal showed `?` for `✅` `⬜` `→` because Inter
  has no glyphs for those ranges. Merged Segoe UI Symbol into the body
  font atlas; `strip_non_bmp` filter drops non-BMP emoji (🟡 🔒) which
  16-bit ImWchar can't address.
- Caveman-review pre-push fixes (`4d0cefb`):
  audit "Copy as NDJSON" now disabled on tampered entries (was hiding
  CRC corruption by recomputing); `profile create` mkdir errors now
  surface with a clear message.

---

## Test state at end-of-session

- Last full run: **1292 cases / 155,490 assertions all green**
- Both binaries link clean (`subuwutuner-gui.exe`, `subuwutuner-cli.exe`)
- The audit-related rebuilds added cumulative test cycles; no flake.
- Demo project's `audit.log` and `edits.toml` are in `.gitignore` so
  smoke-testing doesn't dirty the fixture.

---

## Notes / gotchas the next session should know

### The `BeginPopupModal` Z-order pathology
Documented in memory at `project_imgui_popupmodal_invisible.md`.
Modals that open at startup (first-run wizard, future onboarding
flows) hit a Z-order bug where the popup window's body never
renders even though the dim overlay does. Use the plain
`ImGui::Begin` + manual dim-overlay pattern — see
`src/ui/src/modals/first_run.cpp` or `help.cpp` for the canonical
shape. Modals that only open from user button clicks (Flash,
Settings, About, etc.) keep using `BeginPopupModal` and work fine.

### `precision` variable shadows in table_view.cpp
The render_table_view function defines a `precision` at the same
level as render_table_grid's `precision`. New code at the
render_table_view level (like the Copy table button) can reach
the outer `precision`. Watch for variable-shadow warnings if you
add helpers between them.

### Non-BMP emoji rendering
ImGui's default `ImWchar` is 16-bit. Codepoints above U+FFFF
(most emoji) can't be addressed at all. The help modal's
`strip_non_bmp` filters 4-byte UTF-8 sequences to `*` so doc
content doesn't show tofu. If anyone wants real emoji rendering
in the future, that means enabling `IMGUI_USE_WCHAR32` + loading
an emoji font.

### Glyph coverage
Inter (body font) lacks BMP symbols (✅ ⬜ → ↻). `theme.cpp`
merges Segoe UI Symbol over those ranges. Mac/Linux fallbacks
load Apple Symbols / DejaVuSans but the coverage is less
predictable. If a non-Windows user reports tofu in the help
modal, that's the first thing to check.

### Audit panel forensic semantics
`Copy as NDJSON` on a single row is disabled when
`checksum_valid=false` because `serialize_entry` recomputes the
CRC32 and would launder the tampering. The forensic path is
"Open log location" → share the raw `audit.log` file. Same
constraint applies if anyone adds a "Copy all" path in the
future — `serialize_entry` resets the CRC for every entry.

### Watch-list for cumulative complexity
- `apply_op` in `actions.hpp` is a template, and we added an audit
  hook in its body. Every `apply_op` call now does a string copy
  of the label + a small audit fields vector. Cheap, but if
  per-cell-edit performance ever becomes a concern this is a
  candidate for batching.
- Audit panel auto-refresh polls `last_write_time` every visible
  frame. Single stat per frame; not a problem at 60 fps. If
  someone later wires a watch-fs API the poll can go away.
- Several CLI subcommands have grown ad-hoc arg parsers. If the
  count climbs much further it might be worth introducing a small
  arg-parse helper.

---

## What's still open (priority order)

### Big architectural items
1. **#10 deep multi-ROM refactor** — per-ROM `edit::History` isolation,
   active-ROM switching, refactor of the 41 call sites of
   `source_rom()` / `working_rom()`. Read slice + CLI shipped, but the
   "switch which ROM is active" surface is not there.
2. **#8 deep transport audit subscribers** — `AuditLog*` plumbed
   through `transport::ITransport`, `ecu::uds`, `ecu::ssm`, `flash`
   modules so the wire-level events (`SecurityAccessUnlocked`,
   `FlashSectorWritten`, etc) fire without explicit caller code. GUI
   project-lifecycle + ROM-read hooks shipped; this is the deeper
   plumbing.

### Real-world unblockers
3. **Bench-rig hardware in hand?** — junkyard ECU was awaiting arrival
   per memory `project_bench_rig_awaiting_ecu.md`. If it's now here,
   walking `docs/28-bench-rig-build.md` against the rig is the next
   move and unblocks Phase 4 HIL tests.
4. **RH850 codegen bench validation** — still gated on bench-rig
   availability per the CLAUDE.md status snapshot.

### Medium polish that didn't fit
- Audit panel: "pin" / "star" entries for review later
- Welcome panel: pin / star recents to top of list
- Sidebar: drag-to-reorder categories
- CLI: shell completion for *flags* (currently only subcommands)
- Settings: theme preview slider
- Help modal: full-text search highlighting within content
- New project modal: drag-and-drop ROM into source path
- Compare panel: "Apply A→B" workflow to copy diffs across projects

### Test coverage gaps
- The CLI's new verbs (audit append/verify, project-clone,
  project-validate, stats, table-grep, profile create/delete,
  completion, changelog show) have no integration tests yet. Smoke-
  tested live against fixtures/demo.stune during the session but no
  Catch2 cases added.
- The audit hook in `apply_op` isn't covered by a regression test —
  could land in `tests/unit/audit/` as a "does it write an entry per
  apply_op?" check.

---

## Commands the next session might want first

```sh
# Confirm everything's green from a clean state
cmake --build build/win-mingw --target st_unit_tests
./build/win-mingw/bin/st_unit_tests.exe

# What CLI surface exists now
./build/win-mingw/bin/subuwutuner-cli.exe --help | grep -E "^\s+\w" | head -50

# Demo project smoke-test for the new flows
./build/win-mingw/bin/subuwutuner-cli.exe project-list-roms fixtures/demo.stune
./build/win-mingw/bin/subuwutuner-cli.exe project-validate fixtures/demo.stune
./build/win-mingw/bin/subuwutuner-cli.exe stats fixtures/demo.stune --table boost_target_high_octane
./build/win-mingw/bin/subuwutuner-cli.exe table-grep fixtures/demo.stune boost

# GUI on demo for visual check of the new surfaces
./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
#   F1            → help modal
#   Ctrl+K        → command palette (every new panel + modal listed)
#   View → Audit  → kind chips, sparkline, time range
#   View → Compare → Project ROMs picker, Jump-to-biggest button
```

---

## TL;DR for the next Claude

> 66 commits this session; everything on `origin/main = 4d0cefb`. The
> wizard works, every panel has its chip filter / export pair / keyboard
> shortcut, the CLI doubled in size, the audit story is complete on
> three sides (read + write + integrity), multi-ROM read slice is wired
> through both GUI and CLI, vehicle profiles are usable.
>
> Open big items: #10 deep multi-ROM (active-ROM switching, per-ROM
> history) and #8 deep transport audit. Both need focused sessions.
>
> Watch the `BeginPopupModal` pathology (in memory) — use plain `Begin`
> + dim overlay for any new startup-fired modal. Watch the non-BMP
> emoji constraint when adding new doc content (use BMP fallbacks or
> the `strip_non_bmp` filter will turn them into `*`).
>
> Off you go.
