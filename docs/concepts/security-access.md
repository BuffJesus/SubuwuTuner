# Security Access

UDS service `0x27` (SecurityAccess) is the seed-key challenge an ECU
runs before it lets you write flash. Subaru ECUs may answer this
challenge differently depending on what's been written to them
previously — factory-fresh and post-aftermarket-tool ECUs do not
always negotiate the same way.

SubuwuTuner ships several **`--sa-variant`** options so you can pick
the algorithm flavor matching the ECU in front of you. You generally
don't need to know which one ahead of time — try the most likely,
and on NRC 0x33 (Security Access Denied) the CLI tells you which to
try next.

## Variants

| `--sa-variant` | When to try it |
|---|---|
| `ssmcan1-factory` *(default)* | OEM stock ECU |
| `cobb-uninstalled` | Aftermarket-touched ECU returned to "uninstalled" state |
| `cobb-tuned` | ECU currently running an aftermarket calibration |
| `fehr-active` | Variant observed on Cornelio's car; named for the discoverer |
| `fehr-active-l3` | Same variant, level-3 access |

## Picking one

```bash
# OEM-stock ECU
subuwutuner-cli rom-pull --authenticate \
                         --sa-variant ssmcan1-factory \
                         --transport obdx --device COM5 \
                         -o rom.bin

# Aftermarket-tuned ECU
subuwutuner-cli rom-pull --authenticate \
                         --sa-variant cobb-tuned \
                         --transport obdx --device COM5 \
                         -o rom.bin
```

If the chosen variant returns NRC 0x33, swap to the next likely
candidate — the CLI prints a hint with the most-likely next pick.

## When SA is required

| Operation | SA required? |
|---|---|
| OBD-II Mode 01 / 09 (engine data, VIN, CIDs) | No |
| SSM A8 RAM polling | Varies by ECU state |
| UDS RDBI / RMBA (data read) | Usually no |
| UDS DSC 0x10 0x02 (Programming session) | **Yes**, level 1 |
| UDS RequestDownload / TransferData | **Yes**, level 1, plus Programming session |
| Routine Control (0x31) commit | **Yes**, level 1 |

## Methodology

The variants in the tree were recovered through clean-room analyst
work: captured seed/key pairs from a live bus, behavioral diffing
against the factory algorithm, and pattern recovery from observable
inputs and outputs. **No commercial-tool source was decompiled, read,
or referenced** — the wall between analyst-mode sessions and the
implementer that wrote the C++ is documented in
[`docs/15-clean-room-engineering.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" }
and enforced through output isolation and tool restrictions.

The algorithmic detail (round counts, key schedules, dispatcher
shape) lives in the analyst-source design docs, not on this
user-facing page.

## Deeper detail

- [`docs/23-security-access.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/23-security-access.md){ target="_blank" }
  — SecurityAccess module design + plugin model.
- [`docs/38-subaru-sa-variants.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/38-subaru-sa-variants.md){ target="_blank" }
  — variant catalog cross-referenced to ECU install states.
- [Contributing → Clean-room methodology](../contributing/clean-room.md)
  — the wall and the boundaries.
