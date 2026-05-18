# 07 — Build & Tooling

Snapshot of what's actually on disk and in CI right now. Aspirational items (release packaging, code signing, docs site) live at the end clearly tagged as future work.

## Repo layout

```
SubuwuTuner/
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── .clang-tidy
├── docs/                              (this folder)
├── src/
│   ├── core/                          Result<T>, Error, version
│   ├── rom/                           Rom, CRC32
│   ├── defs/                          Definition, Pack, Table, Axis, Scaling, Pid, Hook
│   ├── edit/                          History, Edit, TableEdit, ByteEdit
│   ├── project/                       Project (.stune)
│   ├── transport/                     ITransport, MockTransport, IByteChannel, j2534/, obdx/, native/, factory
│   ├── ecu/                           ecu/ssm — SsmClient; ecu/uds — UdsClient
│   ├── flash/                         Flasher, FlashPlan, IChecksumRepair
│   ├── log/                           LogStream (SPSC), LogSession (I/O thread), CsvSink
│   ├── can/                           Frame, .asc reader/writer
│   ├── dbc/                           Database, Message, Signal — parser + emitter + decoder
│   ├── discover/                      BaselineModel, ChangeDetector, .cdb bundle
│   ├── autotune/                      MAF + knock-pull kernels (docs/12)
│   ├── policy/                        Jurisdiction-profile lint + flash gate (docs/06)
│   ├── feature/                       Graph, Node, Edge — designer canvas source + feature::ir (typed dataflow IR)
│   ├── feature_codegen/               IBackend, Sh2aBackend, Rh850Backend, RamAllocator, PatchObject
│   ├── ui/                            subuwutuner-gui — Dear ImGui + GLFW + ImPlot
│   └── cli/                           subuwutuner-cli — argparse + same domain
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/<module>/                 Catch2 v3, one dir per src/ module
│   └── private/                       gitignored — analyst-mode regression fixtures (only when present)
├── fixtures/
│   ├── demo-pack/                     synthetic VA WRX MT pack used by tests + CLI smoke
│   ├── demo.stune/                    one-project .stune example
│   ├── demo-trace.hex                 demo UDS trace
│   ├── samples/                       sample `.stmod` graphs
│   ├── private/                       gitignored — user-supplied real packs/ROMs (Path B, docs/17)
│   ├── README.md
│   └── IDE-RUN-CONFIGS.md
├── tools/
│   └── defgen/                        RomRaider XML → SubuwuTuner TOML (Python 3.12+; 88 unit tests)
└── .github/workflows/
    └── ci.yml                         single CI workflow (build matrix + defgen tools + advisory clang-format)
```

No `vcpkg.json` / `vcpkg-configuration.json` in the repo today — vcpkg manifest mode is deferred until a dep brings system-package complexity (see `docs/03-tech-stack.md`). No `third_party/` directory — every C++ dep is fetched via `FetchContent`.

## CMake presets

`CMakePresets.json` ships with one hidden `base` (Ninja, Release, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, `binaryDir = ${sourceDir}/build/${presetName}`) and the following inheritors:

| Preset | Host | Compiler |
|---|---|---|
| `win-msvc` | Windows | `cl` / `cl` |
| `win-mingw` | Windows | `gcc` / `g++` (MinGW-w64) |
| `win-clang` | Windows | `clang-cl` / `clang-cl` |
| `mac-clang` | macOS | Apple Clang (host default) |
| `linux-gcc` | Linux | `gcc` / `g++` |
| `linux-asan` | Linux | `clang-18` / `clang++-18` with ASan + UBSan |

Each preset has a matching build preset + test preset under the same name so `cmake --preset <name> && cmake --build --preset <name> && ctest --preset <name>` works uniformly across the matrix.

`win-mingw` is the dev machine's primary local target (MinGW-w64 g++ 15.2) and not part of CI; the in-repo build at `build/win-mingw/` is what the developer iterates against. CI covers MSVC instead.

## CI

A single workflow at `.github/workflows/ci.yml` with two top-level jobs:

| Job | Matrix entry | Notes |
|---|---|---|
| build | `win-msvc` on `windows-2022` | MSVC env via `microsoft/setup-msbuild` action |
| build | `mac-clang` on `macos-14` | Apple Silicon |
| build | `linux-gcc` on `ubuntu-24.04` | |
| build | `linux-asan` on `ubuntu-24.04` | Clang 18 + ASan + UBSan |
| defgen | `ubuntu-24.04` | Python 3.12, runs `tools/defgen/` pytest suite |
| clang-format | `ubuntu-24.04`, **advisory** | clang-format 18 `--dry-run --Werror` on changed files; not a merge gate yet |

Per matrix entry: configure → build → ctest → smoke-test (`subuwutuner-cli --version` + `--help`) → upload test logs + CMake error logs on failure.

The `clang-format` job is intentionally non-blocking until a pre-commit hook is in place to keep submitters out of CI loops. The handoff is to flip it to required after the hook lands.

No `cppcheck`, no `clang-tidy`-in-CI, no `fuzz` job today — the configs (`.clang-tidy`, `.clang-format`) are checked in for local + editor use, and the warnings policy below leans on the compilers themselves.

## Warnings + sanitizers

- `-Wall -Wextra -Wpedantic -Werror` on GCC + Clang (set globally in the root `CMakeLists.txt`).
- MSVC `/W4 /WX`.
- `linux-asan` preset enables AddressSanitizer + UndefinedBehaviorSanitizer.
- Project compiles cleanly on MinGW g++ 15.2 (dev machine) + every CI compiler.

## Static analysis

- `.clang-tidy` lives at the repo root with the project's check selection; runs on demand locally + via IDE integrations (CLion / VS Code). Not currently run in CI.
- `.clang-format` is the LLVM base with 4-space indent, 100-col line limit, pointer-binds-right. `clang-format --dry-run --Werror` is the CI check (advisory).

## Versioning

SemVer `MAJOR.MINOR.PATCH`. The version is declared in the root `CMakeLists.txt` via `project(SubuwuTuner VERSION X.Y.Z)`; `src/core/include/st/core/version.hpp.in` is templated into a generated `version.hpp` under the build tree so `st::core::version()` returns the same string the build was tagged with.

Pre-1.0 releases will be tagged `0.x.y`. v0.1.0 is the current floor.

## Future (not in CI today)

- **Release artifacts.** No `.exe` / `.msix` / `.dmg` / `.AppImage` / `.deb` packaging exists yet; the CI smoke-tests run the binary but don't archive it. Lands with a `release.yml` workflow when there's a v0.1 release to cut.
- **Code signing.** Windows EV (or sigstore stop-gap), Apple Developer ID + notarization, GPG-signed `.deb` — all deferred to release time.
- **Static-analysis-in-CI.** clang-tidy could be added as another advisory job once its findings count stabilizes.
- **Docs site.** Doxygen + MkDocs — useful once the API surface is more public-facing; today the in-repo `docs/` is read directly from GitHub.
- **Fuzz job.** ROM/TOML loaders are the natural targets; corpus seeding waits for the bench rig.
