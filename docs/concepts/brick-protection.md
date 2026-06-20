# Brick protection

`st::flash` is treated as safety-critical. The brick-protection
subsystem is what makes that claim true rather than aspirational.

## The threat model

A flash that fails partway through can leave an ECU in any of several
broken states:

- Mid-erase (sector blank, boot integrity check fails)
- Mid-write of a non-signature sector (recoverable)
- Mid-write of a signature sector (bricked)
- Cleanly written but checksum-balance broken (bricked at next start)
- Cleanly written but compensating checksum cell wasn't updated (bricked)

The safety model is **per-ISA**, because the recovery primitives the
ECU offers depend on its boot chain. SH-2A single-bank serial-boot
parts (FA20DIT) behave differently from RH850 dual-bank parts.

## Per-ISA recipes

| ISA | Recovery model |
|---|---|
| **SH-2A 2MB** (FA20DIT) | Single-bank, three flash signatures + RAM hash with battery-backed markers. Mid-flash recovery via JTAG (Renesas E2-Lite) or reflashing the boot signature sectors via FCU. |
| **RH850** (newer Subarus) | Dual-bank atomic swap. The inactive bank is written and verified before the swap commit. Mid-write of the inactive bank is recoverable by re-trying. |

Full per-ISA detail:
[`docs/31-brick-protection-by-isa.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/31-brick-protection-by-isa.md){ target="_blank" }.

## What `st::flash` does

- **Delta detection.** Only writes sectors whose content actually
  changes against the read-back baseline. Reduces blast radius and
  flash-cycle wear.
- **Per-sector erase / write / verify.** Each sector is verified
  byte-for-byte before moving on. A verify failure halts the flow
  immediately.
- **Journal-based resume.** Every step is journaled to the project
  directory before it executes; a power loss or crash mid-flash leaves
  enough breadcrumb to resume from the last verified step.
- **Manifest.** Pre-flash, the orchestrator emits a manifest describing
  exactly which sectors will be touched, their pre- and post-checksums,
  and the expected runtime. This is the artifact the policy gate
  reviews.
- **PolicyDenied gate.** Sector ranges flagged as bootloader, FACI-locked,
  or otherwise off-limits are rejected before any wire byte. The
  block-list is codec-level in `is_blocked_command`.
- **Audit subscriber.** Every Flasher state transition fires into the
  `st::audit` append-only log.
- **Optional 0xB6 bulk-transfer.** A faster write path that's
  deliberately gated behind two CMake flags + a CLI flag. Off by
  default; the trade-off is documented in
  [`docs/26-bulk-reflash-cipher.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/26-bulk-reflash-cipher.md){ target="_blank" }.

## The tune-export pipeline

For LF79xxxP ECUs (SH-2A FA20DIT family), calibration writes must
preserve the boot-integrity contract:

- The u16 BE sum of `[0x6000, 0x200000)` must equal `0x5AA5`.
- The boot integrity signatures (`0x6000 = 0x5555`, `0x6010 = 0x0504`,
  `0x6C = 0x05`, `0x1FFFF2 = 0xAAAA`) must remain intact.
- FACI-locked regions (`[0, 0x8000)`) must not be touched (the FCU
  silently drops the write and returns success — leading to a bricked
  ECU at next start).

`st::tune_export` handles all three by emitting a sum-preserving write
plan with a compensating balance cell at `0x1FFFFE`. Spec:
[`docs/44-tune-export.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/44-tune-export.md){ target="_blank" }.

## When things go wrong: JTAG recovery

If a flash leaves an ECU bricked despite the safety subsystem,
[`docs/43-jtag-recovery.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/43-jtag-recovery.md){ target="_blank" }
documents the Renesas E2-Lite JTAG recovery procedure: hardware BOM,
14-pin debug header pinout, RFP step-by-step, RAM marker preset,
post-procedure verification.

## What you can do today

- Inspect what `st::flash` would write without flashing:
  ```bash
  subuwutuner-cli flash-dry-run --project my-tune.stune
  ```
- Verify the boot-integrity contract on a candidate ROM image:
  ```bash
  subuwutuner-cli checksum-verify path/to/candidate.bin
  ```
- Repair the COBB-style aftermarket CRC slot table (AP-side
  metadata only):
  ```bash
  subuwutuner-cli checksum-repair path/to/candidate.bin
  ```

## Deeper detail

- [`docs/05-improvements.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/05-improvements.md){ target="_blank" } §4 — the brick-protection thesis.
- [`docs/31-brick-protection-by-isa.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/31-brick-protection-by-isa.md){ target="_blank" } — per-ISA recipes.
- [`docs/40-delta-flash-brick-protection.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/40-delta-flash-brick-protection.md){ target="_blank" } — v1.5 differential-flash extension.
- [`docs/42-bench-rig-validation-runbook.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/42-bench-rig-validation-runbook.md){ target="_blank" } — bench validation runbook.
- [`docs/43-jtag-recovery.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/43-jtag-recovery.md){ target="_blank" } — JTAG recovery procedure.
- [`docs/44-tune-export.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/44-tune-export.md){ target="_blank" } — tune-export pipeline spec.
