# 10 — Glossary

A working vocabulary for anyone joining the project who has C++ experience but not a tuning background.

| Term | Meaning |
|---|---|
| **ECU** | Engine Control Unit — the embedded computer running the engine |
| **Calibration / "tune"** | The parameters and tables in the ECU that determine its behavior |
| **ROM** | A complete dump of the ECU's flash memory; the file format is just raw bytes |
| **Map / Table** | A 1D, 2D, or 3D table of values in the ROM, indexed by sensor inputs (e.g. RPM × Load → fuel) |
| **Axis** | The index variable of a table; "RPM axis" is the row labels of a 2D map |
| **Scaling** | The transform from raw stored bytes to engineering units (e.g. `value * 0.001953125` → bar) |
| **Definition** | Metadata describing where every map lives in the ROM and how to scale it |
| **Datalog** | Time-series capture of ECU internal values during operation |
| **PID** | Parameter ID — a single addressable ECU value (RPM, IAT, etc.) |
| **Flash / Reflash** | Erasing and reprogramming the ECU's flash memory with a new calibration |
| **Brick** | An ECU that can no longer boot due to a failed/corrupted flash |
| **Seed/Key** | Authentication challenge the ECU issues before allowing writes |
| **DTC** | Diagnostic Trouble Code — the codes that light up the check-engine light |
| **OBD-II** | The standard diagnostic port and protocol every modern car exposes |
| **J2534 ("Pass-Thru")** | SAE standard API for talking to vehicle ECUs from a PC through a vendor adapter |
| **KWP2000** | Keyword Protocol 2000 (ISO 14230) — older diagnostic protocol; VA WRX uses this over K-Line |
| **UDS** | Unified Diagnostic Services (ISO 14229) — modern protocol; VB WRX uses this over CAN |
| **SSM** | Subaru Select Monitor — Subaru's proprietary OEM diagnostic protocol |
| **CAN / CAN-FD** | Controller Area Network — the physical bus modern cars use for ECU comms |
| **K-Line** | Older single-wire diagnostic bus, pre-CAN |
| **ELM327** | Cheap OBD-II adapter chip; serial AT-command interface; read-only for our purposes |
| **STN-series (OBDLink EX, etc.)** | More capable ELM327-compatible adapters with extra commands and higher throughput |
| **Tactrix OpenPort 2.0** | The de-facto J2534 adapter for Subaru tuning |
| **VA / VB** | Subaru chassis codes. VA = 2015–2021 WRX (FA20 engine). VB = 2022+ WRX (FA24 engine) |
| **EJ / FA** | Subaru engine families. EJ is older (boxer 2.0/2.5 turbo, pre-2015 WRX/STi). FA20/FA24 are direct-injection turbos in VA/VB |
| **MT** | Manual transmission. Atlas's definition files are MT-specific because the trans/torque maps differ |
| **TGV** | Tumble Generator Valve — an emissions/intake control device |
| **EGR** | Exhaust Gas Recirculation — emissions device |
| **TWC** | Three-Way Catalyst — the catalytic converter |
