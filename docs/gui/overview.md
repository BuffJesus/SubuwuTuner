# GUI overview

`subuwutuner-gui` is the ImGui-based docking UI. Everything it does is
also reachable from `subuwutuner-cli` — the GUI exists for the surfaces
where a table heatmap, a 3D slice picker, a live gauge cluster, or a
visual node graph wins over a shell.

## Workspaces

Three top-level workspaces, swap with <kbd>Ctrl</kbd>+<kbd>1</kbd> /
<kbd>2</kbd> / <kbd>3</kbd>:

| Workspace | Purpose |
|---|---|
| **Tune** | Table editor, history panel, compare panel, sidebar nav |
| **Datalog** | Live gauge cluster, CSV playback, knock snapshot inspector |
| **Features** | Custom-features designer canvas, IR inspector, codegen preview |

## Panels in the Tune workspace

| Panel | Default position | Key |
|---|---|---|
| Welcome | Center (first run) | — |
| Sidebar (hierarchical nav) | Left | — |
| Table view (heatmap + cells) | Center | — |
| History | Right | <kbd>Ctrl</kbd>+<kbd>H</kbd> |
| Compare (project vs source) | Center (toggle) | — |
| Stats | Right (tab) | — |
| Audit | Right (tab) | — |
| Settings | Modal | — |

The sidebar is a hierarchical 9-group tree (Fuel / Ignition / Boost /
AVCS / Knock / DTC / Special Functions / Misc / Tuner Atlas). Both
levels default-closed; expand as you work.

## Global shortcuts

| Key | Action |
|---|---|
| <kbd>F1</kbd> | Context-aware help (jumps to the doc topic for the active panel) |
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | Command palette — search every action, table, recent file |
| <kbd>Ctrl</kbd>+<kbd>1</kbd> / <kbd>2</kbd> / <kbd>3</kbd> | Switch workspace |
| <kbd>Ctrl</kbd>+<kbd>S</kbd> | Save project |
| <kbd>Ctrl</kbd>+<kbd>Z</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd> | Undo / redo |
| <kbd>Ctrl</kbd>+<kbd>O</kbd> | Open project |
| <kbd>Ctrl</kbd>+<kbd>N</kbd> | New project |

## Theming

Dark / Light themes with **purple accent** `(0.55, 0.35, 0.85)`. Pick
in Settings → Appearance. Theme choice persists in the per-platform
settings file.

## Where state lives

| File | Purpose |
|---|---|
| `%APPDATA%\SubuwuTuner\settings.toml` (Windows) | UI prefs, theme, jurisdiction, recents |
| `%APPDATA%\SubuwuTuner\recents.txt` | Recent project paths |
| `%APPDATA%\SubuwuTuner\definitions\` | Convention dir for definition packs |
| `imgui.ini` (next to the GUI binary) | ImGui dock layout |

Linux: `~/.config/subuwutuner/`. macOS: `~/Library/Application Support/SubuwuTuner/`.

## In-progress pages

Per-panel guides (Table view, History, Sidebar, Datalog cluster,
Features designer) are being written. Until they land, hit
<kbd>F1</kbd> inside the panel — the in-app help links into the design
docs at
[`docs/`](https://github.com/BuffJesus/SubuwuTuner/tree/main/docs){ target="_blank" }
for module-level detail.
