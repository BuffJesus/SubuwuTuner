# Security Access

UDS service `0x27` (SecurityAccess) is the seed-key challenge an ECU
runs before it lets you write flash. Subaru ECUs come in several
flavors depending on what's been installed:

| State | SA variant | CLI flag |
|---|---|---|
| Factory-stock | SSMCAN1 Gen-A 16-round Feistel | `--sa-variant ssmcan1-factory` (default) |
| COBB-installed (uninstalled state) | OTS-layer L1 swap (RK_L35 substitution) | `--sa-variant cobb-uninstalled` |
| COBB-installed (tuned state) | OTS layer + e-tune layer (L3) | `--sa-variant cobb-tuned` |
| Fehr-active (L1) | Forward direction with substituted keys | `--sa-variant fehr-active` |
| Fehr-active (L3) | Same mechanism as L1, L3 keys | `--sa-variant fehr-active-l3` |

## Picking a variant

You usually don't need to know the install state ahead of time —
`subuwutuner-cli` tries the requested variant, and if NRC 0x33 comes
back (security access denied / wrong seed), the CLI tells you which
variant to try next.

```bash
# Factory ECU
subuwutuner-cli rom-pull --authenticate \
                         --sa-variant ssmcan1-factory \
                         --transport obdx --device COM5 \
                         -o rom.bin

# COBB-tuned ECU
subuwutuner-cli rom-pull --authenticate \
                         --sa-variant cobb-tuned \
                         --transport obdx --device COM5 \
                         -o rom.bin

# Fehr-active L3
subuwutuner-cli ssm-a8-poll --authenticate \
                            --sa-variant fehr-active-l3 \
                            --transport obdx --device COM5 \
                            --did 0xF300
```

## Where the variants come from

All five variants in the tree were recovered through clean-room analyst
work — captured seed/key pairs from a live bus, dispatcher disassembly,
and pattern matching against the factory algorithm. No commercial-tool
source was decompiled. Methodology:
[`docs/15-clean-room-engineering.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" }.

The factory algorithm is a 16-round Feistel keyed by a fixed RAM nonce
read at `0xFFFFF220`. The aftermarket variants substitute different key
schedules and (in the Fehr case) reverse the iteration direction. All
variants share the same dispatcher.

## When SA is required

| Operation | SA required? |
|---|---|
| OBD-II Mode 01 / 09 (engine data, VIN, CIDs) | No |
| SSM A8 RAM polling | Depends on ECU state; COBB-tuned needs L3 |
| UDS RDBI / RMBA (data read) | No on most CIDs |
| UDS DSC 0x10 0x02 (Programming session) | **Yes**, L1 |
| UDS RequestDownload / TransferData / B6 | **Yes**, L1, plus Programming session |
| Routine Control (0x31) commit | **Yes**, L1 |

## Deeper detail

- [`docs/23-security-access.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/23-security-access.md){ target="_blank" }
  — full SecurityAccess design + plugin model.
- [`docs/38-subaru-sa-variants.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/38-subaru-sa-variants.md){ target="_blank" }
  — variant catalog cross-referenced to ECU install states.
