# 03 — Tech Stack

## Language & standard

- **C++23** with `std::expected`, `std::flat_map`, `std::print`, `std::stop_token`, `consteval` use across the board.
- Fall back to C++20 only if we adopt a compiler that lacks 23 features we want (unlikely given the target list below).

## Toolchains we will support

| Platform | Compiler | Notes |
|---|---|---|
| Windows x64 | MSVC 19.4x (VS 2022 17.10+) | Primary dev target; J2534 DLLs are Windows-native |
| Windows x64 | Clang-cl | Secondary for sanitizer runs |
| macOS arm64/x64 | Apple Clang 16+ | Apple Silicon supported from day one |
| Linux x64/arm64 | GCC 14+, Clang 18+ | Mainstream distros plus SteamOS / Raspbian |

## Build system

**CMake 3.28+** with presets. No hand-written makefiles, no Bazel, no Meson. Rationale: CMake is what every C++ library we want to depend on already ships, and presets give us reproducible per-platform configs.

Dependencies pinned via **vcpkg manifest mode** (manifest in repo, no global state, easy CI caching). Alternative considered: CPM.cmake — rejected because vcpkg gives us prebuilt Qt and OpenSSL on Windows without source builds.

## GUI

Two finalists, decide at end of Phase 1:

### Option A — Qt 6 Widgets (+ QML for novel views)

Pros: mature table editing, accessibility, native-feeling on Win/Mac/Linux, well-supported on M-series, dark-mode out of the box.
Cons: LGPL dynamic linking complexity, ~80 MB installer footprint, slower iteration than immediate-mode.

### Option B — Dear ImGui + ImPlot + a custom node-editor

Pros: tiny binary, instant iteration, trivial to embed in tools, MIT license.
Cons: accessibility is poor, native look-and-feel is absent, professional tuners will notice.

**Working recommendation: Qt 6 Widgets** for the main app; Dear ImGui only for internal dev tools (definition editor, byte inspector). A professional-grade 2D/3D table editor is the centerpiece of a tuning UI, and that's where ImGui struggles.

## Key third-party libraries (all OSS-friendly)

| Need | Library | License | Why |
|---|---|---|---|
| GUI | Qt 6 | LGPL 3 | See above |
| JSON / TOML | `tomlplusplus`, `nlohmann/json` | MIT | Standard, header-only |
| Binary schemas | FlatBuffers | Apache 2 | Zero-copy log records & log replay |
| CSV | `vincentlaucsb/csv-parser` | MIT | Log export |
| Crypto / TLS | OpenSSL 3 | Apache 2 | Future signed update channel |
| USB | libusb 1.0 | LGPL 2.1 | Tactrix OP2.0 raw USB |
| Serial | `serial` or our own thin wrapper | MIT | ELM/OBDLink COM |
| Plot | QCustomPlot or QtCharts | GPL/Commercial — pick before shipping | 2D gauges, log plots |
| 3D table view | Qt 3D or raw OpenGL | LGPL | Surface plots |
| Scripting | Lua 5.4 + Sol2 | MIT | Sandboxed user scripts |
| Logging | spdlog | MIT | App diagnostics |
| Tests | Catch2 v3 + FakeIt | BSL/MIT | Unit + mocking |
| Fuzzing | libFuzzer / AFL++ | MIT | ROM and protocol parsers |
| CRC / hashing | `crc++`, BLAKE3 | MIT/CC0 | Flash verify, project integrity |
| CLI | `CLI11` | BSD | `subuwutuner` headless binary |

All libraries shortlisted are permissive enough to ship in a single binary without forcing source release. The only license to negotiate carefully is **Qt's LGPL** — we'll dynamically link Qt and ship the shared libraries alongside, which is the conventional way to stay LGPL-compliant.

## What we are explicitly NOT using

- Java / JVM — defeats the native-startup and footprint goals
- Electron / web tech — defeats the size/perf goals
- Boost (anything we'd want from Boost is now in std or in a focused single-purpose library)
- Conan — vcpkg already chosen
