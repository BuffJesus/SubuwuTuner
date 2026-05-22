# 03 — Tech Stack

## Language & standard

- **C++23** with `std::expected`, `std::print`, `std::stop_token`, `consteval` use across the board.
- Fall back to C++20 only if we adopt a compiler that lacks 23 features we want (unlikely given the target list below).
- `std::flat_map` was previously listed here but **isn't used anywhere in `src/`**. libc++ (Apple Clang's stdlib) didn't ship `std::flat_map` until upstream Clang 20, so anything we add later will either polyfill via `boost::container::flat_map` behind a feature macro, or be a small in-tree implementation over `std::vector` + binary search.

## Toolchains we will support

| Platform | Compiler | Notes |
|---|---|---|
| Windows x64 | MSVC 19.4x (VS 2022 17.10+) | Primary dev target; J2534 DLLs are Windows-native |
| Windows x64 | Clang-cl | Secondary for sanitizer runs |
| macOS arm64/x64 | Apple Clang 16+ | Apple Silicon supported from day one |
| Linux x64/arm64 | GCC 14+, Clang 18+ | Mainstream distros plus SteamOS / Raspbian |

## Build system

**CMake 3.28+** with presets. No hand-written makefiles, no Bazel, no Meson. Rationale: CMake is what every C++ library we want to depend on already ships, and presets give us reproducible per-platform configs.

Dependencies pulled in via **CMake `FetchContent`** for source-built libraries (Catch2 v3, tomlplusplus, `tl::expected` as a portable fallback for `<expected>`, GLFW, Dear ImGui, ImPlot, nativefiledialog-extended). Manifest-mode vcpkg is on standby — no `vcpkg.json` lives in the repo today — for the day a dep brings system-package complexity (OpenSSL for a signed-update channel is the most likely trigger). Alternative considered: CPM.cmake — equivalent to FetchContent but adds a transitive dep on its own bootstrap; rejected on YAGNI grounds.

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

Shipped today — actually linked into `subuwutuner-cli` / `subuwutuner-gui`:

| Need | Library | License | Why |
|---|---|---|---|
| GUI core | Dear ImGui (docking branch) v1.91 | MIT | Immediate-mode rendering for grids + charts |
| Window/GL/input | GLFW 3.4 | Zlib | Cross-platform OpenGL context + input |
| Charts | ImPlot | MIT | 100k-point real-time plots; same author as ImGui |
| File dialogs | nativefiledialog-extended (nfd) | Zlib | OS-native Open/Save dialogs |
| TOML | `tomlplusplus` v3.4 | MIT | Pack format, project metadata, flash plans/manifests, `.stmod` |
| Expected | `tl::expected` | CC0 | Portable fallback for C++23 `<expected>` when the toolchain lacks it |
| Tests | Catch2 v3 | BSL-1.0 | Unit testing across `tests/unit/<module>/` |

Every shipped library is permissively licensed (MIT/Zlib/BSL/CC0), so the binary can ship statically linked with no shared-library shipping or written-offer paperwork.

CSV is emitted by our own `st::log::CsvSink` (no external parser); CRC32 is inline in `st::rom`. The CLI uses a hand-rolled argparse rather than CLI11. The headline GUI font (Inter) is `#include`d as a header-only binary blob built from the Inter regular ttf; JetBrains Mono is bundled the same way.

Reserved for future work — referenced in roadmap items, not yet linked:

| Need | Library | Where it lands |
|---|---|---|
| Signature verification | **libsodium** (preferred) or OpenSSL 3 | Signed-update channel + verified release manifests (post-v1). See LGPL/crypto note below. |
| USB raw | libusb 1.0 | OBDX Pro VX + native-handheld platform layer on Linux/macOS (hardware-gated). See LGPL note below. |
| Hashing | BLAKE3 | Flash-verify upgrade from CRC32 once bench rig validates it (docs/05 §4) |

### libusb LGPL linking strategy

libusb is LGPL-2.1, which compels either dynamic linking or distribution of object files / linkable artifacts so end users can re-link with a modified libusb. Plan per-platform:

- **Linux:** dynamic-link against the system-package libusb-1.0 (`libusb-1.0-0` on Debian/Ubuntu, `libusb` on Arch / Fedora). No additional ceremony.
- **macOS:** dynamic-link against the Homebrew or system libusb; document the dependency in the installer/DMG README.
- **Windows:** static-link for installer-size reasons. Releases ship the `subuwutuner-gui` object archive alongside the installer artifact, plus a one-page re-link recipe (cmake target + commands) in `docs/install.md`. This satisfies LGPL-2.1 §6(a) without forcing every user onto a DLL.

This was implicit in the earlier "the LGPL ceremony is worth it" line; it's now explicit so the v1.0 release-engineering checklist can implement it.

### Signature crypto — libsodium over OpenSSL

For verified signed-update manifests, **libsodium is the preferred path**: ~100 KB linked, stable API (Ed25519 / BLAKE2b primitives), small CVE surface. OpenSSL 3 adds ~5 MB to the binary plus ongoing CVE pressure for a use case that needs nothing beyond signature verify. If a future need adds TLS in-process (it shouldn't — updates come via the host's HTTPS), reconsider.

The earlier plan listed Lua + Sol2, FlatBuffers, nlohmann/json, FakeIt, libFuzzer, csv-parser, spdlog, `serial`, and a raw-OpenGL 3D surface widget. None of them shipped — the typed-dataflow IR in `st::feature::ir` replaced the Lua direction, CSV replaced FlatBuffers, and the 3D surface widget didn't make v1.x. They're tracked in this doc's history if a future direction-flip wants them back.

## What we are explicitly NOT using

- Java / JVM — defeats the native-startup and footprint goals
- Electron / web tech — defeats the size/perf goals
- Boost (anything we'd want from Boost is now in std or in a focused single-purpose library)
- Conan — vcpkg already chosen
