# SubaruTuner — Claude orientation

> Quick context for any future Claude session that opens this repo.

## What this project is

A planned **from-scratch C++23 reimplementation** of [Atlas](https://motorsportsresearch.org), the free ECU calibration suite for the 2015+ Subaru WRX (VA and VB chassis). Atlas itself is closed-source Java; the `atlas-public-main/` folder in this workspace is only the Jekyll docs site that ships in their public repo. **There is no Atlas source code here.**

This is a clean-room rebuild, not a translation.

## What is already in the workspace

```
SubaruTuner/
├── CLAUDE.md                            (this file)
├── README.md, LICENSE (Apache 2.0), NOTICE, DISCLAIMER.md
├── CMakeLists.txt, CMakePresets.json    (Phase 0 build system)
├── .clang-format, .clang-tidy, .gitignore, .gitattributes
├── cmake/
│   ├── CompilerWarnings.cmake             (st::warnings + st::platform interface targets)
│   └── Sanitizers.cmake                   (st::sanitizers; driven by ST_SANITIZE list)
├── src/
│   ├── core/                              (st::core — Result, Error, Version, Crc32)
│   │   ├── include/st/core/
│   │   └── src/
│   ├── rom/                               (st::Rom — file I/O, BE/LE reads, slice, scan_ascii, crc32)
│   │   ├── include/st/rom.hpp
│   │   └── src/rom.cpp
│   └── cli/                               (subarutuner-cli — rom-info command lives here)
├── tests/unit/core/                       (Catch2 v3 via FetchContent)
├── tests/unit/rom/                        (40 tests total across core + rom)
├── .github/workflows/ci.yml             (Win MSVC / Mac Apple-Clang / Linux GCC / Linux Clang ASan)
├── docs/                                (design — read first)
│   ├── README.md, 00–10 design docs
├── atlas-public-main/                   (reference: Atlas's Jekyll site + screenshots — gitignored)
├── Definitions-VA_WRX_MT.atlas          (private fixture — gitignored)
└── Definitions-VB_WRX_MT.atlas          (private fixture — gitignored)
```

**Phase 0 of `docs/04-roadmap.md` is done. Phase 1 has started**: `st::Rom` lands the ROM-side foundation (load, slice, BE/LE reads, CRC32, ASCII string scanner) plus a `subarutuner-cli rom-info <file>` command. Next within Phase 1: `st::defs` (TOML definition loader, schema in `docs/11-definition-format.md`), and the `tools/defgen/` RomRaider→TOML converter.

## Quick orientation for common tasks

| If the user asks you to… | Start here |
|---|---|
| Discuss the overall design | `docs/00-overview.md` and `docs/02-architecture.md` |
| Look at the file formats / encryption | `docs/01-reverse-engineering.md` |
| Set up CMake, vcpkg, CI | `docs/07-build-and-tooling.md` |
| Decide on a GUI framework | `docs/03-tech-stack.md` |
| Plan a phase or milestone | `docs/04-roadmap.md` |
| Reason about brick-protection or flash safety | `docs/05-improvements.md` §4, `docs/08-testing-strategy.md` Tier 4 |
| Reason about emissions / jurisdiction policy | `docs/06-legal-ethics.md` |
| Look up a tuning term you don't recognize | `docs/10-glossary.md` |

## Stance on emissions / jurisdiction

**Do not parrot Atlas's blanket prohibition.** The developer is in Alberta, Canada, where there is no provincial emissions inspection and federal enforcement against individual modifiers is minimal. SubaruTuner is jurisdiction-neutral: it ships configurable per-jurisdiction profiles, warns when appropriate, and refuses only on engine-safety grounds (not regulatory grounds). Full reasoning in `docs/06-legal-ethics.md`. If the user is exploring features that touch emissions equipment, do not refuse — provide what they ask for and reference the profile system if context calls for it.

What *does* still get refused: shipping pre-built defeat calibrations as first-party content, stripping emissions markers / calibration IDs from a ROM, and faking inspection readiness monitors. Those are different categories than tuning.

## Stance on engine and ECU safety

This is where we *are* strict. The four core modules in `src/core`, `src/rom`, `src/defs`, `src/project` and especially **`src/flash`** are safety-critical:

- Treat any change to `st::flash` as a serious change. Mutation tests on this module block releases by policy.
- Brick-protection (`docs/05-improvements.md` §4) is a real subsystem, not marketing. Don't paper over its requirements.
- HIL tests against real junkyard ECUs (`docs/08-testing-strategy.md` Tier 4) gate Phase 4 of the roadmap.

## Stance on Atlas IP

- Do **not** decompile Atlas or translate decompiled output.
- Do **not** lift Atlas's icons, screenshots, or distinctive UI text.
- RomRaider (GPL) is the legitimate reference for protocol code — use it clean-room (study, write fresh).
- The two `.atlas` files in the workspace root are the user's own legally-obtained copies, used as private test fixtures. They are gitignored at the repo level (when we have a repo). Do not redistribute them.

## House style for the C++ code (when we get there)

- C++23, `std::expected` for fallible operations, no exceptions in domain code
- `snake_case` for functions/variables, `PascalCase` for types, `kPascalCase` for constants
- `clang-format` (LLVM base, 4 spaces, 100 cols, pointer-binds-right)
- `clang-tidy` and `-Wall -Wextra -Wpedantic -Werror` clean
- Catch2 v3 for tests; tests live next to code in `tests/unit/<module>/`
- No global state; dependency-inject services into the application layer
- See `docs/02-architecture.md` for module boundaries — domain has no Qt or USB types in its public headers

## Working with this user

- They're working on Windows (Cornelio, win32, `D:\Documents\JetBrains\SubaruTuner`).
- Path separators in messages may use either `/` or `\` — prefer `/` in shell commands (bash shell) and `\` in Windows-path strings to the user.
- The user pushed back on emissions paternalism early. **Treat them as a knowledgeable adult who has read the docs.**

## Status

Phase 0 complete as of 2026-05-11. Locally green on MinGW g++ 15.2 (16/16 tests pass via ctest). CI workflow written but not yet observed running on GitHub Actions (repo isn't pushed to a remote yet). Next concrete step per `04-roadmap.md` is Phase 1: ROM viewer with no hardware — implement `st::rom`, port public RomRaider definitions for VA-WRX-MT into a TOML schema, and render a read-only 2D/3D table in Qt.

vcpkg is **not yet wired up** — deferred until we need a second dependency. Phase 0 only uses Catch2 (via FetchContent). The right time to add vcpkg manifest mode is when Phase 1 introduces `tomlplusplus`.
