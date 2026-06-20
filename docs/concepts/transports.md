# Transports

A **transport** is how SubuwuTuner reaches a live ECU. The transport
layer abstracts over USB-OBD adapters, J2534 PassThru devices, and the
in-process mock used by every unit test.

## The four transports

| Transport | CLI flag | Use |
|---|---|---|
| **`MockTransport`** | (default in tests) | Unit tests, end-to-end flow validation hardware-free |
| **OBDX Pro VX** | `--transport obdx --device <COM>` | Recommended USB-OBD adapter; native CAN + ISO-TP, low latency |
| **J2534 PassThru** | `--transport j2534 --device <name>` | Any J2534 v04.04 adapter (in development — not yet wired end-to-end) |
| **Native handheld** | `--transport handheld --device <serial>` | Future portable Teensy/ESP32 device (`docs/18`) |

All four implement the same `ITransport` interface — domain code never
knows which transport is in use.

## OBDX Pro VX (recommended)

The OBDX Pro VX is a USB-CDC virtual COM port. No driver rebind needed
on any platform — Windows uses the built-in usbser driver, Linux uses
`cdc_acm`, macOS uses the built-in CDC driver.

```bash
# Windows
subuwutuner-cli rom-info --transport obdx --device COM5

# Linux / macOS
subuwutuner-cli rom-info --transport obdx --device /dev/ttyACM0
```

Capabilities the SubuwuTuner OBDX backend exercises:

- 11-bit and 29-bit CAN frame TX/RX (`0x7E0`/`0x7E8` for OBD-II
  diagnostics, arbitrary IDs for sniffing)
- ISO-TP segmentation and reassembly (single-frame, first-frame,
  consecutive-frame, flow-control)
- Bus filtering by ID list (e.g., `--filter "0x7E0,0x7E8"`)
- Autonomous periodic-frame TX slots (8 slots, DVI command 0x34
  sub-op 0x1A/0x1B/0x1C) — used for bench-rig vehicle-presence
  injection
- Trace mode (`ST_AP3_TRACE_USB=1` for AP3 specifically; transport-level
  trace lands in v1.x)

The DVI codec spec sits at
[`docs/13-transport.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/13-transport.md){ target="_blank" }.

## J2534

`--transport j2534` is a NotImplemented stub today. The OBDX path
covers every workflow this transport would. J2534 is on the
contributor wishlist for users with existing PassThru hardware.

## MockTransport

The transport every unit test uses, and the one `project-flash` writes
through end-to-end. It accepts a scripted sequence of UDS request /
response pairs and asserts on the wire shape SubuwuTuner sends.

You normally don't see MockTransport directly — it's the test-only
default. Its existence is the reason `st::flash` can ship hardware-free
and still claim coverage.

## Deeper detail

- [`docs/13-transport.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/13-transport.md){ target="_blank" }
  — full transport architecture, OBDX DVI codec, J2534 plan, native
  handheld plan.
- [`docs/18-standalone-master-plan.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/18-standalone-master-plan.md){ target="_blank" }
  — portable handheld design.
- [`docs/24-sniff-workflows.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/24-sniff-workflows.md){ target="_blank" }
  — Y-cable sniffing patterns layered on top of a transport.
