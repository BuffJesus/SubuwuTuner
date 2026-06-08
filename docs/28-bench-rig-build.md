# 28 — Bench rig build runbook (2017 WRX FA20DIT)

This document is the step-by-step runbook for wiring a junkyard ECU onto
a bench harness, applying power without bricking it, and validating that
SubuwuTuner can talk to it the same way it talks to an in-car ECU. It is
the prerequisite for `docs/08-testing-strategy.md` **Tier 4** (HIL tests
against junkyard ECUs) and therefore for the Phase-4 ship gate (100
successful flash cycles, zero bricks).

**Target ECU:** 2017 WRX (FA20DIT), w/o STI variant. The CID family is
`LF79*` / `LF9*` (A-series, SH-2A, 2 MB). Same ECU as the developer's
own car — pins, voltages, immobilizer handshake all match what is
already deployed and validated in the field.

## Authoritative reference

Every voltage, pin number, and connector identifier in this doc comes
from the 2017 WRX FA20DIT FSM extract at:

`D:\Subuwu\findings\2017_WRX_FA_ECU_Bench_Reference.pdf`

Key sections (page numbers are within the extract, not the source FSM):

| Section | Pages | When to open |
|---|---|---|
| ECM I/O Signal Table (terminal-by-terminal function + expected voltage) | 21–25 | **Phase 1 harness verification — this is the single most important reference in the document** |
| Power Supply Circuit diagram | 174–187 | Phase 2 wiring |
| Ground Circuit diagram | 188–197 | Phase 2 wiring |
| CAN Communication System diagram | 198–200 | Phase 3 comms bring-up |
| Engine Electrical System diagram (full ECM schematic) | 201–232 | When something doesn't match the table and you need the wire topology |
| Immobilizer System diagram | 233–236 | Phase 4 — only if you're going to spin the engine (we won't) |

If a pin number in this runbook ever disagrees with the PDF, **the PDF wins**.

## Connectors

The ECU has three connectors. Wiring uses the connector + terminal
number convention (e.g. `B134-12` = connector B134, terminal 12).

| Designator | Color | Position | Pin count |
|---|---|---|---|
| **E158** | Black | Engine harness, left | 48 |
| **B134** | Brown | Body harness, center | 48 |
| **E159** | Gray  | Engine harness, right | 32 |

Pin numbering (PDF p.21): pins 1–12 are the top row, 13–24 the second
row, 25–36 the third, 37–48 the fourth. Numbered left-to-right looking
into the connector from the wire side (which is what you see when
you're crimping). On E159 the numbering stops at 32.

## Phase 0 — Parts list

Before powering anything, gather:

- **Junkyard 2017 WRX ECU** (FA20DIT, w/o STI). Verify by CID label or
  by reading the part number sticker — should start with `22765-` and
  match the LF7/LF9 family.
- **Pigtail harness** — either cut from a donor body harness with the
  three connectors intact, or a fresh aftermarket bench pigtail.
- **12V supply** capable of 5–10 A sustained (an ATX PSU works; a small
  shop battery on a charger works better — ECU is sensitive to
  brownouts during programming).
- **Ignition switch** — any SPST toggle wired between battery+ and
  the ECU's ignition input (B134-32).
- **OBDX Pro VX adapter** (in hand 2026-05-24; COM5 on the development
  Windows host per memory).
- **Multimeter** with continuity, DC volts to 20V, and ideally
  capacitance.
- **CAN termination resistor** — 120 Ω across CAN-H/CAN-L if the
  in-vehicle ones aren't being reused. Two 120 Ω in parallel = 60 Ω,
  which is what the ECU expects to see across the diff pair.

**Do not** include or wire up:

- Fuel injectors (pins E158-11/12/23/24/35/36/47/48). Per FSM table:
  "Measurement is prohibited while the engine is running." For a bench
  rig we never run the engine, but unloaded injector drivers can still
  oscillate; leave the pins floating.
- High-pressure fuel pump (E158-10, E158-22). Per FSM: "Measurement
  prohibited." Do not probe, do not load.
- Throttle body, ignition coils, fuel-pump driver. Not needed for
  read/write; loading them with bench dummies is more brick risk than
  it's worth for v1.0.

## Phase 1 — Harness verification (NO POWER applied)

This is the longest phase and the only one where a mistake is cheap.
Burn the time here.

1. **Identify every wire you plan to attach.** For each connector
   (E158, B134, E159) walk the pin list against PDF p.21–25 and write
   down which terminals you will connect and which you will leave
   floating. The wires the bench actually needs are listed below in
   Phase 2 and Phase 3.

2. **Continuity-check every planned connection** with the ECU
   *unplugged* and *unpowered*:

   - From the connector terminal on the harness side to wherever it's
     going (12V, GND, OBDX CAN-H, ignition switch, etc.).
   - From the connector terminal to **every other terminal you plan to
     use**, looking for shorts. A single misplaced strand of wire
     between, say, B134-12 (12V control-module power) and B134-19
     (sensor ground) will brick the ECU the instant you turn the key.

3. **Isolation-check every UNUSED terminal.** Specifically verify that:

   - The injector pins (E158-11/12/23/24/35/36/47/48) are not shorted
     to anything.
   - The HPFP pins (E158-10, E158-22) are not shorted to anything.
   - The ignition coil pins (E159-8/16/24/32) are not shorted to 12V
     or to each other.

4. **Connect the harness to the ECU.** Verify the connector clips
   fully seat and lock — partially-seated pins create intermittent
   contact that is indistinguishable from a brick during flash.

5. **Do a final visual pass.** Look for stray strands, untaped
   splices, exposed pin barrels. Nothing should be at "almost
   touching" distance to anything else.

## Phase 2 — Minimum-viable power-on (key-on, engine-off)

Goal: ECU boots, lights its internal regulators, and exposes the 5 V
sensor supply on B134-18 / E159-19. Nothing more. No CAN yet, no
attempts to talk to it.

### Pins to wire

Wire colors are from the FSM Engine Electrical System wiring diagrams (WI-160 / WI-161 et al). Subaru color code: `Y` = yellow, `L` = light blue (NOT light green), `R` = red, `G` = green, `W` = white, `B` = black, `Br` = brown, `Lg` = light green, `Sb` = sky blue. A slash (`Y/L`) means main color / stripe color — so `Y/L` is **yellow with a light-blue stripe** and `L/Y` is **light-blue with a yellow stripe** (different wires).

| Function | Pin | Wire color | Wire to | Expected at key-on |
|---|---|---|---|---|
| Control module power supply 1 | **B134-12** | Y/L | +12 V (switched via main relay; for the bench, switched directly via the main relay simulator switch) | Battery voltage |
| Control module power supply 2 | **B134-24** | Y/L | +12 V (same as above) | Battery voltage |
| Back-up power supply | **B134-23** | Lg | +12 V **always-on** (NOT switched) | Battery voltage |
| Ignition switch | **B134-32** | Y/R | +12 V switched by your ignition toggle | Battery voltage |
| Self-shutoff relay control | **B134-38** | Lg | Multimeter probe (output from ECU; this wire runs to the main relay coil) | **~0 V** with ECU running normally (active-low — see step 4) |
| Engine ground 1 | **B134-35** | B/L | Battery negative | 0 V |
| Engine ground 2 | **B134-47** | B/R | Battery negative | 0 V |
| Engine ground 3 | **E159-10** | (see FSM) | Battery negative | 0 V |
| Engine ground 4 | **E159-18** | (see FSM) | Battery negative | 0 V |
| Engine ground 5 | **E159-26** | (see FSM) | Battery negative | 0 V |
| Engine ground 6 | **E159-17** | (see FSM) | Battery negative | 0 V |
| Engine ground 7 | **E159-25** | (see FSM) | Battery negative | 0 V |
| Sensor ground 1 | **B134-19** | G/R | Battery negative | 0 V |
| Sensor ground 2 | **E159-27** | (see FSM) | Battery negative | 0 V |
| Sensor power supply (5 V out) | **B134-18** | L/Y | Multimeter probe (output from ECU) | **5 V — this is the boot success indicator** |
| Sensor power supply (5 V out, duplicate) | **E159-19** | (see FSM) | Multimeter probe (output from ECU) | **5 V** |

**Disambiguating the two Lg wires on B134.** Pin 23 (back-up power, +12 V always-on) and pin 38 (main relay control, ~0 V active) are *both* solid Lg. They live in different rows of the 48-pin grid — B134-23 is in row 2 (positions 13–24, sixth from the right), B134-38 is in row 4 (positions 37–48, second from the left). If you find a Lg wire reading **~0 V** while the ECU is running, that's pin 38 doing its job. If you find a Lg wire reading **battery voltage**, that's pin 23 (back-up). Anything else on an Lg wire = check your harness.

**All seven engine grounds must be tied** — the ECU's internal current
return paths assume them. Skimping on grounds is a leading cause of
bench-rig boot failures that look like dead silicon but aren't.

### Boot sequence

1. With your ignition toggle **OFF**, connect the back-up power
   (B134-23) to +12 V. Battery current draw should be < 10 mA.
2. Flip the ignition toggle **ON**. Current draw should jump to
   100–300 mA as internal regulators come up.
3. Measure B134-18 with a multimeter to chassis ground.
   - **5 V ± 0.1 V**: ECU booted. Go to Phase 3.
   - **0 V**: ECU did not boot. Power off immediately. Most common
     causes (in decreasing order): missing engine ground, swapped
     B134-12 and B134-19, dead ECU.
   - **Anything else**: power off, do not retry. Check for short to
     +12 V or to ground on a 5 V rail before re-applying power.
4. Verify **B134-38 reads ~0 V** (anything under ~0.5 V is healthy).
   This is the ECU's main-relay-control output: the ECU pulls the
   wire low (sinks current through the relay coil to ground via its
   internal switching FET) to keep the main relay engaged. So while
   the ECU is running normally, you should see ~0 V at this pin.
   - **~0 V**: ECU is asserting; main relay engaged; healthy.
   - **~12 V (battery voltage)**: ECU is NOT asserting; main relay
     dropped — which means the bench wouldn't have powered up. If you
     see 12 V here AND have your 5 V telltale, you're probing the
     wrong Lg wire (likely B134-23 back-up instead of B134-38 main-
     relay-control — see the wire-disambiguation note above).
   - **Anywhere in between** (e.g. 2–3 V): the wire is floating /
     poorly connected — check your B134 connector seating.

## Phase 3 — Comms bring-up (CAN to OBDX)

Goal: SubuwuTuner reads the CAL ID off the bench ECU via OBD-II Mode 0x09.

### Pins to wire

Subaru uses non-ISO-standard CAN colors here — don't assume Yellow/Green from generic CAN guides.

| Function | Pin | Wire color | Wire to |
|---|---|---|---|
| CAN-Hi | **B134-10** | R (Red) | OBDX CAN-H → OBD-II pin 6 |
| CAN-Lo | **B134-9** | L (Light blue) | OBDX CAN-L → OBD-II pin 14 |
| (Optional) 120 Ω termination | between CAN-H and CAN-L at the OBDX end | — | matches the ECU's internal expectation of a 60 Ω diff line (two terminations in parallel) |

Per FSM p.198–200 (CAN Communication System diagram), the in-vehicle
network has the ECU + TCM + gateway all on one bus with two 120 Ω
terminators (at the gateway and at the ECU). For a bench rig with only
the ECU on the bus, you replicate that by adding **one** 120 Ω at the
OBDX end — the ECU itself has 120 Ω built in.

### First exchange

With the OBDX adapter plugged in on Windows (COM5 per current setup),
power-cycle the bench ECU (ignition off → on) and run:

```powershell
subuwutuner-cli.exe rom-info --transport obdx --device COM5 --probe
```

Expected output: CAL ID + CVN + VIN read via Mode 0x09. The VIN field
will be whatever was last programmed into the junkyard ECU before it
was pulled — it doesn't have to match a car you own; it just has to
respond.

If this fails:
- **No `>` prompt from ELM probe**: OBDX adapter isn't seeing the bus.
  Check CAN-H/L wiring, check 120 Ω termination, check that the OBDX
  is actually on COM5 (`Get-PnpDevice -Class Ports`).
- **ELM probe OK, SetProtocol fails**: ECU is on the bus but not
  responding. Verify the bench ECU is fully booted (Phase 2 success
  criteria) and CAN-H/L aren't swapped.
- **SetProtocol OK, no Mode 0x09 response**: bus is alive but the ECU
  isn't seeing addressed frames. Most likely the OBDX CAN filter is
  misconfigured; pass `--verbose` to dump the DVI exchange and check
  the filter setup against the trace.

## Phase 4 — Immobilizer (skip for read/write bench work)

**Short version: ignore the immobilizer for now.**

The immobilizer (FSM p.233–236) gates *engine start*, not
*communication*. A bench ECU without immobilizer authorization will:

- ✅ Boot normally (Phase 2).
- ✅ Respond to OBD-II queries (Phase 3).
- ✅ Respond to SubuwuTuner ROM-read and ROM-write requests with the
  same SecurityAccess flow as an in-car ECU.
- ❌ Refuse to fire injectors / ignition coils when commanded to run.

Since our entire Tier-4 test plan is read/write/verify cycles without
ever spinning the engine, the immobilizer is a non-issue. We leave
E158-43 (immobilizer communication) floating.

If we ever want to run the engine on the bench (we won't for v1.0),
that's a separate project — Subaru's immobilizer challenge/response is
on its own LIN bus and is not part of the calibration we ship.

## Phase 5 — First read (`rom-pull`)

Goal: full 2 MB plaintext ROM image off the bench ECU.

```powershell
subuwutuner-cli.exe rom-pull `
    --transport obdx --device COM5 `
    --sa-variant factory `
    --output "D:\Subuwu\subaru-data\reference-dumps\bench-junkyard-stock.bin"
```

Expected: 2,097,152 bytes (0x200000) in ~7–10 minutes. The exact
duration depends on per-block timeout and the ECU's RequestDownload
buffer size; the orchestrator handles negotiation.

If the bench ECU has been touched by an aftermarket flasher or is
otherwise non-factory, the factory SA variant will be refused with
"incorrect key." Try in order:

1. `--sa-variant aftermarket` (= `aftermarket-l1`)
2. `--sa-variant aftermarket-l3`

See `docs/23-security-access.md` for the variant catalog and how to
diagnose which one to use.

Once a read succeeds, **immediately do a second read** and `fc /b`
(Windows) or `diff` the two files. They must be byte-identical. Any
delta is a sign of bus instability or ECU corruption and must be
resolved before any write test.

## Phase 6 — Brick-recovery validation (the gate)

Per `docs/08-testing-strategy.md` Tier 4 and the Phase-4 ship gate, the
bench rig has to demonstrate the recovery path before any customer
flash. The drill:

1. Start a write operation against the bench ECU (any test plan that
   touches the calibration region, NOT the bootloader).
2. **Pull 12 V on the ECU mid-write** by killing the ignition toggle
   AND the back-up power simultaneously. Worst-case timing is during
   a sector erase, but power-loss during a TransferData block is the
   most common real-world failure.
3. Re-power the ECU.
4. Run `subuwutuner-cli.exe flash --resume <manifest>` and verify the
   journal-based resume completes the write.
5. Read back the ROM and compare to the intended post-write image.

This loop must succeed 100 times in a row across both VA and VB junkyard
ECUs before any v1.0 customer flash. We are at 0/100; the rig and this
runbook are what gets us moving on that.

## Safety guardrails (re-read every session)

- **Always have a kill switch** between battery+ and the ECU 12 V
  rails. The instinct to "just disconnect the multimeter for a sec"
  when something looks wrong is exactly when you brick things.
- **Never probe HPFP or injector pins** (FSM "Measurement prohibited").
- **Read twice before any write.** A non-deterministic read is the
  bus telling you something is wrong; don't ignore it.
- **Photograph the harness** before every change. Twenty seconds of
  phone time saves the "wait, was that wire on B134-12 or B134-32 again"
  panic.
- **Treat the brick-recovery loop as the precondition for shipping**,
  not as a final check. The first time you write to the bench, it
  should be a write you intend to immediately recover from.

## Cross-references

- `docs/05-improvements.md` §4 — brick-protection model + recovery
  shim
- `docs/08-testing-strategy.md` Tier 4 — HIL test plan
- `docs/13-transport.md` — OBDX adapter wiring on the host side
- `docs/23-security-access.md` — SA variant catalog (factory +
  aftermarket-installer variants)
- `docs/26-bulk-reflash-cipher.md` — optional gated 0xB6 write path
  (off by default; not needed for Tier 4)
- `D:\Subuwu\findings\2017_WRX_FA_ECU_Bench_Reference.pdf`
  — the authoritative wiring + voltage reference cited throughout
