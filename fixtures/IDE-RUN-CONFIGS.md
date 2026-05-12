# IDE Run Configurations

Seven run configs ship in `.idea/runConfigurations/`. All are
**`ShConfigurationType`** (shell-script type) — a JetBrains-built-in
that's recognized in every IDE in the family (CLion, Rider, IntelliJ,
PyCharm, etc.) without needing a C++ plugin to interpret CMake target
configurations.

Each script does **build-then-run** in one line so you don't have to
remember to build first:

| Name | What it runs |
|---|---|
| **GUI (demo)** | builds `subuwutuner-gui`, launches against `fixtures/demo.stune` |
| **GUI (no args)** | builds `subuwutuner-gui`, launches with no args |
| **CLI rom-info (demo)** | builds `subuwutuner-cli`, runs `rom-info` on the demo |
| **CLI table-list (demo)** | builds, runs `table-list fixtures/demo-pack` |
| **CLI dump-table fuel (demo)** | builds, renders the fuel map |
| **CLI --help** | builds, prints the full CLI help |
| **Tests (ctest)** | builds everything, runs `ctest --preset win-mingw` |

## Prerequisites

These configs assume:

1. **`cmake` is on your `PATH`.** Verify in a terminal: `cmake --version`.
2. **The `win-mingw` CMake preset exists** (it does — shipped in
   `CMakePresets.json`).
3. **MinGW-w64 g++ is on PATH** so the build can resolve the compiler. If you
   prefer MSVC, swap `win-mingw` for `win-msvc` in each script's
   `SCRIPT_TEXT` (one-line edit per config).

## If a script doesn't work inside the IDE

The script body is just shell. Paste it into a terminal:

```bash
cmake --build --preset win-mingw -j --target subuwutuner-gui && \
  ./build/win-mingw/bin/subuwutuner-gui.exe fixtures/demo.stune
```

If that errors, the IDE will error the same way. Common fixes:

- **`cmake: command not found`** → install CMake or add to PATH
- **`g++.exe: not found`** → no MinGW; install MinGW-w64 (UCRT preferred) or
  pick a different preset
- **`No such file or directory: fixtures/demo.stune`** → `git checkout
  fixtures/demo.stune/` to restore the committed fixture, or run
  `python scripts/gen-demo-fixture.py` + `subuwutuner-cli project-new` to
  regenerate

## Why shell instead of native CMake run configs?

An earlier iteration used `type="CMakeRunConfiguration"`, which is
CLion-native. Rider's C++ plugin doesn't fully recognize it — the configs
showed up in the UI but with a red X on the icon and a greyed-out Run button.
The shell variant avoids the whole "what does this IDE think CMake configs
look like?" problem by invoking the build system directly.

The cost is no IDE-managed "Before Run" tasks, but since the script already
contains a build step, that doesn't matter. The benefit is universal
compatibility across the JetBrains IDE family without per-IDE quirks.
