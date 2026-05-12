# SubaruTuner — C++ Port of Atlas

This folder contains the design and planning documents for **SubaruTuner**, a from-scratch C++ reimplementation (and rethinking) of [Atlas](https://motorsportsresearch.org), the free ECU calibration suite for the Subaru WRX (VA 2015–2021 and VB 2022+).

The reference Atlas application is a Java GUI tool. The public GitHub repo at `atlas-public-main/` ships **no source code** — only a Jekyll website and screenshots. The two `.atlas` files at the workspace root are ZIP containers holding an encrypted `project.aproj` and a handful of encrypted `.acf` calibration blobs. That means this is not a code translation; it is a clean-room reimplementation that has to reverse-engineer formats and protocols before it can match feature parity.

## Read in this order

1. [00-overview.md](00-overview.md) — What we are building and why
2. [01-reverse-engineering.md](01-reverse-engineering.md) — File formats, encryption, ECU protocol
3. [02-architecture.md](02-architecture.md) — Module layout and core abstractions in C++
4. [03-tech-stack.md](03-tech-stack.md) — Compiler, build, GUI, libraries
5. [04-roadmap.md](04-roadmap.md) — Phased delivery plan and milestones
6. [05-improvements.md](05-improvements.md) — Where we intend to beat Atlas
7. [06-legal-ethics.md](06-legal-ethics.md) — Emissions, IP, distribution constraints
8. [07-build-and-tooling.md](07-build-and-tooling.md) — Toolchain, CI, packaging
9. [08-testing-strategy.md](08-testing-strategy.md) — Unit, hardware-in-the-loop, fuzzing
10. [09-risks.md](09-risks.md) — What can kill the project, and mitigations
11. [10-glossary.md](10-glossary.md) — ECU/tuning vocabulary cheat sheet
12. [11-definition-format.md](11-definition-format.md) — TOML schema for definition packs
13. [12-auto-tuning.md](12-auto-tuning.md) — MAF/knock/closed-loop auto-tune design (v1.1+)

## Status

**Phase 0 complete; Phase 1 in progress** (2026-05-11). Skeleton, CMake + presets, `st::core` (Result/Error/Version/Crc32), `st::Rom` (file I/O, BE/LE reads, slice, scan_ascii, crc32), `subarutuner-cli rom-info` command. 40 unit tests green locally on MinGW g++ 15.2. GitHub Actions CI for Win MSVC / macOS Apple Clang / Linux GCC / Linux Clang ASan+UBSan. Next: `st::defs` (TOML loader using the schema in `11-definition-format.md`) and a `tools/defgen/` Python tool to convert RomRaider XML to our TOML.
