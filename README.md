# SubuwuTuner

A comprehensive, free, open-source **Subaru ECU tuning suite** written in modern C++23.

**Status:** pre-1.0, active development. Phase 0 complete; Phase 1 in progress.

## What this is

SubuwuTuner reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. It's designed to be the tool a Subaru owner, hobbyist, or independent tuning shop reaches for first: fast, native, scriptable, and built around a definition format you can version-control like code.

- **Native and fast** — sub-second startup, < 150 MB idle RAM, < 60 MB installer
- **Headless-first** — same engine drives the GUI and `subuwutuner-cli`; scriptable end-to-end
- **Git-friendly definitions** — TOML schema, diffable, pull-requestable
- **Open auto-tune** — MAF / knock-pull / closed-loop algorithms shipped first-party (v1.1)
- **Jurisdiction-aware, not paternalistic** — first-run profile picker, engine-safety warnings stay strict
- **Real brick protection** — bench-tested recovery shim subsystem, not a marketing bullet
- **Cross-platform on day one** — Windows, macOS (Intel + Apple Silicon), Linux (x64 + arm64)

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

## Safety

ECU tuning can damage engines and brick ECUs. **Read [`DISCLAIMER.md`](DISCLAIMER.md) before connecting this software to a real vehicle.**

## License

[Apache 2.0](LICENSE). See [`docs/06-legal-ethics.md`](docs/06-legal-ethics.md) for the project's stance on emissions, IP, and distribution.
