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
│   ├── rom/                               (st::Rom — file I/O, BE/LE reads + writes, slice, scan_ascii, crc32)
│   ├── defs/                              (st::Definition — TOML single-file + directory loader, typed reads + writes, scaling + inverse, axis/table values, diff, writeback)
│   ├── edit/                              (st::edit — Rect, set/add/multiply/percent/smooth/interpolate, Snapshot, History undo/redo)
│   └── cli/                               (subuwutuner-cli — rom-info, dump-axis, dump-table, rom-diff, table-edit)
├── tools/defgen/                          (Python: RomRaider XML -> our TOML; clean-room facts-only)
├── tests/unit/{core,rom,defs,edit}/       (95 C++ tests; 28 Python tests under tools/defgen/tests/)
├── .github/workflows/ci.yml             (Win MSVC / Mac Apple-Clang / Linux GCC / Linux Clang ASan)
└── docs/                                (design — read first; 00–12)
```

**Phase 0 done. Phase 1 CLI side done. Phase 2 read→edit→save loop done.** What's shipped:

- `st::Rom` — file I/O, big/little-endian typed reads + writes with overflow-safe bounds, slice, ASCII scanner, CRC32, mutable data span
- `st::Definition` — TOML single-file + directory loader (tomlplusplus), cross-reference validation, CID matching, typed value reads + writes, linear + piecewise scaling (both directions), axis-value extraction, table-value extraction (1D + 2D), per-table diff, writeback
- `st::edit` — Rect-scoped cell operations (set/add/multiply/percent/smooth/interpolate), Snapshot, History stack with branching-undo semantics
- `tools/defgen/` — Python tool that translates public RomRaider XML to our TOML schema; clean-room rules in `docs/01-reverse-engineering.md`. Standard-library Python only.
- CLI: `rom-info`, `dump-axis`, `dump-table`, `rom-diff`, `table-edit` — all working with single-file or directory `.toml` packs.

The read→edit→save loop is end-to-end exercisable without hardware:

```
$ subuwutuner-cli table-edit --def pack.toml --table fuel_map \
    --rows 0:0 --cols 2:3 set 12.5 stock.bin --output tuned.bin
$ subuwutuner-cli rom-diff --def pack.toml stock.bin tuned.bin
$ subuwutuner-cli dump-table --def pack.toml --table fuel_map tuned.bin
```

What remains for the full Phase 1 ship gate: a real RomRaider XML through `defgen`, verified against a real stock dump showing ≥ 20 factory maps with correct scaling. That's a hardware/data gate; user is waiting on the OBDX Pro VX adapter to land before they can dump their own car. **Until then, do not block work on it** — there's still Phase 2 work (project files, 3D table support, definition inheritance) and Phase 3 design that's hardware-free.

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

As of 2026-05-11: Phase 0 done. Phase 1 CLI side done. Phase 2 read→edit→save loop done. **95 C++ + 28 Python tests** green on MinGW g++ 15.2. Repo at `https://github.com/BuffJesus/SubuwuTuner`. Phase 1 hardware gate (real ROM, ≥20 maps from real definitions) waiting on user's OBDX Pro VX adapter.

Deps wired so far via FetchContent: Catch2 v3 (tests), `tl::expected` (fallback when libc++ lacks `<expected>`), tomlplusplus v3.4 (definition parser). vcpkg manifest mode still deferred — natural moment is when Qt joins the dep list for Phase 2's UI.

CI: clang-format job is advisory (non-blocking) since no pre-commit hook is set up yet. Once one is wired in, flip it back to required.

**Hardware-free work still on the table** (any of these can be picked up next):
- `.stune` project files — directory-based persistence wrapping source ROM + working ROM + def pack reference
- 3D table support — extends TableData with axis_z and a vector of slices
- Definition inheritance (`extends`) — cross-pack inheritance for shared bases
- CLI convenience: `pack-info`, `table-list`, `--csv` mode on `dump-table`
- defgen polish: handle RomRaider `<base>` inheritance, better non-linear formula reporting
- Phase 3 design: J2534 transport abstraction shape (can be designed without hardware)
