# Brick protection

`st::flash` is treated as safety-critical. The brick-protection
subsystem is what makes that claim true rather than aspirational.

## The threat model

A flash that fails partway through can leave an ECU in any of several
broken states:

- Mid-erase, with a sector that the boot chain needs left blank
- Mid-write of a non-signature sector (often recoverable)
- Mid-write of a signature sector (bricked)
- Cleanly written but with a broken integrity contract (bricked at next start)

The safety model is **per-ISA**, because the recovery primitives the
ECU offers depend on its boot chain. SH-2A single-bank serial-boot
parts (FA20DIT) behave differently from RH850 dual-bank parts.

## Per-ISA recipes

| ISA | Recovery model |
|---|---|
| **SH-2A 2 MB** (FA20DIT) | Single-bank with a multi-signature boot integrity contract. Mid-flash recovery requires an exact-board, vendor-supported boot/JTAG path or qualified recovery service; the historical E2-Lite recipe is not validated for the current VA SH72543-style ECU. |
| **RH850** (newer Subarus) | Dual-bank atomic swap. The inactive bank is written and verified before the swap commit. Mid-write of the inactive bank is recoverable by re-trying. |

Per-ISA addresses, sector maps, and the exact contract live in the
spec docs — see "Deeper detail" below.

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
- **Policy gate.** Sector ranges flagged as bootloader, write-locked,
  or otherwise off-limits are rejected before any wire byte. The
  block-list is codec-level in `is_blocked_command`.
- **Audit subscriber.** Every Flasher state transition fires into the
  `st::audit` append-only log.
- **Optional bulk-transfer path.** A faster write path that's
  deliberately gated behind two CMake flags + a CLI flag. Off by
  default; the trade-off is documented in
  [`docs/26-bulk-reflash-cipher.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/26-bulk-reflash-cipher.md){ target="_blank" }.

## The tune-export pipeline

For LF79xxxP ECUs (SH-2A FA20DIT family), the boot integrity contract
constrains what a calibration write may touch. `st::tune_export` is
the module that translates a tuned working ROM into a write plan that
respects the contract — verifying the candidate image, skipping
write-locked regions, and emitting compensating writes where the
contract requires them.

The contract specifics — sum target, signature addresses, write-lock
ranges — are part of the per-ISA spec. The module is the contract's
authoritative implementation; the user-facing CLI surface is
[`workflows/flashing.md`](../workflows/flashing.md). Spec:
[`docs/44-tune-export.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/44-tune-export.md){ target="_blank" }.

## When things go wrong: JTAG recovery

If a flash leaves an ECU bricked despite the safety subsystem,
[`docs/43-jtag-recovery.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/43-jtag-recovery.md){ target="_blank" }
contains a quarantined historical recovery reference and the hard identity
gate for selecting a probe. It is not a universal E2-Lite/14-pin procedure;
use a qualified service or exact-board vendor-supported path until the MCU,
board revision, debug interface, and target support are confirmed.

## What you can do today

- Inspect what `st::flash` would write without flashing:
  ```bash
  subuwutuner-cli project-flash my-tune.stune --dry-run
  ```
- Verify the boot-integrity contract on a candidate ROM image:
  ```bash
  subuwutuner-cli checksum-verify path/to/candidate.bin
  ```
- Repair the aftermarket CRC slot table (cosmetic / tool-side
  metadata only — not boot-integrity affecting):
  ```bash
  subuwutuner-cli checksum-repair path/to/candidate.bin
  ```

## Methodology

Per-ISA contract specifics were recovered through clean-room analyst
work on OEM firmware (boot ROM behavior, observable failure modes
from bench-rig writes, public Renesas FCU documentation). **No
commercial-tool source was decompiled, read, or referenced** —
methodology in
[`docs/15-clean-room-engineering.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" }
and [Contributing → Clean-room methodology](../contributing/clean-room.md).

## Deeper detail

- [`docs/05-improvements.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/05-improvements.md){ target="_blank" } §4 — the brick-protection thesis.
- [`docs/31-brick-protection-by-isa.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/31-brick-protection-by-isa.md){ target="_blank" } — per-ISA recipes (addresses, sectors, contract specifics).
- [`docs/40-delta-flash-brick-protection.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/40-delta-flash-brick-protection.md){ target="_blank" } — v1.5 differential-flash extension.
- [`docs/42-bench-rig-validation-runbook.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/42-bench-rig-validation-runbook.md){ target="_blank" } — bench validation runbook.
- [`docs/43-jtag-recovery.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/43-jtag-recovery.md){ target="_blank" } — JTAG recovery procedure.
- [`docs/44-tune-export.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/44-tune-export.md){ target="_blank" } — tune-export pipeline spec.
