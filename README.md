# SubaruTuner

A from-scratch C++23 ECU calibration suite for the Subaru WRX (VA 2015–2021 and VB 2022+).

**Status:** pre-1.0, active development. Phase 0 (foundations).

## What this is

A clean-room reimplementation and rethinking of [Atlas](https://motorsportsresearch.org). Where Atlas runs on the JVM, SubaruTuner is native C++ aiming for sub-second startup, headless/scriptable workflows, git-friendly definitions, and rigorous brick-protection.

For the full design rationale, read [`docs/`](docs/README.md):

- [Overview](docs/00-overview.md)
- [Reverse engineering strategy](docs/01-reverse-engineering.md)
- [Architecture](docs/02-architecture.md)
- [Tech stack](docs/03-tech-stack.md)
- [Roadmap](docs/04-roadmap.md)
- [Improvements over Atlas](docs/05-improvements.md)
- [Legal & ethics](docs/06-legal-ethics.md)
- [Build & tooling](docs/07-build-and-tooling.md)
- [Testing strategy](docs/08-testing-strategy.md)
- [Risks](docs/09-risks.md)
- [Glossary](docs/10-glossary.md)

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
./build/linux-gcc/src/cli/subarutuner-cli --version
```

See [`CMakePresets.json`](CMakePresets.json) for the full preset list.

## Safety

ECU tuning can damage engines and brick ECUs. **Read [`DISCLAIMER.md`](DISCLAIMER.md) before connecting this software to a real vehicle.**

## License

[Apache 2.0](LICENSE). See [`docs/06-legal-ethics.md`](docs/06-legal-ethics.md) for the project's stance on emissions, IP, and distribution.
