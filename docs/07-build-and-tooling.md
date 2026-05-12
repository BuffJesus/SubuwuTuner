# 07 — Build & Tooling

## Repo layout

```
SubaruTuner/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── vcpkg-configuration.json
├── docs/                       (this folder)
├── src/
│   ├── core/
│   ├── rom/
│   ├── defs/
│   ├── project/
│   ├── transport/
│   ├── ecu/
│   ├── flash/
│   ├── log/
│   ├── script/
│   ├── nodegraph/
│   ├── ui/                     (Qt app)
│   └── cli/                    (headless tool)
├── tests/
│   ├── unit/
│   ├── integration/            (uses fixture ROMs)
│   └── hil/                    (hardware-in-the-loop, opt-in)
├── fixtures/
│   ├── roms/                   (legally-obtained stock dumps, gitignored)
│   └── definitions/
├── third_party/                (only vendored bits vcpkg can't supply)
├── tools/
│   ├── defgen/                 (RomRaider XML → our TOML)
│   └── canlogview/
└── .github/workflows/
    ├── ci-windows.yml
    ├── ci-macos.yml
    ├── ci-linux.yml
    └── release.yml
```

## CMake presets

```jsonc
{
  "version": 6,
  "configurePresets": [
    { "name": "win-msvc",   "generator": "Ninja", "binaryDir": "build/win-msvc",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } },
    { "name": "win-clang",   "inherits": "win-msvc", "binaryDir": "build/win-clang",
      "cacheVariables": { "CMAKE_C_COMPILER": "clang-cl", "CMAKE_CXX_COMPILER": "clang-cl" } },
    { "name": "mac-clang",   "inherits": "win-msvc", "binaryDir": "build/mac" },
    { "name": "linux-gcc",   "inherits": "win-msvc", "binaryDir": "build/linux-gcc" },
    { "name": "linux-asan",  "inherits": "linux-gcc","binaryDir": "build/linux-asan",
      "cacheVariables": { "ST_SANITIZE": "address;undefined" } }
  ]
}
```

## CI matrix

| Job | OS | Compiler | Mode | Artifacts |
|---|---|---|---|---|
| `win-release` | windows-2022 | MSVC | Release | `.exe`, `.msix` |
| `win-asan` | windows-2022 | clang-cl | ASan | none |
| `mac-release` | macos-14 | Apple Clang | Release universal | `.dmg` notarized |
| `linux-release` | ubuntu-24.04 | GCC | Release | `.AppImage`, `.deb` |
| `linux-asan` | ubuntu-24.04 | clang-18 | ASan+UBSan | none |
| `fuzz` | ubuntu-24.04 | clang-18 | libFuzzer | corpus updates |

A PR is mergeable only when **win-release, mac-release, linux-release, linux-asan, and fuzz (15 min)** are all green.

## Static analysis

- `clang-tidy` with our config in `.clang-tidy` — runs in CI as advisory at first, blocking after Phase 1
- `cppcheck --enable=warning,performance,portability` — blocking from day one
- MSVC `/W4 /WX` and Clang/GCC `-Wall -Wextra -Wpedantic -Werror` always on

## Formatting

`clang-format` with our `.clang-format` (LLVM base, 4-space indent, 100-col, pointer-binds-right). Pre-commit hook + CI check.

## Versioning

SemVer `MAJOR.MINOR.PATCH`. Pre-1.0 releases tagged `0.x.y`. Each release bumps the `ST_VERSION` cmake cache variable and writes it into the app via a generated `version.hpp`.

## Code signing

- Windows: EV certificate (when affordable) or sigstore for early pre-1.0
- macOS: Apple Developer ID + notarization in the release workflow
- Linux: GPG-signed `.deb`, BLAKE3 hash listed in release notes

## Documentation generation

- `doxygen` for the API reference
- `mkdocs` + `mkdocs-material` for the user-facing site (replaces the Jekyll site)
- Docs site auto-deploys on tag from `release.yml`
