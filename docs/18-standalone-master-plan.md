# 18 — Standalone Handheld Tuner (Master Plan, v5)

Companion plan for the portable Teensy device. Slots into `docs/` as
`18-…`; **subordinate to** `docs/04`, `docs/05`, `docs/09`, `docs/13`,
`docs/15` — if it conflicts with them, they win.

> **v5 adds the ESP32 wireless co-processor.** v4's safety spine (the
> device runs the same audited orchestrator, cross-compiled) is unchanged.
> v5 adds one hard rule (§4) that keeps wireless entirely out of the
> existential-risk path.

---

## 0. Prime directive (unchanged)

> A failed flash must never leave an ECU in a non-recoverable state.

Project's existential risk: `docs/09` R1, `docs/05` §4, `docs/08` Tier 4.

---

## 1. Product vision

A standalone handheld in the spirit of a COBB / Bully Dog / SCT: flash
tune files **without a PC**, gauges, datalog, DTC read/clear, restore
stock — *and* act as a PC↔car interface, now also **wirelessly** (phone
dash, wireless log offload, wireless package delivery) like the OBDX.

Clean-room: handheld-tuner and wireless-OBD-adapter are *concepts*, free
to implement per `docs/15`. No competitor firmware/protocol is read. The
ESP32 BT/WiFi stack is Espressif's; Subaru comms remain the project's own
clean-room UDS/SSM.

---

## 2. Architectural spine (unchanged from v4)

The standalone executor is `st::flash` + `st::ecu::uds` + `st::ecu::ssm`
+ `st::log`, cross-compiled to the Teensy, driving an on-chip
`ITransport`. Same code path, same 37 flash tests, same Manifest/journal/
`plan_resume`, same mutation gate, same Phase 4 bench gate. The Teensy is
the sole safety brain.

---

## 3. Wireless via an ESP32 co-processor

**The ESP32 is a dedicated radio peripheral, not the brain.** It connects
to the Teensy over a narrow UART/SPI message protocol. The Teensy treats
it as just another untrusted requester — identical trust level to a USB
host or an SD-borne package. Nothing the ESP32 says bypasses the
Manifest / CID-stock / `st::policy` / brownout interlocks.

Rejected: using the ESP32's own TWAI/CAN and letting it run the show.
That co-locates a large WiFi/BT attack surface and a non-deterministic
RTOS with the flash path. Not acceptable for an R1-class operation.

Hardware: a **pre-certified module** (ESP32-S3-WROOM class — IC/FCC
pre-cert; user is in Canada). ESP32-S3 = mature BLE5 + WiFi4; ESP32-C6 if
WiFi6/Thread is wanted later. A pre-cert module shrinks RF design risk to
antenna placement + keep-out only.

What wireless is *for* (all non-flash-critical):

- Phone/PC as a remote gauge + datalog viewer (`st::log` stream sink over
  BLE/WiFi, in addition to / instead of the local screen).
- Wireless delivery of signed tune packages onto SD.
- Wireless log offload.
- Signed OTA updates of the ESP32 firmware.

---

## 4. The hard rule

> **The wireless link is never load-bearing during a flash.**

A flash is always executed locally by the Teensy over wired CAN/K-Line,
from a package already on SD, journaled to SD, brownout-protected.
Wireless is **observe-and-command only**; "command" = "start a
locally-executed, locally-gated operation," never "carry the bytes."

Consequences:

- A wireless dropout the instant a flash starts is a **non-event** — the
  flash is local and journaled; the phone just reconnects to the
  progress/journal.
- The PC-passthrough wireless `ITransport` is **read/datalog only** —
  mirrors `docs/13`'s ELM327 "no unproven write path on principle"
  stance. Live UDS/ISO-TP flash timing never travels over a lossy radio.
- PC-initiated flashing = the PC tells the device to flash a package
  *that is on the device's SD*; the device runs it locally and streams
  progress back. The PC never pushes UDS frames over WiFi.
- Wireless-delivered packages are safe because the signed-Manifest +
  CID-stock + on-device policy gate treat every package as
  untrusted-until-verified regardless of arrival channel. Delivery
  bypasses no interlock.

---

## 5. Tune package + marriage interlock (unchanged from v4)

- Package = signed `FlashPlan` (TOML + external `data_file` binary form)
  + tamper-evident `Manifest`. Mandatory verify before any write. **Pull
  the CRC32→BLAKE3 Manifest upgrade forward** — more important now that
  packages can arrive over a radio.
- Stock backup mandatory, CID-keyed (`read_full_rom` + existing Manifest
  metadata). No write without a verified stored stock for that exact CID.
- Restore-to-stock always one action away (a `FlashPlan` over the stored
  image).
- `st::policy` engine-safety gate runs on-device, blocking in every
  jurisdiction profile.

---

## 6. Port surface (v4 + wireless)

1. Embedded `ITransport` — FlexCAN_T4 + K-Line; on-wire ISO-TP here.
2. SD-backed path/IO + clock shim; SDIO (not SPI).
3. Heap/stack budget; streaming reads/writes (no whole-ROM buffering).
4. LVGL UI sampling `st::log`'s ring; UI carries no tuning logic.
5. Hardware brownout interlock — input-V sense + cutout; the only safety
   job not in shared code.
6. **Inter-MCU link to ESP32** — framed UART/SPI; a defined command/
   telemetry protocol. The Teensy validates every ESP32 request through
   the same interlocks; the ESP32 holds no keys to the flash path.

---

## 7. Hardware / electrical

- Socket the Teensy 4.1 as a module (not bare i.MX RT1062).
- SD is SDIO, not SPI.
- **Brownout is the top electrical risk** — buck-boost + hold-up cap +
  §6.5 interlock.
- Automotive transient protection — reverse-polarity + input TVS
  (ISO 7637-2/16750); series R + TVS/CM-choke on CAN.
- Isolation mandatory for any write-capable build.
- **ESP32 RF power is separable.** WiFi TX spikes ~500 mA, but wireless
  is idle/off during the crank-brownout-critical flash window (it's
  non-load-bearing, §4), so keep the RF rail **off** the budget that
  must survive a flash.
- **Provision the ESP32 on PCB rev A** even though the wireless feature
  lands later (§9): module footprint, inter-MCU lines, RF keep-out,
  separate RF rail. Retrofitting a radio onto a later spin is wasteful.

### AI-assisted PCB workflow

Human owns the critical ~20%, locked before any AI pass: isolation
barrier, brownout/power path, transient clamps, CAN front-end,
arm-to-flash interlock, **and the RF/antenna section** (RF + controlled
impedance are explicitly manual). AI-assisted: carrier passives, connector
breakouts, **inter-MCU UART/SPI + ESP32 decoupling**, BOM/sourcing,
first-pass route of non-critical nets, context-aware review. Flux
all-in-one for a sub-100-part one-off; KiCad + Quilter to own the
schematic format.

---

## 8. Security (raised by adding a radio)

- **Physical arm-to-flash interlock earns its keep** — a remote attacker
  still cannot flash without someone physically present at the car.
- Signed packages → a compromised radio can't deliver a flashable tune.
- ESP32 OTA must itself be signed.
- **Backstop principle:** a fully compromised ESP32 must remain
  *insufficient* to brick or maliciously tune a car — the Teensy
  interlocks do not trust it.
- BLE pairing/auth; WiFi locked down (WPA2/3, no open services);
  optional physical wireless-enable.

---

## 9. Roadmap — still rides docs/04

- **Pre-req (true today):** Phase 3 + Phase 4 orchestrator complete
  hardware-free. Device doesn't substitute for the OBDX you're waiting on
  for the Phase 1/3 *data* gates.
- **D1 — PC-passthrough bring-up** (wired USB-CDC `ITransport` + 2nd-Teensy
  ECU sim). Prove the PHY before trusting the on-device stack.
- **D2 — Cross-compile the stack** (`arm-none-eabi`; embedded `ITransport`
  + SD/clock shims). Gate: flash/UDS suite passes on target,
  byte-identical to PC build.
- **D3 — Phase 3 ride** (read/log on a real VA/VB; `read_full_rom` stock
  capture to SD). Same `docs/04` Phase 3 gate.
- **D4 — Phase 4 ride, device as flashing agent.** Device runs the
  cross-compiled orchestrator through the existing `docs/04` Phase 4 gate:
  100 bench flashes, zero bricks; power-cut-mid-`0x36` recovery;
  restore-from-SD-stock; no-stored-stock refusal; policy-block test.
  **No car until this gate. No wireless anywhere in this milestone.**
- **D5 — PCB rev A** (§7 critical blocks + ESP32 *provisioned*, firmware
  later); fab; bring-up; re-run D4.
- **D6 — Wireless feature (after D4, never near it).** ESP32 firmware +
  inter-MCU protocol; phone gauges/log offload; wireless package
  delivery; wireless read-only `ITransport`; signed OTA. Gated by the §4
  rule and §8.

---

## 10. Risk — folds into docs/09

R1 governs. Standalone + wireless additions:

| New | Failure | Mitigation |
|---|---|---|
| D-a | Bad/forged package (no PC operator) | Mandatory signed-Manifest verify; BLAKE3 pulled forward |
| D-b | Flash onto wrong/unknown ECU | CID-keyed stored-stock interlock |
| D-c | Brownout mid-write, nothing watching | §6.5 hardware interlock + hold-up cap; orchestrator error/journal path |
| D-d | Unsafe tune, no PC to warn | `st::policy` gate on-device, every profile |
| D-e | Wireless dropout mid-flash | Non-event by §4 — flash is local + journaled; radio not load-bearing |
| D-f | Remote attacker commands a flash | §4 (command ≠ bytes) + §8 (physical arm-to-flash + signed packages + ESP32-not-trusted backstop) |
| D-g | ESP32 firmware compromise | Backstop: insufficient to flash; signed OTA; isolated inter-MCU protocol |

Session timeout / ISO-TP / stale checksum / hung-mid-write remain
mitigated in the shared audited code, because the device runs that code.

---

## 11. Clean-room (firmware)

Mirror `docs/15`. Firmware ISO-TP/CAN/K-Line framing from public ISO
standards + public Teensy/FlexCAN APIs only. ESP32 side uses Espressif's
public SDK. Handheld-tuner and wireless-adapter are idea-side concepts;
no competitor firmware, capture, or scheme is read or mirrored.

---

## Out of scope

- Reimplementing `st::ecu::*` / `st::flash` (shared, not rebuilt).
- Any flash path that bypasses the shared orchestrator, the stored-stock
  interlock, or the §4 rule.
- Wireless in the live flash transport path (the §4 rule).
- COBB `.ptm` / proprietary encrypted tunes; locked-ECU decryption.
- ELM327-style write paths (`docs/13` non-goal).
- Cloud / always-online dependency (`docs/00` non-goal). Wireless is
  local-link only — no telemetry servers.

---

*Update after each D-milestone. Subordinate to the real docs at all times.*
