# 56 - Findings rollup and product impact (2026-08-21)

This page records the useful conclusions from the 2026-08-21 findings sweep
and translates them into SubuwuTuner requirements. It is not a substitute for
the evidence. The source-of-truth reports are:

- `D:/Subuwu/findings/COMPLETE-SYSTEM-UNDERSTANDING-2026-08-21.md`
- `D:/Subuwu/findings/CORRECTIONS-LEDGER-2026-08-21.md`
- `D:/Subuwu/findings/corpus-decompile-2026-08-21/ROLLUP.md`
- `D:/Subuwu/findings/corpus-decompile-2026-08-21/canonical_inventory.tsv`
- `D:/Subuwu/findings/corpus-decompile-2026-08-21/canonical_overrides.tsv`
- `D:/Subuwu/findings/corpus-decompile-2026-08-21/source_quality_exclusions.tsv`

The findings tree is analyst-side evidence and is not distributed as product
data. Only independently stated facts with provenance may enter public code,
tests, or definition packs.

## Executive result

Yes, the findings contain several material advances. The most important are:

1. The corpus decompilation is now a usable, repaired inventory rather than a
   collection of nominally successful Ghidra jobs. Wrapped, incomplete,
   encrypted, and architecture-mismatched sources are explicitly identified.
2. The VB/RH850 checksum balance contract is recovered: a little-endian u16
   sum over `[0, 0x3F0000)` targets `0x5AA5`, with the balance word at
   `0x3EFF7E`; all 16 available VB images satisfy it.
3. The `0xB6` bulk-transfer cipher is confirmed from both SH-2A and RH850
   firmware as the same four-round Feistel construction and existing key. This
   closes the cipher question, but not safe-write or durable-commit validation.
4. The bench ECU's silent state now has a coherent boot-path explanation, and
   the former E2-Lite recovery recommendation is refuted for this SuperH target.
5. A shipping VA Wastegate Duty definition shape is unsafe: the declared 8x7
   shape conflicts with a byte-verified 14x17 firmware descriptor. Editing it
   must be blocked until the exact-CID pack is corrected and tested.

## Confidence and promotion rules

Use these labels when carrying a finding into a ticket, definition, or test:

| Label | Meaning | Product use |
|---|---|---|
| Byte-verified | Reproduced directly from exact ROM bytes or a corpus invariant | May seed a test or guarded implementation after clean-room review |
| Decompiled | Supported by architecture-correct decompilation with an exact source hash | May guide implementation; retain the source hash and address |
| Cross-corpus | Reproduced across a stated, canonical set | May support family rules only for that exact set |
| Inferred | Best explanation of several observations | Documentation and research queue only |
| Unknown | Evidence is absent or contradictory | Hard product/flash gate |

File dates are not authority. The corrections ledger documents several cases
where a newer generated artifact regressed behind older byte-verified work.

## Canonical corpus state

The first master index described 319 images and 62 family buckets. The repaired
living rollup now reports 71 represented family prefixes and 55 complete at its
first canonical coverage snapshot. These numbers answer different questions;
they must not be mixed. Product code should consume the canonical inventory,
not quote a bare corpus total.

Important repairs:

- `AV9D100E`, `EE5F300K`, and `EP5I200D` were `INFO` containers. Extracting
  their declared big-endian `MEMD` payloads changed them from misleading or
  tiny analyses into valid firmware analyses (7,123, 3,163, and 4,220
  functions respectively).
- `AID43593` is big-endian PowerPC at base `0x008C0000`, not SH-2A; it yields
  419 functions with the corrected import.
- `AID43528` is M32R at base `0xFF000000`, not SH-2A; a header-seeded pass
  recovered 1,271 functions.
- The original `EP5D202N` source was incomplete. The canonical alternate has
  3,070 functions; alternates for `EP5D202W` and `EP5D202X` have 3,114 and
  3,212.
- `EA1T001K`, `EE5K000N`, `EP5D202T`, and `EP5D700T` are held as source-quality
  failures. They are not decompiler failures and must not be used for semantic
  promotion.
- `EP5D600B` was valid; its initial result was lost in a duplicate-lane race.
  Its isolated canonical rerun contains 3,821 functions.

Operational consequence: every RE-derived fact must carry the canonical source
identity, architecture, image base, content hash, and any override/exclusion
record. Directory names such as `decrypted` or `plaintext` are not evidence
that bytes are executable plaintext.

## Flash, checksum, and recovery facts

### SH-2A A-series

- The 2 MiB runtime sum is u16 big-endian over `[0x6000, 0x200000)`, including
  balance word `0x1FFFFE`, and must equal `0x5AA5`.
- The 2.5 MiB A.3 form moves the end to `0x280000`, the balance word to
  `0x27FFFE`, and the trailer signature to `0x27FFF2`.
- Stock finalization is SID `0x37`. The `31 01 0202` / `0x319E` account came
  from a bench-donor or image-specific path and must not be described as the
  universal stock commit.
- SID `0xB7` observes a staging buffer, not durable array flash. It cannot be a
  post-write proof.
- The boot signature gate uses three u16 comparisons. On the reference 2 MiB
  image these include `0x5555` at `0x6000`, `0xAAAA` at `0x1FFFF2`, and the
  word at `0x6C` matching the word at `0x6010`.
- The observed silent bench state is consistent with the pass branch reaching
  a damaged application before normal CAN initialization. Silence alone does
  not prove a dead CPU.

### VB/RH850

- Checksum word order is little-endian.
- The verified window is `[0, 0x3F0000)`, target `0x5AA5`, balance word
  `0x3EFF7E`.
- This is byte-verified across 16 canonical VB ROMs, but ECU-side enforcement,
  bank-selection behavior, and post-write durability remain bench-gated.
- The findings do not justify describing RH850 as safely flashable merely
  because checksum repair is now known.

### Recovery

Renesas E2 Lite does not provide the required SuperH/H-UDI support for this
target, so the old E2-Lite recipe is retired. The evidence-supported order is:

1. preserve the failed state and perform non-invasive power-domain controls;
2. prefer an exact-CID replacement ECU with documented intake;
3. for research recovery, identify the MCU and board pads exactly;
4. investigate SH72543R SCI boot mode with a supported serial toolchain;
5. do not erase or write until read/identify support is proven on the exact
   device.

SCI boot mode remains an exact-board research path, not a product recovery
promise.

## SecurityAccess and transfer crypto

- Factory A-series SecurityAccess constants are byte-identical across the eight
  checked 2 MiB and 2.5 MiB CIDs.
- The aftermarket LF79101P sample changed both round-key tables and six bytes
  in the Feistel core. A model that says only constants changed, or only branch
  polarity changed, is incomplete.
- VB SecurityAccess uses AES-128 with a 16-byte challenge/response. Three
  family-constant keys were found across the 16 checked images, but only the
  flash-write role is confirmed.
- The `0xB6` payload cipher is the same four-round Feistel construction on both
  silicon families. This validates the existing algorithm/key implementation;
  it does not remove the write feature gate.

Do not publish recovered secret material in user-facing docs or definition
packs. Tests should use synthetic or policy-cleared vectors.

## Definition safety finding

The most urgent definition defect is Wastegate Duty on VA/LF79. Shipping inputs
declare a 7-cell X axis by 8-cell Y axis, while the firmware descriptor resolves
to a 14-cell torque axis by 17-cell RPM axis. The manually curated strict XML
omitted these tables because they failed its monotonicity filter; omission was
safer than the generated wrong shape.

Required behavior:

- quarantine affected tables from editing and tune export;
- add a linter rule that compares table dimensions, axis lengths, data span,
  and descriptor evidence independently;
- do not regenerate from the currently stale RomRaider input until it is fixed;
- add an exact-CID 14x17 fixture and read/edit/write round-trip test before
  re-enabling the table;
- treat every generated VA 2-D table shape as suspect until descriptor-backed.

Other corrected definition facts:

- `x_5_12` means `raw / 5.12`, not `raw * 5 / 12`;
- LF79103P table counts are artifact-specific (for example TOML, Atlas JSON,
  strict XML, and broader knowledge-base counts differ), so UI coverage must
  name the source instead of showing one unqualified total;
- FA20-to-FA24 portability is relocation by verified block, not a global
  offset. Shape mismatches are a stop condition.

## Product and implementation delta

| Area | Newly supportable | Still blocked |
|---|---|---|
| Corpus RE | Canonical inventory, substitutions, exclusions, architecture-correct baselines | Semantic review of many recovered functions |
| SH-2A checksum | 2 MiB and 2.5 MiB family-aware balance rules | Hardware durability and safe FCU sequencing |
| VB checksum | Offline verify/fixup implementation and 16-image regression corpus | ECU enforcement and bank-swap validation |
| `0xB6` crypto | Cross-silicon algorithm regression tests | Enabling writes by default |
| Recovery | Retire E2-Lite; focus exact-CID donor or exact-board SCI research | A validated recovery operation on the failed ECU |
| Definitions | Quarantine known-bad WGDC shape and add descriptor lint | Publishing corrected exact-CID tables without regression tests |

## Ordered work queue

1. Add family-aware checksum profiles for SH-2A 2 MiB, SH-2A 2.5 MiB, and
   RH850 4 MiB; reject unknown size/endian combinations.
2. Add all 16 canonical VB images as offline checksum regression cases without
   redistributing private ROM bytes.
3. Quarantine the VA Wastegate Duty entries and add descriptor/dimension lint.
4. Make the RE manifest consume `canonical_overrides.tsv` and
   `source_quality_exclusions.tsv`, failing closed on a noncanonical source.
5. Correct remaining docs that call `31 01 0202` the universal stock commit or
   imply that `0xB7` proves array-flash persistence.
6. Keep `0xB6` writes disabled until array-flash readback after power cycle,
   safe FCU sequencing, interruption behavior, and restore are demonstrated.
7. Use the replacement-ECU intake path for hardware progress; keep SCI recovery
   as a separate exact-board research project.

## Explicit non-conclusions

The findings do not prove that every Subaru image uses `0x5AA5`, every LHB-like
file is RH850 plaintext, every generated definition address is correct, or any
current flash path is production-safe. They also do not convert proprietary
Atlas implementation details into clean-room product source. Each promoted
fact remains scoped to its exact evidence set.
