# 49 - DIY SH-2A recovery probe feasibility

Updated 2026-08-02. This is a research and planning note, not a pin-by-pin
recovery procedure. It must not be used to connect hardware to the bricked ECU
until the exact MCU, ECU board revision, debug pads, voltage domains, and
recovery image are confirmed.

## Short answer

Yes, a community-built recovery stack is technically plausible. No, a Teensy
is not a drop-in replacement for a Kess/Trasdata/E200F-class tool, and building
one is unlikely to be the fastest or cheapest way to recover the current ECU.

The most realistic architecture is:

`PC recovery controller -> USB JTAG transport probe -> level/reset hardware ->
SH-2A H-UDI target`

A Teensy could occupy the transport-probe position. The difficult part is not
generating TCK/TMS/TDI/TDO; it is implementing the SH-2A H-UDI command layer,
debug register access, flash-control sequence, target-specific protection
handling, and verified read/erase/program behavior.

## What the current evidence establishes

- Renesas describes the SH72543R as an SH-2A device with an H-UDI interface
  compatible with JTAG. Renesas's E200F emulator documentation explicitly
  includes SH72543R among its supported devices.
- Renesas's E10A documentation describes the legacy E10A as a SuperH H-UDI
  debugger/programmer and exposes a flash-download path, but availability and
  exact SH72543 target support must be verified for any used unit.
- OpenOCD provides many physical JTAG adapter drivers, but its documented CPU
  target list does not establish SH72543 H-UDI target support. “OpenOCD can
  drive this adapter” is not the same as “OpenOCD can recover this ECU.”
- Renesas's SH7058 documentation and SCI/XMODEM examples are useful for the
  older SH7058 branch, but they cannot be assumed to apply to SH72543 or to
  the Subaru ECU's board-level boot pins.

References:

- [Renesas SH-2A/E200F precautions](https://www.renesas.com/tw/en/document/mat/e200f-emulator-precautions-using-sh-2a-sh-2)
- [Renesas SH72543R interface-converter documentation](https://www.renesas.com/en/document/mat/sh72546rfcc-sh72544r-sh72543r-user-system-interface-converter-board-r0e572546cbf10-converter-board)
- [Renesas E10A-USB](https://www.renesas.com/en/software-tool/e10a-usb)
- [Renesas E10A overview](https://www.renesas.com/en/software-tool/e10a-discontinued-product)
- [Renesas SH7058 hardware manual](https://www.renesas.com/en/document/mah/sh-2e-sh7058-f-ztat-tm-hardware-manual)
- [OpenOCD adapter configuration](https://openocd.org/doc-release/html/Debug-Adapter-Configuration.html)

## Three possible recovery paths

### Path A - Serial/boot mode

The SH family has boot/user-program modes in silicon, but the existence of a
mode in the MCU manual does not prove that the Subaru ECU exposes the required
mode pins or routes the required SCI channel to an accessible pad. We need the
exact SH72543 variant manual, ECU schematic/board tracing, mode-pin state,
reset timing, SCI pins, baud negotiation, and programming command format.

This path is attractive because a Teensy or USB-UART bridge could be the host.
It is also the path most likely to fail if the ECU vendor did not expose the
mode pins or if the on-chip boot mode is not the path used by the commercial
recovery tool.

### Path B - H-UDI/JTAG with an existing legacy probe

An actual E200F or compatible E10A-USB is the highest-value research purchase
if one can be found at a reasonable price. It gives us a known-good physical
and target-protocol reference. The obstacle is old software, discontinued
hardware, connector/board access, and whether the Subaru board exposes a usable
H-UDI connection.

This is the best way to learn before attempting a home-built probe.

### Path C - DIY JTAG transport

Use an FT2232H/Tigard-class JTAG transport or a Teensy as a low-level TAP
engine, then write a PC-side H-UDI implementation. The PC should own the
recovery state machine; the microcontroller should remain a dumb, deterministic
signal engine with level shifting, reset control, target-voltage sensing, and
trace capture.

Do not begin by adding erase/program commands. The first milestones are:

1. electrically safe target-voltage detection;
2. reset and TAP state-machine control;
3. stable IDCODE/chain response on a known-good compatible target;
4. read-only H-UDI identification;
5. read-only memory/register access;
6. full-image read, hash, and repeatability check;
7. only then, a single-sector erase/program experiment on a sacrificial ECU.

## Why a Teensy alone is not enough

- H-UDI is JTAG-compatible at the physical boundary but is not simply generic
  ARM JTAG or SWD.
- The adapter must respect target voltage, reset, TRST, emulator-enable, mode,
  and clock constraints. The SH72543 documentation specifically constrains
  JTAG clock relative to the target clock.
- Flash programming requires target-specific debug accesses and flash-control
  sequencing. A raw TAP bit-banger cannot infer these safely.
- A failed DIY implementation can erase the only recoverable copy or damage
  board-level debug pins. The first test target must not be the current brick.

## Recommended DIY development plan

### Stage 0 - identity and acquisition

- Photograph and record the ECU label, PCB revision, MCU top-mark, package,
  debug test pads, and all board power rails.
- Obtain the exact SH72543/SH72543R hardware and E200F/H-UDI documentation.
- Look for a used E200F/E10A or a recovery service before buying a generic E2,
  E2 Lite, Kess clone, or unverified adapter.
- Source a second sacrificial ECU for experiments; reserve the current bricked
  unit for read-only recovery once the path is proven.

### Stage 1 - transport-only prototype

- Build a USB-controlled JTAG transport with target-voltage sensing and
  current-limited power.
- Implement TAP reset, clock-rate control, pin-state logging, and trace capture.
- Add a hard interlock that refuses erase/program operations in firmware until
  an explicit development flag and a verified target identity are present.
- Keep all wire-level traces and pin assignments analyst-side until validated.

### Stage 2 - protocol oracle

- If an E200F/E10A becomes available, capture host-to-probe USB traffic during
  identification and read-only operations on a compatible development target.
- Compare the traces against the Renesas manuals and isolate transport framing
  from H-UDI target commands.
- Implement the PC-side protocol in a separate `st::recovery_probe` module;
  do not couple it to the normal OBD flasher yet.

### Stage 3 - read-only ECU validation

- Identify the target without modifying it.
- Read a small immutable region repeatedly and compare hashes.
- Read the complete image and verify size, repeatability, and exact-CID match.
- Add power-loss and cable-disconnect tests while still in read-only mode.

### Stage 4 - recovery write

- Write only an approved exact-CID stock image.
- Verify every block, read back the full image, power-cycle, and re-identify.
- Keep RAM-marker writes disabled until firmware/CID evidence proves their
  addresses and values apply to that exact image.

## Decision rule

For the current ECU, pursue recovery service or a verified legacy probe in
parallel with the DIY research. Do not make the bench rig depend on a new
reverse-engineered JTAG stack. The DIY stack is valuable as a long-term
SubuwuTuner capability and could eventually make the project uniquely useful,
but it is a multi-stage instrumentation project, not a weekend Teensy sketch.

## Product integration later

Once proven, the recovery probe belongs behind a separate capability boundary:

- `recovery identify`
- `recovery read --output ...`
- `recovery verify --image ...`
- `recovery program --exact-cid ... --confirm`
- `recovery audit`

The normal tuning UI should show recovery capability and evidence, but the
recovery writer should remain a deliberately narrow expert tool with stronger
interlocks than ordinary project editing.
