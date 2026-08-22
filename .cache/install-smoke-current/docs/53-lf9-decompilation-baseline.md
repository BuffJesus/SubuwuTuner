# 53 - LF9C/D/G/L decompilation baseline

The four supported late-VA WRX manual-transmission ROMs have exact plaintext
sources and complete Ghidra function indexes. This is a provenance and
coverage baseline, not a claim that every discovered function is understood.

| CID | Source bytes | SHA-256 | Functions | Named | Unnamed |
|---|---:|---|---:|---:|---:|
| LF9C102P | 2,072,576 | `9eaa074fd4a5e17faedc9e965431e98f3c6e392c33033d6a680c9d59dbe4d90a` | 7,101 | 226 | 6,875 |
| LF9D012H | 2,596,864 | `4c29204bc5cea2bb936c56f2a0fcc18fdb1d8e1fdbe9fa4ede2e444889adbaf5` | 7,276 | 220 | 7,056 |
| LF9G003T | 2,596,864 | `d165bec2b9fb33b7cbb00c8c9b1233bed8ce850a592465c25af70e42cf478b29` | 7,366 | 218 | 7,148 |
| LF9L000E | 2,596,864 | `b6642441d314706f84084ce45f46f1b6600b1efee23ed3c88e5ac1baeb846b82` | 7,385 | 220 | 7,165 |

All indexed functions are inside each source image's `0x6000`-based mapping.
The larger sizes printed in Ghidra index headers (6,266,880 bytes for LF9C and
6,791,168 for LF9D/G/L) are modeled address-space totals after the setup script
adds RAM and register blocks. They are not imported-file sizes.

Reproduce the distinction with:

```powershell
python tools/rom_decompile_baseline.py ROM.bin function_index_named.txt `
  --image-base 0x6000 --out baseline.json
```

## Remaining understanding work

The corrected byte-parity audit already establishes a useful relocation
baseline:

| Adjacent pair | Shared body hashes | Unique relocated bodies | Ambiguous hashes |
|---|---:|---:|---:|
| LF9C102P -> LF9D012H | 2,570 | 2,189 | 339 |
| LF9D012H -> LF9G003T | 2,700 | 2,322 | 347 |
| LF9G003T -> LF9L000E | 3,061 | 2,655 | 336 |

`rom_function_parity.py` maps Ghidra addresses through the supplied image
bases before hashing. For this family, pass `--left-base 0x6000
--right-base 0x6000`. Unique relocated bodies are strong candidates for name
projection; ambiguous hashes must be reviewed because small thunks and common
stubs can occur at multiple addresses.

The review-only projection ledger confirms that the existing adjacent-CID
analyst passes already named every uniquely matched body that has a meaningful
source name: each pair currently has zero eligible missing names and zero
semantic conflicts. One shared 352-byte interpolation routine uses an
address-suffixed name in each ROM; the ledger classifies these as equivalent
address variants with canonical name `interp_2d_ax_u32_u8_data_s`.

```powershell
python tools/rom_name_projection.py SOURCE.bin SOURCE_INDEX SOURCE_CID `
  TARGET.bin TARGET_INDEX TARGET_CID --source-base 0x6000 `
  --target-base 0x6000 --out projection.json --tsv-out projection.tsv
```

The output records both CIDs, both addresses, full-body SHA-256, match method,
and review status. It explicitly sets `automatic_application_authorized` to
false and excludes ambiguous duplicate-body hashes.

1. Compare function bodies across C/D/G/L using normalized signatures rather
   than assuming equal addresses imply equal code.
2. Promote names only from byte-identical bodies or reviewed structural
   matches, retaining the source CID and match method as provenance.
3. Reconstruct boot/checksum, scheduler, diagnostics, live-signal,
   authorization, chassis-I/O, and calibration-consumer seams.
4. Connect public definition tables to code references and classify every
   unresolved cluster separately from calibration-value variance.
5. Track named coverage as evidence grows; a Ghidra-generated `FUN_*` name
   counts as discovered but not understood.
