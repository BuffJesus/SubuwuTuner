# Contributing to SubuwuTuner

Thank you for your interest. SubuwuTuner is pre-1.0 and actively developed; this doc covers how to get a local build going, the conventions the code is held to, and the (important) IP rules that protect the project.

If you're just looking to use the tool, [`README.md`](README.md) + [`docs/getting-started.md`](docs/getting-started.md) are the better entry points.

## Quick start (contributor edition)

```bash
git clone https://github.com/BuffJesus/SubuwuTuner.git
cd SubuwuTuner

# Pick your preset (CMakePresets.json lists all). One-shot configure:
cmake --preset win-mingw          # or: linux-gcc / mac-clang / win-msvc

# Build everything (CLI + GUI + tests):
cmake --build --preset win-mingw

# Run the test suite (~1700 cases, takes <30s):
ctest --preset win-mingw

# Enable the pre-commit hook (recommended):
git config core.hooksPath .githooks
```

Requirements: C++23 (MSVC 19.40+ / Apple Clang 16+ / GCC 14+ / MinGW-w64 15+), CMake 3.28+, Ninja, Git. No vcpkg or system packages — everything pulls via `FetchContent` on first configure.

## Repository layout

| Path | What lives here |
|---|---|
| [`src/`](src/) | C++ source. 25 modules, one per subdirectory. See [`src/README.md`](src/README.md) for the per-module breakdown. |
| [`tests/`](tests/) | Catch2 v3 test tree. Mirrors `src/` layout. See [`tests/README.md`](tests/README.md) for tag conventions. |
| [`tools/`](tools/) | Standalone helpers — `defgen/` (Python: RomRaider XML → our TOML), `library_inventory/` (Python: local tune-library indexer), bench-rig scripts, etc. |
| [`docs/`](docs/) | Design docs (numbered 00-40+), starting with [`00-overview.md`](docs/00-overview.md). [`docs/README.md`](docs/README.md) is the index. |
| [`fixtures/`](fixtures/) | Sample data — `demo-pack/`, `demo.stune/`, sample `.stmod` graphs. Public; safe to ship. |
| [`fixtures/private/`](fixtures/private/) | Gitignored. For real ROM dumps or AP captures that have PII. See [`tests/private/README.md`](tests/private/README.md). |
| `findings/` (off-tree, `D:/Subuwu/findings/`) | Analyst-mode RE output. Not in this repo — sibling directory. |

## Code style

- **C++23 throughout.** `st::Result<T>` is portable via feature-detected fallback to `tl::expected` when `<expected>` isn't available.
- **No exceptions in domain code.** Exceptions only at UI boundaries.
- **Naming:** `snake_case` for functions/variables, `PascalCase` for types, `kPascalCase` for constants.
- **Formatting:** `clang-format` (LLVM base, 4 spaces, 100 cols, pointer-binds-right). `clang-format --dry-run --Werror` is the CI gate.
- **Warnings:** `-Wall -Wextra -Wpedantic -Werror` clean. Plus a long list (see `CMakeLists.txt`) — sign-conversion, useless-cast, double-promotion, format-2, etc.
- **Tests:** Catch2 v3, tests live next to code in `tests/unit/<module>/`. CMake glob picks up new files automatically — no manual list edits.
- **No global state.** Dependency-inject services into the application layer.
- **Domain has no ImGui or USB types in its public headers** (see [`docs/02-architecture.md`](docs/02-architecture.md)). UI implements opaque interfaces from the domain layer.

The pre-commit hook at [`.githooks/pre-commit`](.githooks/pre-commit) runs `clang-format --dry-run --Werror` on staged C/C++ files and refuses commits that would change. Opt in:

```bash
git config core.hooksPath .githooks
```

Bypass once with `git commit --no-verify` for WIP commits you intend to fix up.

## Comment style

Comments are for the **why**, not the what:

- **Don't** write a comment that paraphrases the line below it. Identifiers do that work.
- **Do** write a comment that surfaces a constraint, an invariant, a workaround for a specific bug, or behavior that would surprise a reader.
- **Don't** reference "the current task" or "the recent issue #N" — that belongs in the PR description and rots as the codebase evolves.
- **Default to no comment.** If removing the comment wouldn't confuse a future reader, leave it out.

## The IP rules (read this)

SubuwuTuner is Apache-2.0. The project's existence depends on staying clean of third-party-tool IP — both because it's the right thing and because GPL-contamination or trade-secret-tainted code would torpedo the public release.

If you're contributing protocol knowledge, calibration semantics, or RE findings, **understand the rules in [`docs/15-clean-room-engineering.md`](docs/15-clean-room-engineering.md) first**:

### General rules

- **Do not decompile** any commercial or closed-source tuning tool (COBB, EcuTek, HP Tuners, OEM tools, etc.). Do not lift icons, screenshots, distinctive UI text, or trademarks.
- **RomRaider (GPL)** is a legitimate reference for ECU protocol facts. Use it **clean-room**: study, document the protocol in plain English, write fresh C++. Do not paste or paraphrase RomRaider Java source — that would GPL-contaminate the Apache-2.0 codebase.
- **Atlas (source-available, All Rights Reserved)** is studied for concepts only — never source. Visibility on GitHub does not change the license.
- The line is **idea / expression.** A "node-graph custom feature designer" is an idea — build one freely. A specific node class hierarchy, file format, or compiler implementation copied from another tool is expression — don't.
- **Definition packs ship out-of-band** ([`docs/17-data-distribution-policy.md`](docs/17-data-distribution-policy.md)). The public repo doesn't bundle VA/VB calibration packs. Don't add them in a PR.

### Analyst / implementer wall

The project uses a clean-room methodology where one session ("analyst") extracts facts from protected references, and another session ("implementer") writes code from those extracted facts. This wall is documented in [`docs/15-clean-room-engineering.md`](docs/15-clean-room-engineering.md) and the analyst-mode kickoff prompt at [`docs/analyst-mode-prompt.md`](docs/analyst-mode-prompt.md).

If you're contributing reverse-engineering work, please:

1. **Don't include source excerpts** from any commercial tool in PRs or issues. Paraphrased descriptions of behavior are fine; literal code is not.
2. **Cite the source of each fact** in your PR description. "Per public spec X §N", "via clean-room observation of the wire protocol", etc.
3. **If you're not sure whether a contribution crosses the wall, ask first.** Open a draft PR or issue and tag the maintainer.

## Testing

Add tests next to your code:

```
src/<module>/src/foo.cpp           # your impl
tests/unit/<module>/test_foo.cpp   # your tests
```

CMake's `CONFIGURE_DEPENDS` glob picks up new test files on the next `cmake --build`. Catch2 discovers new `TEST_CASE`s on the next run.

Tag conventions (see [`tests/README.md`](tests/README.md) for the full list):

- `[module]` — the module under test (`[transport]`, `[ecu]`, `[devices][ets]`, etc.)
- `[.private]` (leading dot) — hidden from default runs; loads files from `fixtures/private/`
- `[.live]` (leading dot) — hidden from default runs; talks to real hardware; gated by env var
- ASCII-only `TEST_CASE` names on Windows + MinGW (cp1252 / UTF-8 discovery issue documented in `tests/README.md`)

## Pull request flow

1. **Fork + branch.** No direct pushes to `main`.
2. **One topic per PR.** "Refactor X" and "add feature Y" don't co-exist.
3. **Tests pass locally.** `ctest --preset <your-preset>` green.
4. **Pre-commit hook clean.** No clang-format diffs.
5. **Description explains the why.** What user-visible behavior changes, what the trade-offs were, what alternatives you considered.
6. **Link the doc.** If your PR touches behavior described in a `docs/` page, link the doc and note whether the doc needed updating.

The PR template ([.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md)) walks through this.

## Reporting bugs

- **Crash / data corruption / safety regression:** see [`SECURITY.md`](SECURITY.md). Do not file a public issue for these.
- **Everything else:** GitHub issue tracker. Use the [bug-report template](.github/ISSUE_TEMPLATE/bug.md) — it asks for the right info.
- **Feature requests:** [feature-request template](.github/ISSUE_TEMPLATE/feature.md). State the user-visible benefit, not the implementation.

## Communication

- Issues + PRs on GitHub.
- The repo is a solo-developer project today; expect async response, sometimes days.
- For RE / definition-pack contributions, the analyst-mode pattern works — open an issue describing the protocol fact or table semantic you want to add, the maintainer responds with whether/where it fits.

## What's NOT a good contribution path right now

- **Adding definition packs to the repo.** Path B distribution ([`docs/17`](docs/17-data-distribution-policy.md)) keeps them out by design.
- **Adding "tune presets" or "stage tunes".** SubuwuTuner is infrastructure; tune content is user-supplied.
- **Decompiling someone's tool to add interop.** Even if the interop would be useful, the contamination cost is too high.
- **Refactoring `src/flash/` without a strong reason.** Safety-critical code; changes need bench-rig validation we don't have today.

## License

By contributing, you agree your contribution is licensed under the project's Apache-2.0 license. See [`LICENSE`](LICENSE).
