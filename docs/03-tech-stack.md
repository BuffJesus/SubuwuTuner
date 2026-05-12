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

Dependencies pulled in via **CMake `FetchContent`** for source-built libraries (Catch2, tomlplusplus, tl::expected, GLFW, Dear ImGui, ImPlot, nativefiledialog-extended). Manifest-mode vcpkg is on standby for the day a dep brings system-package complexity (OpenSSL for a signed-update channel is the most likely trigger). Alternative considered: CPM.cmake — equivalent to FetchContent but adds a transitive dep on its own bootstrap; rejected on YAGNI grounds.

## GUI

**Dear ImGui (docking branch) + GLFW + OpenGL3, with ImPlot for charts and nativefiledialog-extended for OS file dialogs.** The MVP is committed under `src/ui/`.

The decision flipped from the original "Qt 6 finalist" stance during Phase 2 once the domain layer stabilized. The reasoning:

- A tuner's UI is dominated by **numerical grids and live charts**. ImGui + ImPlot handles both natively; Qt's `QTableView` and `QChart` are general-purpose and need substantial customization to feel right for tuning workflows.
- ImGui is **MIT-licensed**, sidestepping the LGPL-compliance rituals Qt brings (shared-lib shipping, written-offer paperwork on distribution).
- **Iteration speed**: immediate-mode lets the same dev write the data layer and the view in one pass. No `.ui` files, no signal/slot ceremony, no QML/C++ split-brain.
- **Footprint**: a statically-linked ImGui+GLFW binary is ~5 MB; Qt would add ~80 MB of shared libs to the installer.
- **Industry precedent**: NVIDIA Nsight, Tracy, RenderDoc, OpenSCAD's new UI — production tools where polished ImGui is the norm.

Trade-offs accepted:

- **Accessibility** (screen-reader support, system contrast modes) is poor in ImGui. Tracked as a post-v1 gap, not a v1 gate.
- **Native look-and-feel** is absent by design. Mitigated with a tuned dark palette, paired UI + monospace fonts (Inter + JetBrains Mono), and tasteful padding/rounding — see the "Polish layer" notes in `02-architecture.md`.
- **Dialog primitives** — ImGui's built-in file picker is rudimentary, so we link nativefiledialog-extended for native Open/Save dialogs.

## Key third-party libraries (all OSS-friendly)

| Need | Library | License | Why |
|---|---|---|---|
| GUI core | Dear ImGui (docking branch) | MIT | Immediate-mode rendering for grids + charts |
| Window/GL/input | GLFW 3.4 | Zlib | Cross-platform OpenGL context + input |
| Charts | ImPlot | MIT | 100k-point real-time plots; same author as ImGui |
| File dialogs | nativefiledialog-extended (nfd) | Zlib | OS-native Open/Save dialogs |
| JSON / TOML | `tomlplusplus`, `nlohmann/json` | MIT | Standard, header-only |
| Binary schemas | FlatBuffers | Apache 2 | Zero-copy log records & log replay |
| CSV | `vincentlaucsb/csv-parser` | MIT | Log export |
| Crypto / TLS | OpenSSL 3 | Apache 2 | Future signed update channel |
| USB | libusb 1.0 | LGPL 2.1 | Tactrix OP2.0 raw USB |
| Serial | `serial` or our own thin wrapper | MIT | ELM/OBDLink COM |
| 3D table view | ImGui + raw OpenGL | MIT | Surface plots — ImPlot covers 2D, custom shader for 3D |
| Scripting | Lua 5.4 + Sol2 | MIT | Sandboxed user scripts |
| Logging | spdlog | MIT | App diagnostics |
| Tests | Catch2 v3 + FakeIt | BSL/MIT | Unit + mocking |
| Fuzzing | libFuzzer / AFL++ | MIT | ROM and protocol parsers |
| CRC / hashing | `crc++`, BLAKE3 | MIT/CC0 | Flash verify, project integrity |
| CLI | `CLI11` | BSD | `subuwutuner` headless binary |

Every GUI-stack library is permissively licensed (MIT/Zlib/BSD), so the binary can ship statically linked with no shared-library shipping or written-offer paperwork. The only LGPL surface that remains is `libusb` for raw USB on the transport side — that one is genuinely useful enough that the LGPL ceremony is worth it, and it's an optional component that only Tactrix-OP2-via-libusb users link against.

## What we are explicitly NOT using

- Java / JVM — defeats the native-startup and footprint goals
- Electron / web tech — defeats the size/perf goals
- Boost (anything we'd want from Boost is now in std or in a focused single-purpose library)
- Conan — vcpkg already chosen
