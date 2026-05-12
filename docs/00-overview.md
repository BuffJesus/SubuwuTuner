# 00 — Overview

## What is Atlas?

Atlas is a closed-source, free-for-personal-use Java application that lets owners of 2015+ Subaru WRX vehicles read, edit, log, and reflash their ECU calibration. Its standout features per the public README:

- Custom **brick protection** flash routine that improves on OEM recovery
- Compression and **delta-aware flashing**
- Native drivers for **Tactrix OpenPort 2.0**, **OBDLink** (STN-based), and **ELM327** over serial/COM
- 2D/3D table editor and gauge/dashboard system, dark-mode UI
- **Visual node-graph "custom feature designer"** that compiles user logic into the tune
- Composite project files bundling multiple calibrations and ECU configs

## What is SubaruTuner?

A ground-up C++ rewrite that aims to match Atlas's feature set on the same vehicles, then surpass it on the axes where a native, modern codebase has a structural advantage: startup time, memory footprint, flashing throughput, plugin extensibility, and headless/CLI automation. See `05-improvements.md`.

The longer-term goal is a **comprehensive Subaru tuning application**: any Subaru ECU we have a definition for and a verified flash routine for. v1.0 ships narrow (VA + VB WRX MT) so we can actually brick-test what we ship; subsequent versions expand platform coverage incrementally. The architecture is designed for breadth from day one — new vehicles are TOML definition files, and protocol clients are model-agnostic. See `04-roadmap.md` for the expansion order.

## What we have to work with

- **`atlas-public-main/`** — Jekyll docs site, screenshots, license. **No Java source.**
- **`Definitions-VA_WRX_MT.atlas`** and **`Definitions-VB_WRX_MT.atlas`** — ZIP archives. Each contains:
  - `project.aproj` — the composite project file (binary, encrypted)
  - Several `<uuid>_0.acf` files — individual calibration blobs (binary, encrypted)
  - `.gitignore` (cosmetic)
- Atlas is distributed as obfuscated JVM bytecode. Decompilation is technically possible but the EULA and emissions posture make it a poor starting point.

A first peek at `project.aproj` shows **16 leading zero bytes** followed by high-entropy data. That is consistent with an AES‑CBC payload where the IV slot is present but zeroed (key derived elsewhere), or an explicit IV field that this particular blob simply zeroes. Either way, the format is not plain XML/JSON like classic Subaru Definition Files (`ECUFlash` `*.xml`).

## Non-goals

- Supporting non-Subaru platforms ever (different OEM = different project)
- Shipping platform coverage faster than we can brick-test it (architectural support ≠ supported product)
- Cloud sync, telemetry servers, or any always-online dependency
- Hiding what the tool is doing from the user. The user is in charge of what's legal where they live (see `06-legal-ethics.md`)
- 1:1 visual clone of Atlas — we will rebuild the UX, not pixel-copy it

## Success criteria for v1.0

A user with a Tactrix OpenPort 2.0 and a supported VA or VB WRX can:

1. Read the stock ROM off the ECU
2. Open it in SubaruTuner, recognize all factory maps via a definition pack
3. Edit common maps (fuel, ignition, boost, throttle) with the table editor
4. Datalog at ≥ 50 Hz across ≥ 20 PIDs
5. Reflash with delta-only writes and brick recovery armed before the write
