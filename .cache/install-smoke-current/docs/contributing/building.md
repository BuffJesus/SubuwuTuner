# Building from source

The full build story. For a quickstart, [Install](../getting-started/installation.md)
is the 5-minute path; this page covers what changes when you're working
on the codebase.

## Prerequisites

| Tool | Minimum version |
|------|------|
| C++23 compiler | MSVC 19.40+ / GCC 14+ / Apple Clang 16+ / MinGW-w64 15+ |
| CMake | 3.28+ |
| Ninja | latest |
| Git | latest |
| Python (defgen + docs) | 3.10+ |

No vcpkg or system package manager required. Every C++ dependency
pulls via `FetchContent`.

## Presets

The repo ships `CMakePresets.json` with four presets:

```bash
cmake --preset win-msvc
cmake --preset win-mingw
cmake --preset linux-gcc
cmake --preset mac-clang
```

Build the matching preset:

```bash
cmake --build --preset win-mingw
```

Per-preset binaries land under `build/<preset>/bin/`.

## CMake options

| Option | Default | Purpose |
|---|---|---|
| `ST_ENABLE_GUI` | `ON` | Build `subuwutuner-gui` |
| `ST_ENABLE_TESTS` | `ON` | Build Catch2 unit tests |
| `ST_ENABLE_COBB_AP_WORKFLOW` | `OFF` | Master switch for the full AP file-vault workflow (browser panel, push/pull, libusb device enumeration) |
| `ST_ENABLE_COBB_AP_CIPHER` | implied by workflow flag | `.ptm` cipher chain read path |
| `ST_ENABLE_COBB_AP_PTM_REWRITE` | implied | `.ptm` cipher write path (asymmetric default-OFF) |
| `ST_ENABLE_AP3` | `OFF` | libusb pull-in for AP3 device enumeration |
| `ST_ENABLE_BULK_REFLASH_CIPHER` | `OFF` | Arm the gated 0xB6 bulk-transfer write path |

Cornelio's local build is `-DST_ENABLE_COBB_AP_WORKFLOW=ON` (which
force-enables `ST_ENABLE_COBB_AP_CIPHER`, `ST_ENABLE_COBB_AP_PTM_REWRITE`,
and `ST_ENABLE_AP3` and propagates `ST_HAVE_AP_WORKFLOW=1` as a global
compile def).

## Running tests

```bash
# All unit tests
ctest --preset win-mingw

# A specific tag
./build/win-mingw/bin/st_unit_tests "[edit]"

# defgen Python tests
cd tools/defgen && python -m pytest
```

!!! note "CTest + Unicode on Windows"
    Test cases with `→` or other non-ASCII characters in the name
    can show as false-failed via `ctest` on Windows due to a code-page
    issue. If something looks broken, run the Catch2 binary directly to
    verify before blaming a regression.

## Pre-commit hook

The repo ships an opt-in pre-commit hook at `.githooks/pre-commit` that
runs `clang-format --dry-run --Werror`:

```bash
git config core.hooksPath .githooks
```

CI's `clang-format` job is advisory today; it flips to required once
contributors are reliably running the hook.

## Building the docs site locally

```bash
pip install -r requirements-docs.txt
mkdocs serve            # http://127.0.0.1:8000
mkdocs build --strict   # production build, fails on broken links
```

CI publishes to GitHub Pages on every push to `main` that touches
`docs/`, `mkdocs.yml`, or `requirements-docs.txt`. Workflow at
[`.github/workflows/docs.yml`](https://github.com/BuffJesus/SubuwuTuner/blob/main/.github/workflows/docs.yml){ target="_blank" }.

## CI matrix

| Job | Runner |
|---|---|
| Windows MSVC | windows-latest |
| macOS Apple Clang | macos-latest |
| Linux GCC | ubuntu-latest |
| Linux Clang + ASan + UBSan | ubuntu-latest |
| defgen Python tests | ubuntu-latest |
| defgen PyInstaller freeze (per OS) | matrix |
| Docs build + deploy | ubuntu-latest |

## Deeper detail

- [`docs/07-build-and-tooling.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/07-build-and-tooling.md){ target="_blank" }
  — full build + CI design.
- [`docs/08-testing-strategy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/08-testing-strategy.md){ target="_blank" }
  — unit / HIL / fuzz testing layers.
- [`CONTRIBUTING.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/CONTRIBUTING.md){ target="_blank" }
  — code-of-conduct, PR process, sign-off.
