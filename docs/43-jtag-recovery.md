# JTAG Recovery Procedure for Bricked Subaru ECUs (LF79xxxP family)

> **Status**: Operational reference for when E2-Lite hardware is on hand. Compiled from analyst round-58 findings and Renesas SH-2A documentation.

## When to use this procedure

Use JTAG recovery if **all** of these are true:
- ECU shows power draw at 12V (typically 0.3-0.6 A) — CPU is alive
- No CAN traffic on the OBD bus despite power-cycle
- All UDS SIDs return transport timeout (no NRC, no response)
- 45-second cold-start passive sniff captures zero frames

These symptoms indicate the boot integrity check (`FUN_00000D6E` at ROM `0xD6E`) is returning nonzero, sending `_stage2_entry` into the `FUN_00000D98` recovery loop. The recovery loop does NOT initialize CAN, so the ECU is unreachable via OBD-II.

## What you need

### Hardware
- **Renesas E2 emulator Lite** (e.g., Digi-Key part `RTE0T00020KCE00000R`, ~$60-100)
- **14-pin Renesas user-interface cable** (often sold separately; the SH-2A uses the standard Renesas 14-pin debug header)
- **12 V regulated bench PSU** for the ECU (current capability ≥ 1 A; brief peaks higher)
- ECU bench harness exposing the debug header (most Subaru ECUs require shell disassembly to access the PCB)

### Software (free)
- **Renesas Flash Programmer** (RFP) — Windows download from `renesas.com`. Sufficient for erase + write + verify.
- **e² studio** (optional) — full IDE for halt/step debugging if needed for advanced cases.

### Data
- **Reference ROM**: `D:/Subuwu/subaru-data/reference-dumps/bench-junkyard-stock.bin` (LF79002P, 2 MiB, SHA-256 `5fb404e70beb912224e8b141be8cc2be20cda1965d129002305aef5c116d0d11`).
- For non-LF79002P ECUs: substitute the appropriate stock ROM for the donor's original CID. The procedure is otherwise identical.

## The procedure

### 1. Pre-flight checks
- Confirm reference ROM SHA-256 matches the stored hash.
- Confirm the E2-Lite firmware is current (RFP will warn if not).
- Disconnect any other harness connections to the ECU besides power and the JTAG cable. Leaving the CAN bus connected during boot mode operation can cause unpredictable behavior.

### 2. Identify the debug header on the PCB
For Subaru engine ECUs based on the SH-2A (Ho7058 / SH7058A family), the 14-pin Renesas debug header pinout is:

| Pin | Signal | Purpose |
|---|---|---|
| 1 | TCK | JTAG clock |
| 2 | GND | |
| 3 | TRST# | JTAG reset (active low) |
| 4 | EMLE | Emulator enable |
| 5 | TMS | JTAG mode select |
| 6 | (NC) | |
| 7 | TDO | JTAG data out (target → debugger) |
| 8 | Vcc | (sense) |
| 9 | TDI | JTAG data in (debugger → target) |
| 10 | ASEMD0# | ASE mode select |
| 11 | /RES | Target reset |
| 12 | GND | |
| 13 | (key) | |
| 14 | GND | |

If the PCB doesn't have a 14-pin header populated, the corresponding test pads usually exist and can be probed from the bottom side. Tactrix's Subaru SH7058 article describes pin-tracing from the CPU package as the fallback.

### 3. Connect and verify
1. ECU **unpowered** during connection.
2. Plug the 14-pin cable into the debug header.
3. Connect the E2-Lite to USB.
4. Apply 12 V to the ECU power input.
5. Launch RFP, create a new project for "Renesas SH-2A SH72531/Ho7058" (or closest match), select the E2-Lite as the tool.
6. RFP should establish a JTAG session and report the chip identity. If not, check power and cable seating.

### 4. Dump current flash state (diagnostic, optional but recommended)
Before erasing, dump the current flash to a file for forensics. This lets you compare to the reference ROM and confirm exactly which regions drifted (closes loops in the round-58 analysis).
- In RFP: `Read` → range `0x00000000..0x001FFFFF` → save as `bench-bricked-snapshot.bin`.
- Lifecycle: keep this file. SHA-256 it. It's evidence for round-59+ forensics on the silent-drop catalog.

### 5. Erase entire flash
- In RFP: `Erase` → all blocks (full chip).
- This bypasses FACI window protection because JTAG operates below the FCU protocol layer.
- Verify by reading back; should be all `0xFF`.

### 6. Write reference ROM
- In RFP: `Program` → file `bench-junkyard-stock.bin`, address `0x00000000`.
- This writes 2 MiB byte-for-byte. RFP handles the FCU sequencer internally — we don't need to use the broken `0x3910` path.
- Verify via RFP's built-in compare; should report no differences.

### 7. (Recommended) Write boot integrity RAM markers
The boot integrity check (`FUN_00000B88` at ROM `0xB88`) reads RAM markers at `0xFFF82016` and `0xFFF82002`. After a power-cycle these may be undefined. To force a steady-state pass without going through priming:
- In RFP or e² studio: halt CPU after reset.
- Live-write `0xFFF82016 = 0x55AA` (u16 BE, byte order: write `55 AA`).
- Live-write `0xFFF82002 = 0x5A5A` (u16 BE).
- Resume CPU.

If your tool doesn't support live RAM writes, skip this step — the first boot may go to recovery, but subsequent boots tend to settle once the markers initialize. If the ECU doesn't recover after 2-3 power cycles, this is the next step.

### 8. Power-cycle and verify
- Disconnect 12 V.
- Wait 30 seconds for caps to discharge.
- Reconnect.
- Probe via `subuwutuner-cli subaru-dsc-unblock-sequence --burst-write-extended` (existing path). Expected: chain proceeds through `50 02` Phase C, `74 20 01 04` Phase D, etc.
- Or simpler: `subuwutuner-cli subaru-uds-send-raw "01 00"` — Mode 01 PID 0 should return positive in default session.

### 9. If verification fails
Most likely cause: RAM markers wrong (step 7 skipped or didn't take). Diagnose:
- Re-dump flash, verify byte-for-byte against reference ROM.
- If flash matches, problem is RAM markers. Re-do step 7 with a tool that supports halt + live RAM writes.
- If flash doesn't match, the JTAG write didn't take cleanly. Retry with smaller block sizes.

Less likely causes:
- Hardware damage (e.g., the bricking event included over-voltage or a short). JTAG can't fix this.
- ECU is using a CID we don't have a stock ROM for. Substitute a known-good ROM for that CID.

## Future-proofing

Once the bench is restored:
- Always write tunes via the **Atlas tune-export pipeline** (per `st::tune_export` spec in `docs/44-tune-export.md`). The pipeline refuses writes that would brick the ECU again.
- If the user's actual car ever exhibits these symptoms, the SAME procedure applies — substitute the user's stock ROM (LF79103P-class) as the reference. The boot integrity rules are universal across the LF79xxxP family (verified round-58 cross-CID diff).

## References

- Round-58 analyst handoff: `findings/handoffs/HANDOFF-from-analyst-2026-06-19-round-58-TIER-1-CLOSED-...md`
- Boot integrity decompile: `findings/decompile/lf79103p/boot_integrity_2017-wrx-stock.bin.txt:803-960`
- Tactrix Subaru SH7058 reflash article: <https://www.tactrix.com/index.php?option=com_content&view=article&id=66> — older (EJ era) but procedurally similar
- Renesas E2 Lite user manual: <https://www.manualslib.com/manual/2168743/Renesas-E2-Lite.html>
- gregjhogan/renesas-bootmode (alternative open-source serial-mode tool, if E2-Lite is unavailable): <https://github.com/gregjhogan/renesas-bootmode>
