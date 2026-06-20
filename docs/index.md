# SubuwuTuner

A free, open-source Subaru ECU tuning suite.

**Platforms:** Windows · Linux · macOS &nbsp;·&nbsp; **License:** Apache 2.0 &nbsp;·&nbsp; **Language:** C++23

SubuwuTuner reads, edits, datalogs, and reflashes the calibration on
supported Subaru ECUs. It's a clean-room reimplementation — every
protocol fact is derived from public sources or live-bus capture, no
commercial-tool source code is touched, and the codebase is yours to
read, build, and modify.

v1.0 targets the WRX (VA 2015–2021 and VB 2022+, manual transmission);
v1.x expands to STI, AT variants, EJ-powered cars, BRZ/86, and the rest
of the Subaru lineup as definition packs land and flash routines clear
bench testing.

## Where to start

!!! tip "New here?"
    [Install](getting-started/installation.md) →
    [Quickstart](getting-started/quickstart.md) →
    [Your first tune](getting-started/first-tune.md).

- **Getting started** —
  [Install](getting-started/installation.md) ·
  [Quickstart](getting-started/quickstart.md) ·
  [Your first tune](getting-started/first-tune.md)
- **Concepts** —
  [Definition packs](concepts/definition-packs.md) ·
  [.stune projects](concepts/stune-projects.md) ·
  [Transports](concepts/transports.md) ·
  [Security Access](concepts/security-access.md) ·
  [Brick protection](concepts/brick-protection.md)
- **Workflows** —
  [Reading your ROM](workflows/reading-rom.md) ·
  [Editing a table](workflows/editing-tables.md) ·
  [Datalogging](workflows/datalogging.md) ·
  [Flashing safely](workflows/flashing.md)
- **Reference** —
  [Definition format](reference/definition-format.md) ·
  [.stune format](reference/stune-format.md) ·
  [Glossary](reference/glossary.md)

## What's in the box

Two binaries ship from one build:

- **`subuwutuner-cli`** — 70+ subcommands, scriptable, every domain capability
  reachable from headless. JSON output for CI integration.
- **`subuwutuner-gui`** — ImGui-based docking UI: table editor with full
  undo/redo, ImPlot heatmap + 3D slice picker, live datalogger, custom-features
  designer, audit panel, and per-platform user-data conventions.

Both are built from the same C++23 core (`st::core`, `st::Rom`,
`st::Definition`, `st::edit`, `st::Project`, `st::transport`,
`st::ecu::{ssm,uds}`, `st::flash`, `st::log`, `st::autotune`,
`st::feature`, `st::tune_export`, …). See
[Architecture](contributing/architecture.md) for the module map.

## Honest status (2026-06-19)

| Surface | State |
|---|---|
| ROM read / edit / diff / table editor | **Working** — full undo/redo, project model, audit log |
| Definition pack loader + linter | **Working** — TOML, `extends` chains, `pack-info` / `pack-lint` |
| Datalogger pipeline | **Working** — multi-sink SPSC, CSV, live gauge cluster |
| CAN reverse-engineering toolkit | **Working** — replay, DBC, baseline + change detection, `.cdb` |
| Auto-tune (MAF + knock-pull) | **Working** — engine-safety linting on by default, preview-then-commit |
| Custom-features designer (node graph → ROM patch) | **Working** — SH-2A + RH850 backends at 22-primitive parity |
| Transport stack (J2534, OBDX Pro VX, MockTransport) | **Working** |
| ECU protocols (SSM, UDS, SecurityAccess variants) | **Working** — factory + aftermarket SA, CLI-selectable |
| `st::flash` orchestrator (delta, manifest, journal, resume) | **Working hardware-free**; live-bench validation in progress |
| Tune-export pipeline (sum-preserving cal writes for LF79xxxP) | **Working** — 18 unit tests pass, awaits post-JTAG bench re-validation |
| Live flashing to a real ECU | **In progress** — bench rig brought up; JTAG recovery in flight |

The full picture lives in the design docs at
[`docs/04-roadmap.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/04-roadmap.md){ target="_blank" }
and the project README.

## Stance the project takes

- **Jurisdiction-neutral.** Per-jurisdiction profiles warn when
  appropriate; we refuse on engine-safety grounds, not regulatory
  grounds. Full reasoning:
  [`docs/06-legal-ethics.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/06-legal-ethics.md){ target="_blank" }.
- **Engine + ECU safety strict.** `st::flash` is treated as
  safety-critical; brick-protection is a real subsystem with per-ISA
  recovery recipes ([`docs/31`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/31-brick-protection-by-isa.md){ target="_blank" })
  and HIL tests gate releases.
- **Clean-room engineering.** Methodology + analyst/implementer wall:
  [`docs/15`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" }.
- **Path B distribution.** The tool ships public; calibration packs are
  user-supplied. Reasoning + workflow:
  [`docs/17`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" } +
  [Install](getting-started/installation.md).

---

!!! note "Docs site is still filling in"
    Getting Started, Concepts, and the high-traffic Workflows are in
    place. Per-panel GUI pages, per-subcommand CLI pages, and the full
    Reference section are being written in follow-up passes. The
    numbered design docs at
    [`docs/`](https://github.com/BuffJesus/SubuwuTuner/tree/main/docs){ target="_blank" }
    are the source of truth for everything below the user-facing
    surface and are not published here — browse on GitHub.
