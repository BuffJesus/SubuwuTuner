# IDE Run Configurations — troubleshooting

The committed configs in `.idea/runConfigurations/` come in two flavors:

## CMake-based configs (preferred)

`GUI (demo)`, `CLI rom-info (demo)`, etc. — `type="CMakeRunConfiguration"`.

These resolve the binary path from the CMake target (`subuwutuner-gui` /
`subuwutuner-cli`) and auto-rebuild before running. They require:

1. **CMake project loaded in the IDE.** File → Reload CMake Project
   (CLion) or "Sync CMake Project" (Rider). If your CMake panel is empty,
   load it first.
2. **A CMake profile selected** (any name — Debug / Release /
   `win-mingw` / whatever you've configured). The configs no longer pin
   a specific profile name, so any active profile should work.
3. **The targets visible.** After CMake reload, `subuwutuner-gui` and
   `subuwutuner-cli` should appear in the target dropdown.

## Shell-based fallbacks

`GUI (demo, shell)`, `CLI rom-info (demo, shell)` — `type="ShConfigurationType"`.

These run a hardcoded shell command — no CMake awareness required. They
**won't auto-build**; you have to `cmake --build --preset win-mingw` (or
your IDE's build button) before running. The script path assumes the
binary lives at `build/win-mingw/bin/subuwutuner-{cli,gui}.exe`. If your
build directory is somewhere else (e.g. `cmake-build-release/`), edit
the `SCRIPT_TEXT` in the XML.

## Common gotchas

**Run button greyed out** → the IDE doesn't know about the target.
Reload CMake; check that `subuwutuner-gui` appears under your active
CMake profile.

**"Cannot run program" error** → the binary path doesn't resolve.
For the shell fallbacks, build first. For the CMake variants, verify
the target builds successfully (View → Tool Windows → CMake).

**Configs change between sessions** → the IDE rewrites them on save.
If you tweak a config, the IDE may add/remove fields; that's normal.
Just commit the result if you want to share it.

**Wrong working directory** → all configs set `WORKING_DIR=$PROJECT_DIR$`
so `fixtures/demo.stune` resolves relative to the repo root. If you
moved the project, IDE may default to the build dir instead; check the
"Working directory" field.

If a config still won't run after these checks, drop a screenshot of
the error somewhere and I'll iterate. Rider's C++ support occasionally
diverges from CLion's in subtle ways — the shell fallbacks are the
guaranteed escape hatch.
