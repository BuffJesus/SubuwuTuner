# Glossary

A working vocabulary for anyone joining the project who has C++
experience but not a tuning background. Same content as
[`docs/10-glossary.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/10-glossary.md){ target="_blank" } —
this page is the user-facing copy.

## Tuning basics

| Term | Meaning |
|---|---|
| **ECU** | Engine Control Unit — the embedded computer running the engine |
| **Calibration / "tune"** | The parameters and tables in the ECU that determine its behavior |
| **ROM** | A complete dump of the ECU's flash memory; the file format is just raw bytes |
| **Map / Table** | A 1D, 2D, or 3D table of values in the ROM, indexed by sensor inputs (e.g., RPM × Load → fuel) |
| **Axis** | The index variable of a table; "RPM axis" is the row labels of a 2D map |
| **Scaling** | The transform from raw stored bytes to engineering units (e.g., `value * 0.001953125 → bar`) |
| **Definition** | Metadata describing where every map lives in the ROM and how to scale it |
| **Datalog** | Time-series capture of ECU internal values during operation |
| **PID** | Parameter ID — a single addressable ECU value (RPM, IAT, etc.) |
| **Flash / Reflash** | Erasing and reprogramming the ECU's flash memory with a new calibration |
| **Brick** | An ECU that can no longer boot due to a failed / corrupted flash |
| **Seed/Key** | Authentication challenge the ECU issues before allowing writes |
| **DTC** | Diagnostic Trouble Code — the codes that light up the check-engine light |

## Protocols

| Term | Meaning |
|---|---|
| **OBD-II** | Standard diagnostic port and protocol every modern car exposes |
| **J2534 ("Pass-Thru")** | SAE standard API for talking to vehicle ECUs from a PC through a vendor adapter |
| **KWP2000** | Keyword Protocol 2000 (ISO 14230) — older diagnostic protocol; VA WRX uses this over K-Line |
| **UDS** | Unified Diagnostic Services (ISO 14229) — modern protocol; VB WRX and many VA generations use this over CAN |
| **SSM** | Subaru Select Monitor — Subaru's proprietary OEM diagnostic protocol |
| **CAN / CAN-FD** | Controller Area Network — the physical bus modern cars use for ECU comms |
| **K-Line** | Older single-wire diagnostic bus, pre-CAN |
| **ISO-TP** | ISO 15765 transport-layer segmentation over CAN |

## Hardware

| Term | Meaning |
|---|---|
| **OBDX Pro VX** | The USB-OBD adapter SubuwuTuner targets as primary |
| **Tactrix OpenPort 2.0** | The de-facto J2534 adapter for Subaru tuning |
| **ELM327** | Cheap OBD-II adapter chip; serial AT-command interface; read-only for our purposes |
| **STN-series (OBDLink EX, etc.)** | More capable ELM327-compatible adapters with extra commands and higher throughput |
| **COBB AccessPort V3** | Commercial Subaru tuner with on-device tune vault; SubuwuTuner reads from / writes to the vault as a "file system" |
| **Renesas E2-Lite** | Legacy Renesas emulator previously proposed for SH-2A recovery; not validated or approved for the current VA SH72543-style ECU |

## Subaru-specific

| Term | Meaning |
|---|---|
| **VA / VB** | Subaru chassis codes. VA = 2015–2021 WRX (FA20 engine). VB = 2022+ WRX (FA24 engine) |
| **EJ / FA** | Subaru engine families. EJ is older (boxer 2.0/2.5 turbo, pre-2015 WRX/STi). FA20/FA24 are direct-injection turbos in VA/VB |
| **MT** | Manual transmission. v1.0 definition packs are MT-specific because the transmission/torque maps differ from AT variants |
| **CID** | Calibration ID — the per-firmware identifier that picks the right definition pack (e.g., `LF79103P`) |
| **AVCS** | Active Valve Control System — Subaru's variable cam timing |
| **DAM** | Dynamic Advance Multiplier — the ECU's running knock-feedback factor |
| **DIT** | Direct Injection Turbo — the engine family code for the FA20DIT |
| **TGV** | Tumble Generator Valve — intake control device |
| **EGR** | Exhaust Gas Recirculation — emissions device |
| **TWC** | Three-Way Catalyst — the catalytic converter |
| **MAF / MAP / IAT** | Mass Air Flow / Manifold Absolute Pressure / Intake Air Temperature sensors |
| **AFR / Lambda** | Air-Fuel Ratio (mass ratio of air to fuel) / Lambda (AFR normalised to stoichiometric) |

## SubuwuTuner-specific

| Term | Meaning |
|---|---|
| **`.stune`** | A SubuwuTuner project directory |
| **`.stmod`** | A custom-features node graph file |
| **`.cdb`** | A CAN-discovery bundle (frames + decoded signals + baseline) |
| **`.ptm`** | A COBB-format tune file (importable via `ptm import`) |
| **Pack** | A definition pack — TOML describing tables, axes, scaling, identification |
| **`pack.toml`** | The root file of a directory-style pack |
| **Edit** | A single mutation in `edits.toml`; always undoable |
| **Workflow tag** | Tag on a group of edits that lets them undo as a single unit (e.g., `"fa24_swap"`) |
| **Audit log** | CRC32-protected append-only log of every domain-significant action |
| **Transport** | The `ITransport`-implementing layer that reaches a live ECU |
| **SA variant** | A SecurityAccess algorithm flavor; CLI-selectable via `--sa-variant` |
| **Tune-export** | The sum-preserving cal-write pipeline for LF79xxxP ECUs (`st::tune_export`) |
