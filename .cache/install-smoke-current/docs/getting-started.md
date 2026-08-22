# Getting started

The 5-minute path from a fresh repo clone to your first tune edit. Pairs with [`install.md`](install.md) (which focuses on definition-pack installation) and the [`docs/00-overview.md`](00-overview.md) design doc.

This guide assumes you've already cloned the repo. If not: `git clone https://github.com/BuffJesus/SubuwuTuner.git`.

## TL;DR for the impatient

```bash
cmake --preset win-mingw       # or: linux-gcc / mac-clang / win-msvc
cmake --build --preset win-mingw
./build/win-mingw/bin/subuwutuner-gui.exe
```

Click **"Try the demo project"** on the welcome panel. You're in.

## Step 1 — Build

Requires C++23 (MSVC 19.40+ / Apple Clang 16+ / GCC 14+ / MinGW-w64 15+), CMake 3.28+, Ninja, Git. No vcpkg or system packages — everything pulls via `FetchContent` on first configure.

```bash
cd SubuwuTuner
cmake --preset win-mingw       # adapt to your platform
cmake --build --preset win-mingw
```

First build downloads ~12 deps (Catch2, GLFW, ImGui, ImPlot, etc.) and takes a few minutes. Subsequent builds are incremental.

Verify the binaries:

```bash
./build/win-mingw/bin/subuwutuner-cli.exe --version
./build/win-mingw/bin/subuwutuner-gui.exe   # opens the GUI
```

If the build fails, run `subuwutuner-cli doctor` to triage. (Also reachable from the GUI's Tools menu once it's running.)

## Step 2 — Try the demo project (no hardware needed)

The repo ships `fixtures/demo-pack/` and `fixtures/demo.stune/` — a fully-synthetic ROM + definition pack you can poke around in without a real ECU dump.

In the GUI:

1. Launch `subuwutuner-gui`.
2. The welcome panel offers **"Try the demo project"** as one of three buttons. Click it.
3. The demo loads. The sidebar shows synthetic tables (Fuel Main, Ignition Main, Boost Target, etc.). Pick one.
4. The table view opens. Click any cell, type a new value, press Enter. Your edit appears in the history panel.
5. Undo/redo work (Ctrl+Z / Ctrl+Shift+Z). Save with Ctrl+S — your edits persist to disk in `fixtures/demo.stune/edits.toml`.

This is **the entire editing loop**, just with synthetic data. Everything you'll do on a real ROM works the same way.

## Step 3 — Read your ROM (eventually)

The OBDX Pro VX path (`--transport obdx --device COM5`) lets you dump your ECU's current calibration via `subuwutuner-cli rom-pull`. This is the "where do I get a ROM" answer.

> **Status (2026-06-13):** the host-side code is shipped + tested. The live-ECU validation gate is the bench rig (see `docs/28-bench-rig-build.md`). For now, ROM dumps come from other sources (your own prior dumps, public dumps for your CID, or a stock-from-disk image).

## Step 4 — Where to go from here

By workflow:

| Want to… | Read |
|---|---|
| Understand the design | [`docs/00-overview.md`](00-overview.md) → [`docs/02-architecture.md`](02-architecture.md) |
| Tune safely | [`docs/05-improvements.md`](05-improvements.md) §4 brick protection, [`docs/06-legal-ethics.md`](06-legal-ethics.md), [DISCLAIMER.md](../DISCLAIMER.md) |
| Author definition packs | [`docs/11-definition-format.md`](11-definition-format.md) + `tools/defgen/` |
| Run an FA20 → FA24 swap | Tools → Common Workflows → FA24 swap |
| Auto-tune from datalogs | [`docs/12-auto-tuning.md`](12-auto-tuning.md) |
| Author a custom feature (clutch-kill, flat-foot, etc.) | [`docs/16-custom-features.md`](16-custom-features.md) |
| Reverse-engineer CAN traffic | [`docs/14-can-reverse-engineering.md`](14-can-reverse-engineering.md) |
| Contribute | [`docs/07-build-and-tooling.md`](07-build-and-tooling.md) + [`src/README.md`](../src/README.md) + the pre-commit hook: `git config core.hooksPath .githooks` |
| Diagnose install / connection issues | `subuwutuner-cli doctor` (CLI) or Tools → Run Diagnostics (GUI, when available) |

By GUI surface:

- **F1** on any panel → context-aware help (lands on the topic most relevant to where you are).
- **Ctrl+K** → command palette. Search every action + table + recent file in one input.
- **Ctrl+1 / Ctrl+2 / Ctrl+3** → switch between Tune / Datalog / Features workspaces.
- The **status bar** (bottom) shows project save state, jurisdiction profile, and the active workspace.

By CLI surface:

```bash
subuwutuner-cli --help               # full subcommand list
subuwutuner-cli doctor               # install health check
subuwutuner-cli pack-info <DEF>      # what's in a definition pack
subuwutuner-cli project-info <DIR>   # what's in a .stune project
```

## What's NOT in scope yet

- **Flashing to a real ECU.** Host-side code shipped + tested; bench-rig validation (`docs/28`) is the live gate. The `subuwutuner-cli project-flash` command runs end-to-end against `MockTransport`; the same path against a real ECU is currently developer-only.
- **AT transmissions, STI, BRZ/86.** v1.0 targets VA + VB WRX manual. Other platforms land in v1.1+ ([`docs/04-roadmap.md`](04-roadmap.md)).
- **Auto-update.** Currently zip-and-send; see [`docs/22-auto-update.md`](22-auto-update.md) for the planned `st::updater` design.

## Common first-time questions

**"Why doesn't the GUI show any tables?"** No definition pack loaded for the current project. Demo pack ships in the repo at `fixtures/demo-pack/`. For real ROMs, point a project at a TOML pack via `project-new --def <path>`. See [`install.md`](install.md).

**"Where do my recent projects + settings live?"** Per-platform user-data dir. Windows: `%APPDATA%\SubuwuTuner\`. Linux: `~/.config/subuwutuner/`. macOS: `~/Library/Application Support/SubuwuTuner/`. Settings are TOML and human-readable; edit by hand if you must.

**"How do I undo everything I just did?"** Ctrl+Z in the GUI walks the history back one step at a time. The History panel (View → History Panel) shows the full sequence and lets you jump to any prior state. Workflow batches (e.g. FA24 swap) collapse into single "Revert all" actions via tag groups.

**"What's the deal with definition packs not shipping?"** Distribution choice, not technical limitation. See [`docs/17-data-distribution-policy.md`](17-data-distribution-policy.md). Generate your own from public ECU definition XML via `tools/defgen/`, or use the bundled demo pack to learn the tool.
