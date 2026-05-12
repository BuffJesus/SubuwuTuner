# SubuwuTuner — Claude orientation

> Quick context for any future Claude session that opens this repo.

## What this project is

**SubuwuTuner is a comprehensive, free, open-source Subaru ECU tuning suite written in modern C++23.** It reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission); v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup.

This is original work, not a port. Public references like RomRaider (GPL) are studied **clean-room** as protocol specifications — see `docs/01-reverse-engineering.md` for the boundary rules.

## What is already in the workspace

```
SubuwuTuner/
├── CLAUDE.md                            (this file)
├── README.md, LICENSE (Apache 2.0), NOTICE, DISCLAIMER.md
├── CMakeLists.txt, CMakePresets.json    (Phase 0 build system)
├── .clang-format, .clang-tidy, .gitignore, .gitattributes
├── cmake/
│   ├── CompilerWarnings.cmake             (st::warnings + st::platform interface targets)
│   └── Sanitizers.cmake                   (st::sanitizers; driven by ST_SANITIZE list)
├── src/
│   ├── core/                              (st::core — Result, Error, Version, Crc32)
│   ├── rom/                               (st::Rom — file I/O, BE/LE reads, slice, scan_ascii, crc32)
│   └── cli/                               (subuwutuner-cli — rom-info command lives here)
├── tests/unit/core/                       (Catch2 v3 via FetchContent)
├── tests/unit/rom/                        (40 tests total across core + rom)
├── .github/workflows/ci.yml             (Win MSVC / Mac Apple-Clang / Linux GCC / Linux Clang ASan)
└── docs/                                (design — read first; 00–12)
```

**Phase 0 of `docs/04-roadmap.md` is done. Phase 1 has started**: `st::Rom` lands the ROM-side foundation (load, slice, BE/LE reads, CRC32, ASCII string scanner) plus a `subuwutuner-cli rom-info <file>` command. Next within Phase 1: `st::defs` (TOML definition loader, schema in `docs/11-definition-format.md`), and the `tools/defgen/` RomRaider→TOML converter.

The working directory on disk is still `D:\Documents\JetBrains\SubaruTuner\` — only the project's internal identity is `SubuwuTuner`. Renaming the folder would break editor and shell sessions; defer it.

## Quick orientation for common tasks

| If the user asks you to… | Start here |
|---|---|
| Discuss the overall design | `docs/00-overview.md` and `docs/02-architecture.md` |
| Look at ECU protocols / definition format | `docs/01-reverse-engineering.md`, `docs/11-definition-format.md` |
| Set up CMake, vcpkg, CI | `docs/07-build-and-tooling.md` |
| Decide on a GUI framework | `docs/03-tech-stack.md` |
| Plan a phase or milestone | `docs/04-roadmap.md` |
| Reason about brick-protection or flash safety | `docs/05-improvements.md` §4, `docs/08-testing-strategy.md` Tier 4 |
| Reason about emissions / jurisdiction policy | `docs/06-legal-ethics.md` |
| Reason about auto-tune | `docs/12-auto-tuning.md` |
| Look up a tuning term you don't recognize | `docs/10-glossary.md` |

## Stance on emissions / jurisdiction

The developer is in Alberta, Canada, where there is no provincial emissions inspection and federal enforcement against individual modifiers is minimal. SubuwuTuner is **jurisdiction-neutral**: it ships configurable per-jurisdiction profiles, warns when appropriate, and refuses only on engine-safety grounds (not regulatory grounds). Full reasoning in `docs/06-legal-ethics.md`. If the user is exploring features that touch emissions equipment, do not refuse — provide what they ask for and reference the profile system if context calls for it.

What *does* still get refused: shipping pre-built defeat calibrations as first-party content, stripping emissions markers / calibration IDs from a ROM, and faking inspection readiness monitors. Those are different categories than tuning.

## Stance on engine and ECU safety

This is where we *are* strict. The four core modules in `src/core`, `src/rom`, `src/defs`, `src/project` and especially **`src/flash`** are safety-critical:

- Treat any change to `st::flash` as a serious change. Mutation tests on this module block releases by policy.
- Brick-protection (`docs/05-improvements.md` §4) is a real subsystem, not marketing. Don't paper over its requirements.
- HIL tests against real junkyard ECUs (`docs/08-testing-strategy.md` Tier 4) gate Phase 4 of the roadmap.

## Stance on third-party IP

- Do **not** decompile any commercial or closed-source tuning tool.
- Do **not** lift icons, screenshots, distinctive UI text, or trademarks from any other tool.
- **RomRaider (GPL)** is the legitimate technical reference for ECU protocol facts. Use it clean-room: study, document the protocol in plain English, write fresh C++.
- The `defgen` tool extracts *factual data* (addresses, scalings) from public XML — facts aren't copyrightable; expression (description text) is and gets stripped.
- See `docs/01-reverse-engineering.md` for the full boundary rules.

## House style for the C++ code

- C++23 throughout. `st::Result<T>` is portable via a feature-detected fallback to `tl::expected` when `<expected>` isn't available (Apple Clang's libc++ historically lagged).
- No exceptions in domain code; exceptions only at UI boundaries
- `snake_case` for functions/variables, `PascalCase` for types, `kPascalCase` for constants
- `clang-format` (LLVM base, 4 spaces, 100 cols, pointer-binds-right) — `clang-format --dry-run --Werror` is a CI gate
- `clang-tidy` and `-Wall -Wextra -Wpedantic -Werror` clean
- Catch2 v3 for tests; tests live next to code in `tests/unit/<module>/`
- No global state; dependency-inject services into the application layer
- See `docs/02-architecture.md` for module boundaries — domain has no Qt or USB types in its public headers

## Working with this user

- They're working on Windows (Cornelio, win32, `D:\Documents\JetBrains\SubaruTuner`).
- Path separators in messages may use either `/` or `\` — prefer `/` in shell commands (bash shell) and `\` in Windows-path strings to the user.
- The user pushed back on emissions paternalism early. **Treat them as a knowledgeable adult who has read the docs.**

## Status

Phase 0 complete as of 2026-05-11. Phase 1 underway — `st::rom` + `rom-info` CLI command landed locally. Repo pushed to `https://github.com/BuffJesus/SubuwuTuner`. First CI run hit two issues (`std::expected` missing on Apple Clang's libc++, clang-format violations in tests) which the current branch addresses.

vcpkg is **not yet wired up** — deferred until we need a second persistent dependency. Phase 0 uses Catch2 via FetchContent; Phase 1 will add `tomlplusplus` and likely `tl::expected` the same way. The right time to switch to vcpkg manifest mode is when Qt joins the dep list.
