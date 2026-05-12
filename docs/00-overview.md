# 00 — Overview

## What SubuwuTuner is

**SubuwuTuner is a comprehensive Subaru ECU tuning suite, written from scratch in modern C++23, free and open source under Apache 2.0.**

It reads, edits, datalogs, and reflashes the calibration on supported Subaru ECUs. It is designed to be the tool a Subaru owner, hobbyist, or independent tuning shop reaches for first: fast, native, scriptable, and built around a definition format you can version-control like code.

v1.0 ships narrow on purpose — VA (2015–2021) and VB (2022+) WRX manual transmission — because that's what the project can credibly brick-test on the Phase 4 bench rig. The architecture is multi-platform from day one, so v1.x adds STI, AT variants, older EJ-powered cars, BRZ/86, and the rest of the lineup as definitions land and flash routines clear testing. See `04-roadmap.md`.

## What it does

- **Read** the stock ROM off the ECU over a supported adapter (Tactrix OpenPort 2.0, OBDLink, OBDX Pro, etc.)
- **Identify** the calibration via CID against a definition pack and reveal every documented map
- **Edit** common maps (fuel, ignition, boost, throttle, idle, transmission) with a 2D/3D table editor
- **Datalog** at sustained high rates with per-PID timestamps and a zero-copy replay format
- **Auto-tune** MAF scaling and knock-based ignition pull (v1.1) from real driving logs
- **Flash** the ECU with delta-only writes, brick-protection recovery shim installed and verified beforehand, and tamper-evident manifests on every operation
- **Script** any of the above from `subuwutuner-cli` for batch and CI workflows

## What makes it worth building

- **Native and fast.** Sub-second startup, < 150 MB idle RAM, < 60 MB installer — see `05-improvements.md`.
- **Headless-first.** Same engine drives GUI and CLI; everything you can do in the UI you can script. Dyno operators, tune shops, and CI pipelines stop being second-class users.
- **Git-friendly definitions.** Definitions are TOML; projects are diffable; pull requests for new maps are realistic.
- **Open auto-tune.** MAF / knock-pull / closed-loop / boost-trim algorithms shipped first-party with engine-safety linting on by default. See `12-auto-tuning.md`.
- **Jurisdiction-aware, not paternalistic.** First-run profile picker (`motorsport-only` default) decides whether emissions-flagged edits warn, confirm, or stay silent. Engine-safety warnings are always strict. See `06-legal-ethics.md`.
- **Real brick protection.** First-class subsystem with bench-tested recovery shim, not a marketing bullet. See `05-improvements.md` and `08-testing-strategy.md`.
- **Cross-platform on day one.** Windows, macOS (Intel + Apple Silicon), Linux (x64 + arm64), all from the same CI matrix.

## Beyond v1.0

Subsequent versions expand both platform coverage and feature scope. Highlights:

- **Auto-tune** (MAF, knock-based ignition pull, closed-loop trim integration) — `docs/12-auto-tuning.md`
- **STI / older EJ-powered cars / BRZ / FB-powered SUVs** — `docs/04-roadmap.md`
- **Programmatic CAN reverse-engineering toolkit** — for engine swaps and cluster integration. Watch-and-label discovery loop, draft-DBC export. `docs/14-can-reverse-engineering.md`

## Non-goals

- Supporting non-Subaru platforms — different OEM = different project
- Shipping platform coverage faster than we can brick-test it (architectural support ≠ supported product)
- Cloud sync, telemetry servers, or any always-online dependency
- Hiding what the tool is doing from the user — the user is in charge of what's legal where they live (see `06-legal-ethics.md`)
- Pre-built defeat-preset calibrations as first-party content
- Full SavvyCAN/CANalyzer replacement — the CAN toolkit (`docs/14`) is focused on the scriptable discovery loop, not visual signal-graphing

## Success criteria for v1.0

A user with a supported adapter and a VA or VB WRX (MT) can:

1. Read the stock ROM off the ECU
2. Open it in SubuwuTuner, recognize all factory maps via a definition pack
3. Edit common maps (fuel, ignition, boost, throttle) with the table editor
4. Datalog at ≥ 50 Hz across ≥ 20 PIDs
5. Reflash with delta-only writes and brick recovery armed before the write
