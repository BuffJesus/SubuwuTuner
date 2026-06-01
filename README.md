# SubuwuTuner

A comprehensive, free, open-source **Subaru ECU tuning suite** written in modern C++23.

**Status:** pre-1.0, active development. Phases 0–4 complete hardware-free (rom/defs/edit/project, transport + ECU protocols, datalogger, flash orchestrator with optional gated 0xB6 path); Phase 5 design + IR + SH-2A codegen for VA shipped, RH850 codegen for VB at SH-2A parity (LoadConstant + LoadHookInput slices plus a 22-primitive CallPrimitive slice — int arithmetic, int compares, bool logic, branchless select, float arithmetic, `sqrt_float`, `flex_fuel_scale`, float compares, and nested CallPrimitive operands). Bench-validation against a real ECU is the open gate before v1.0.

## What this is

SubuwuTuner reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. It's designed to be the tool a Subaru owner, hobbyist, or independent tuning shop reaches for first: fast, native, scriptable, and built around a definition format you can version-control like code.

- **Native and fast** — sub-second startup, < 150 MB idle RAM, < 60 MB installer
- **Headless-first** — same engine drives the GUI and `subuwutuner-cli`; scriptable end-to-end
- **Git-friendly definitions** — TOML schema, diffable, pull-requestable
- **Open auto-tune** — MAF / knock-pull / closed-loop algorithms shipped first-party (v1.1)
- **Jurisdiction-aware, not paternalistic** — first-run profile picker, engine-safety warnings stay strict
- **Real brick protection** — bench-tested recovery shim subsystem, not a marketing bullet
- **Optional 0xB6 bulk-transfer write path** — off by default behind a two-layer gate (build flag `ST_ENABLE_BULK_REFLASH_CIPHER` + runtime `--enable-bulk-reflash-cipher`); see [`docs/26-bulk-reflash-cipher.md`](docs/26-bulk-reflash-cipher.md)
- **Cross-platform on day one** — Windows, macOS (Intel + Apple Silicon), Linux (x64 + arm64). Editing, datalogging, project work, auto-tune, and the CAN toolkit run on all three; some adapter-specific flashing paths are Windows-only (see [Platform feature matrix](#platform-feature-matrix) below).

v1.0 targets VA (2015–2021) and VB (2022+) WRX manual transmission. v1.x expands to STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the Subaru lineup. See [`docs/04-roadmap.md`](docs/04-roadmap.md).

For the full design rationale, read [`docs/`](docs/README.md):

- [Overview](docs/00-overview.md)
- [Reverse engineering strategy](docs/01-reverse-engineering.md)
- [Architecture](docs/02-architecture.md)
- [Tech stack](docs/03-tech-stack.md)
- [Roadmap](docs/04-roadmap.md)
- [What makes SubuwuTuner different](docs/05-improvements.md)
- [Legal & ethics](docs/06-legal-ethics.md)
- [Build & tooling](docs/07-build-and-tooling.md)
- [Testing strategy](docs/08-testing-strategy.md)
- [Risks](docs/09-risks.md)
- [Glossary](docs/10-glossary.md)
- [Definition format](docs/11-definition-format.md)
- [Auto-tuning](docs/12-auto-tuning.md)
- [Data distribution policy](docs/17-data-distribution-policy.md)
- [Installing definition packs](docs/install.md)

## Building

Requirements:

- C++23 compiler (MSVC 19.40+ / Apple Clang 16+ / GCC 14+ / MinGW-w64 15+)
- CMake 3.28+
- Ninja (recommended)
- Git

```bash
cmake --preset linux-gcc      # or win-mingw, win-msvc, mac-clang
cmake --build --preset linux-gcc
ctest --preset linux-gcc
./build/linux-gcc/bin/subuwutuner-cli --version
```

See [`CMakePresets.json`](CMakePresets.json) for the full preset list.

## Definition packs

SubuwuTuner ships the *infrastructure* — loader, table editor, project model, flash orchestrator, auto-tune kernels, CAN toolkit, GUI — without bundled calibration definitions. Drop a TOML pack into the per-platform user-data directory (see [`docs/install.md`](docs/install.md)), or generate one from a public RomRaider XML with [`tools/defgen/`](tools/defgen/). The repo includes [`fixtures/demo-pack/`](fixtures/demo-pack/) as a synthetic always-available example so the tool is exercisable from a fresh checkout.

The reasoning behind this distribution choice lives in [`docs/17-data-distribution-policy.md`](docs/17-data-distribution-policy.md).

## Platform feature matrix

The GUI, CLI, project model, auto-tune, and the CAN replay/decode pipeline are platform-symmetric. The asymmetry is in **adapter / flashing transports**, because J2534 v04.04 is a Windows-DLL API.

| Capability | Windows | macOS | Linux |
|---|---|---|---|
| GUI, CLI, project work, ROM viewer/editor | ✅ | ✅ | ✅ |
| TOML pack loader + `defgen` pipeline | ✅ | ✅ | ✅ |
| Auto-tune kernels (MAF + knock-pull) | ✅ | ✅ | ✅ |
| CAN replay / DBC decode / `.cdb` discovery | ✅ | ✅ | ✅ |
| Datalogging via OBDX Pro VX (USB-CDC) | ✅ | ✅ | ✅ |
| Datalogging via native handheld (USB-CDC) | ✅ | ✅ | ✅ |
| Flashing via OBDX Pro VX / native handheld | ✅ | ✅ | ✅ |
| Flashing via **J2534** (Tactrix OpenPort 2.0, Tactrix Pro J) | ✅ | ❌ Windows-only DLL | ❌ Windows-only DLL |
| Live CAN bus capture (`can-record --live`) | ✅ | ✅ | ✅ |

On macOS and Linux, **flashing requires an OBDX Pro VX or the SubuwuTuner native handheld** (per `docs/13-transport.md`). J2534 support on those platforms is not on the roadmap — the realistic substitute is the OBDX-VX/native path, which is what SubuwuTuner has put its transport-layer engineering behind. If you only own a Tactrix OpenPort 2.0, you flash from Windows.

## Safety

ECU tuning can damage engines and brick ECUs. **Read [`DISCLAIMER.md`](DISCLAIMER.md) before connecting this software to a real vehicle.**

## License

[Apache 2.0](LICENSE). See [`docs/06-legal-ethics.md`](docs/06-legal-ethics.md) for the project's stance on emissions, IP, and distribution.
