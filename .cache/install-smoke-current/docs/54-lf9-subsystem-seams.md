# 54 - LF9 subsystem seam reconstruction

This pass expands the exact-source and function-body baseline in `docs/53`
into direct call-edge evidence. It does not automatically rename functions.

## Direct-call inventory

| CID | Parsed call edges | Unnamed seam candidates | Multi-anchor candidates |
|---|---:|---:|---:|
| LF9C102P | 7,736 | 497 | 76 |
| LF9D012H | 7,899 | 499 | 76 |
| LF9G003T | 8,035 | 527 | 76 |
| LF9L000E | 8,126 | 522 | 77 |

`tools/rom_subsystem_seams.py` associates an unnamed function only when a
direct decompiler call edge connects it to an already named subsystem anchor.
It records the edge direction and anchor; it does not invent a function name.

Exact full-body relocation matches then cross-check those associations:

| Adjacent pair | Stable associations | Classification disagreements |
|---|---:|---:|
| LF9C102P -> LF9D012H | 171 | 0 |
| LF9D012H -> LF9G003T | 170 | 0 |
| LF9G003T -> LF9L000E | 172 | 0 |

The stable sets contain 51-52 diagnostics seams, four security/authorization
seams, two checksum/integrity seams, 7-9 calibration/interpolation seams, and
105-107 runtime/I/O seams per adjacent pair. No stable unnamed body is
directly adjacent to a currently named true boot/flash anchor. These are
subsystem associations, not complete semantic labels.

## First promoted semantic candidate

Proposed name: `sa_mark_response_pending`

| CID | Address | Size |
|---|---:|---:|
| LF9C102P | `0x98D1C` | 8 |
| LF9D012H | `0xA2B70` | 8 |
| LF9G003T | `0xAF494` | 8 |
| LF9L000E | `0xAF718` | 8 |

Full-body SHA-256:
`090206762730ec390b6434d2e1acb5cbefc70a624880e0120d6255dafc9fce48`.

Evidence:

1. The body is byte-identical and unique in every adjacent-CID comparison.
2. Each instance is called by `sa_level_key_handler_a`,
   `sa_level_key_handler_b`, and `sa_seed_send_handler`.
3. The body sets one byte to 1 and returns.
4. In the surrounding communication code, `sa_send_response` sets the same
   per-CID state byte before populating the response buffer.

This supports “mark response pending” more specifically than a generic
security-access association. The name remains a reviewed proposal until
applied through the analyst naming workflow with this document and body hash
as provenance.

## Additional reviewed semantic candidates

### `copy_bytes_u16`

| CID | Address |
|---|---:|
| LF9C102P | `0x991FA` |
| LF9D012H | `0xA304E` |
| LF9G003T | `0xAF972` |
| LF9L000E | `0xAFBF6` |

- Size: 22 bytes
- SHA-256: `baa67379e77885f44bfd2183c0c64b1f78662b95a1ac2eb9f65bb0075c10ffd1`
- Behavior: copies exactly `param_3` bytes from `param_1` to `param_2`
  using a 16-bit loop counter.
- Boundary: this is a generic utility used by SecurityAccess code, not itself
  an authorization decision.

### `sa_protocol_state_dispatch`

| CID | Address |
|---|---:|
| LF9C102P | `0x97F08` |
| LF9D012H | `0xA1D5C` |
| LF9G003T | `0xAE680` |
| LF9L000E | `0xAE904` |

- Size: 454 bytes
- SHA-256: `e90c2b87b753b463e00a43fc82fe086693bdb3c9c305f0e0adbb27552eee2eaf`
- Behavior: advances a request state, reads the request, validates the
  subfunction class, dispatches seed generation or key comparison, constructs
  positive/error responses, and advances the communication state.
- Direct named callees include `sa_read_request`, `sa_seed_generate_from_nonce`,
  `sa_key_verify_compare`, and `sa_send_response`.

### `sa_key_transform_dispatch`

| CID | Address |
|---|---:|
| LF9C102P | `0x992B0` |
| LF9D012H | `0xA3104` |
| LF9G003T | `0xAFA28` |
| LF9L000E | `0xAFCAC` |

- Size: 248 bytes
- SHA-256: `fbe70cdaac1543d1066253b42c1e8ae1ea2b255c79f18e14feac44f7b31e61ba`
- Behavior: transforms a four-byte input into a four-byte output. One branch
  performs a 19-round Feistel-style path; the other splits the block, rotates
  an eight-round schedule, applies the round function, and rejoins the halves.
- Direct named callees include `sa_feistel_round_f`,
  `sa_key_schedule_rotate`, `sa_block_split_halves`, and
  `sa_block_join_halves`.

These names describe visible control/data behavior. They do not publish secret
material, imply that generic immobilizer authorization is bypassed, or
authorize modification of the SecurityAccess implementation.

The four proposals are pinned in
`fixtures/re_evidence/lf9_security_semantics.json`. Verify every address and
full-body hash against exact ROM sources with:

```powershell
python tools/rom_semantic_evidence.py `
  fixtures/re_evidence/lf9_security_semantics.json --image-base 0x6000 `
  --rom LF9C102P=PATH_TO_LF9C_ROM --rom LF9D012H=PATH_TO_LF9D_ROM `
  --rom LF9G003T=PATH_TO_LF9G_ROM --rom LF9L000E=PATH_TO_LF9L_ROM
```

Any missing CID, out-of-range body, or byte drift fails verification.

## Integrity seam

Proposed name: `checksum8_table_validate_ff`

| CID | Address | Size |
|---|---:|---:|
| LF9C102P | `0xBF192` | 36 |
| LF9D012H | `0xC7546` | 36 |
| LF9G003T | `0xD4F86` | 36 |
| LF9L000E | `0xD56DA` | 36 |

Full-body SHA-256:
`c8ea67725c004c7c0bc48b95271666c724c8d2dade2f79681215f581f02e0967`.

The function invokes `checksum8_carry_fold`, selects a check byte from a
table using the current record index, and returns whether their sum equals
`0xFF`. This is generic record/frame integrity evidence, not ROM flash
checksum evidence. It is pinned separately in
`fixtures/re_evidence/lf9_integrity_semantics.json`.

A second stable integrity-adjacent body computes parity bits and writes
modeled registers at `0xFFFF800C/0xFFFF8010`. It remains unnamed until the
peripheral/register context is independently confirmed.

## Diagnostic maturation-record banks

The four existing `diag_monitor_mature_rec00` through `rec03` functions
expose a repeatable record layout:

- each record is 12 bytes;
- the status byte is at record offset `+2`;
- bit `0x01` starts maturation and bit `0x04` records that transition;
- an external state advances from 1 to 2 after the record value reaches its
  threshold.

`tools/rom_diag_record_candidates.py` recovers the record index from
`(status_offset - 2) / 12` and distinguishes multiple backing-pointer banks.
It validates all four existing names in every CID with zero inconsistencies
and produces 21 collision-free review candidates per ROM.

Exact-body parity gives the portability boundary:

| Adjacent pair | Stable inferred identities | Disagreements |
|---|---:|---:|
| LF9C102P -> LF9D012H | 19 | 0 |
| LF9D012H -> LF9G003T | 20 | 0 |
| LF9G003T -> LF9L000E | 20 | 0 |

Nineteen inferred identities are stable through the complete four-CID chain.
`diag_monitor_mature_bank02_rec08` changes between LF9C and LF9D, while
`diag_monitor_mature_bank02_rec10` changes at every adjacent generation.
Those two remain per-CID structural candidates; equal record layout does not
prove identical monitor semantics.

```powershell
python tools/rom_diag_record_candidates.py LF9G003T DECOMPILE_DIR `
  --out lf9g-diag-records.json

python tools/rom_diag_record_parity.py lf9g-diag-records.json `
  lf9l-diag-records.json lf9g-vs-lf9l-parity.json `
  --out lf9g-to-lf9l-diag-records.json
```

## Interpolation-adjacent math primitives

Three stable bodies adjacent to interpolation functions are generic integer
math primitives rather than calibration consumers:

| Proposed name | Size | LF9C | LF9D | LF9G | LF9L |
|---|---:|---:|---:|---:|---:|
| `udiv64_by_64_quotient32_sat` | 136 | `0x6C50` | `0x6C50` | `0x6C50` | `0x6C50` |
| `umul32_wide_split` | 90 | `0x6CDC` | `0x6CDC` | `0x6CDC` | `0x6CDC` |
| `udiv64_by_u32_wrapper` | 24 | `0x1E637C` | `0x1F330C` | `0x20964C` | `0x212A88` |

The first performs a two-word numerator/two-word denominator division and
saturates an overflowing quotient to `0xFFFFFFFF`. The second multiplies two
32-bit inputs and writes the high and low 32-bit halves. The third packages a
two-word dividend and invokes the normalized division dispatcher with a
32-bit divisor.

Their exact bodies are pinned in
`fixtures/re_evidence/lf9_math_semantics.json`. Their use by interpolation
code proves arithmetic infrastructure, not the purpose of any calibration
table.

## Generic two-dimensional interpolation

Three exact bodies form a reusable interpolation layer rather than
application-specific calibration consumers:

| Proposed name | Size | LF9C | LF9D | LF9G | LF9L |
|---|---:|---:|---:|---:|---:|
| `linear_interp_u16` | 66 | `0x6F54` | `0x6F54` | `0x6F1C` | `0x6F1C` |
| `interp2d_s16_table_u32_u16_axes` | 198 | `0x6778` | `0x6778` | `0x6778` | `0x6778` |
| `interp2d_s16_table_u16_axes` | 218 | `0x71D8` | `0x72BC` | `0x7284` | `0x7284` |

Their respective full-body SHA-256 values are
`d2539e96145280b27d46f832e250a598a12b0e34a37ad7d0456cce47afbcc014`,
`aac958d2dbe1dab9237247736893e655d91163d4483482d67f2f92ba02336f62`,
and `084056e9a481037452e5e6b62daf873ab0fb643ee4f6f85c2243ab9ed9e5ab7d`.

`linear_interp_u16` computes a linearly interpolated unsigned 16-bit result
between two knots, handles either increasing or decreasing output values,
and returns the first output unchanged when the input knots coincide.

The first 2D descriptor supplies a signed 16-bit data grid, an unsigned 32-bit X
axis, an unsigned 16-bit Y axis, the X-axis count, and the row width. The
routine clamps both inputs to their endpoint knots, locates the enclosing
cell, interpolates the two rows along Y, and interpolates those results along
X with `linear_interp_2pt`. Ghidra currently renders the function as `void`,
but the final interpolation call is in tail-result position; the proposed
name intentionally makes no return-type claim.

The second 2D routine performs the same clamp/search/two-stage interpolation
shape with unsigned 16-bit axes in both dimensions and uses
`linear_interp_u16` for all three interpolation operations. Ghidra likewise
renders its tail result as `void`, so no return type is asserted here.

The exact-byte evidence is pinned in
`fixtures/re_evidence/lf9_interpolation_semantics.json`. This identifies the
data shape and algorithm only. It does not identify the engineering units or
purpose of any table passed by its callers.

An exact-body-equivalent caller in the named mass-airflow processing path
passes one of these descriptors to the U32/U16-axis variant. The caller and
descriptor move, but the complete table shape and contents do not:

| CID | Caller | Descriptor | Data grid |
|---|---:|---:|---:|
| LF9C102P | `0x6F7C0` | `0x63428` | `0x62894` |
| LF9D012H | `0x71B58` | `0x656E8` | `0x645CC` |
| LF9G003T | `0x7E38C` | `0x71EFC` | `0x68510` |
| LF9L000E | `0x7E504` | `0x72064` | `0x68614` |

Every version has the same 10-entry U32 X axis
(`1536000, 3840000, 6912000, 9984000, 13056000, 16128000, 19200000,
22272000, 26624000, 30720000`), the same six-entry U16 Y axis
(`0, 12288, 24576, 36864, 49152, 61440`), and the same 60 signed-16 cells.
Every cell is `256`, so the map is structurally a neutral Q8 multiplier
(`1.0`) if interpreted with the surrounding fixed-point scale. This
interpretation is local to the MAF caller; it does not rename the generic
interpolation routine or establish axis units. It also shows that this
surface exists but remains stock-neutral throughout LF9C/D/G/L.

The descriptor can be reproduced with:

```powershell
python tools/rom_interp_descriptor.py LF9G003T_ROM.bin 0x71EFC `
  --image-base 0x6000
```

## Interpolation descriptor inventory

`tools/rom_interp_inventory.py` scans aligned ROM data for mapped descriptor
pointers, bounded axis counts, and strictly increasing axes. It validates all
targets through the typed descriptor decoder and hashes axes plus grid content
independently of their relocated addresses.

The U32/U16-axis scan finds exactly one descriptor in each CID: the MAF map
above. The U16/U16-axis scan finds six non-overlapping descriptors in every
CID. Those 24 instances collapse to three content hashes:

- Three 8-by-8 descriptors per CID share a constant grid value of `6425`.
- One 16-by-16 descriptor per CID is constant `256`, another neutral Q8
  surface.
- Two 4-by-8 descriptors per CID have identical nonconstant content, ranging
  from `100` to `1300` with 18 distinct values.

In LF9G the two 4-by-8 descriptors at `0x772DC` and `0x772EC` are referenced
directly from literal slots `0xB5604` and `0xB5640`. Their respective callers,
`0xB543C` and `0xB550E`, invoke the confirmed U16/U16 interpolator and update
different bits of one shared state byte. The descriptors deliberately alias
the same axes and grid, so they represent two consumers of one calibration
surface rather than two independent tables. The remaining descriptor pointers
occur in literal-pool regions that Ghidra does not currently recover as direct
calls; they remain structurally verified but semantically unnamed.

The shared LF9G state byte is `0xFFF89925`: `0xB543C` controls bit 1 and
`0xB550E` controls bit 0. This byte has three ROM literal aliases. The alias at
`0xB55DC` ties both interpolating routines to a contiguous state-machine block
from `0xB53C0` through `0xB558C`; the earlier aliases at `0xB50E0` and
`0xB5350` connect the same byte to additional debounce, threshold, and mode
logic beginning at `0xB4FBC`. Its adjacent byte `0xFFF89926` is referenced by
the same block and by several consumers elsewhere in the ROM. The local
literal pool also identifies scalar thresholds at `0x6888E`, `0x68890`,
`0x68892`, `0x68896`, `0x68898`, `0x6889A`, and `0x6889E`. Consequently the
two table lookups are now known to feed one larger enable/fault state machine,
not merely return display or logging values.

Neither function entry address appears anywhere in LF9G as a plain big-endian
32-bit pointer, despite Ghidra reporting one xref to every routine in this
block. Direct `calls:` edges to the two functions are also absent. Their owner
therefore remains encoded in scheduler/table metadata or in an instruction
reference that the current export does not preserve. The state-machine shape
narrows the role, but is still insufficient to choose among ignition phase,
valve timing, or another angle-dependent control, so no subsystem name is
assigned yet.

The shared 4-by-8 surface uses X knots
`14331, 19961, 25933, 30720` and Y knots
`4096, 6144, 8192, 12288, 16384, 57344, 61440, 63488`. Its rows are:

```text
1300  812  462  300  250  250  275  275
1100  688  362  200  150  150  175  175
1000  625  312  156  100  100  125  125
1000  625  312  156  100  100  125  125
```

These raw values are stable across C/D/G/L, but engineering-unit and scaling
labels remain withheld pending identification of the two caller inputs.

There is, however, a strong representation clue in the Y knots. If one full
unsigned-16 turn is `65536`, they convert exactly to `22.5, 33.75, 45, 67.5,
90, 315, 337.5, 348.75` degrees. The apparent jump from `16384` to `57344`
then becomes an ordinary circular wrap from 90 to 315 degrees rather than an
implausible linear sensor discontinuity. This supports an angle-domain
hypothesis for the second input, but does not distinguish crank, cam, phase,
or another cyclic coordinate. The axis remains unnamed until its producer
chain is tied to a known subsystem.

The two inputs are family-equivalent fields but not fixed RAM addresses:

| CID | First input | Second input | Second-input pre-transform field |
|---|---:|---:|---:|
| LF9C102P | `0xFFF88E16` | `0xFFF88E24` | `0xFFF88EC8` |
| LF9D012H | `0xFFF88F16` | `0xFFF88F24` | `0xFFF88FC8` |
| LF9G003T | `0xFFF890E6` | `0xFFF890F4` | `0xFFF89198` |
| LF9L000E | `0xFFF89276` | `0xFFF89284` | `0xFFF89328` |

Within each CID the second input is exactly 14 bytes after the first. The
whole field group relocates by `+0x100`, `+0x1D0`, and `+0x190` at the three
generation boundaries. This is direct evidence that semantic identity must
be projected through code/data references rather than assumed from absolute
RAM addresses.

The descriptor, both runtime inputs, and the affine pre-transform field are
pinned as exact big-endian pointer-slot evidence in
`fixtures/re_evidence/lf9_interp_runtime_pointers.json`. Verification is
independent of decompiler names:

```powershell
python tools/rom_pointer_evidence.py `
  fixtures/re_evidence/lf9_interp_runtime_pointers.json `
  --image-base 0x6000 --rom LF9C102P=LF9C_ROM.bin `
  --rom LF9D012H=LF9D_ROM.bin --rom LF9G003T=LF9G_ROM.bin `
  --rom LF9L000E=LF9L_ROM.bin
```

For LF9G, `tools/rom_symbol_usage.py` finds the first field in 13 functions with
21 total references and the second in 46 functions with 96 total references.
None of those consumer functions currently has a semantic name, and direct
assignments are hidden behind pointer aliases. The fanout supports treating
both as central shared runtime signals, but it does not yet justify sensor,
temperature, load, or speed labels for either axis.

The producer chain is recoverable for the second field in every CID. The
function currently named `sensor_read_scale_wordidx` occurs at `0x8214C`,
`0x84EB0`, `0x916FC`, and `0x91980` in C/D/G/L respectively. Closer inspection
shows that name is misleading: `0xFFFFB000` is used as the signed arithmetic
constant `-0x5000`, not dereferenced as a hardware base. The function computes
`(input * 8 - 0x5000) / 5`, clamps the result to `0..65535`, and stores it to
the second field. Its corrected evidence name is
`affine_u16_x8_minus_0x5000_div5_sat`.

In LF9G, adjacent routines at `0x91594` and `0x9166C` retain previous values,
check range and delta thresholds, filter accepted samples, and install
`0x9000` as a fallback when a status bit is set. The pre-transform field is
itself dynamically produced by `0x7D3BA` through lookup, conditioning, and
mode-dependent paths; it is not a constant channel selector. The exact affine
body is stable across all four ROMs, but this evidence still does not establish
the physical meaning of the quantity.

The acquisition body's full SHA-256 is
`d3d3e791ed0c89056d815fd6f361eb9a9f00214f991dd93e79ad3bb8d0b1a15e`
and is pinned with the interpolation evidence manifest. Its `clamp_u16`
dependency is also identical across the family, with SHA-256
`c9e9aff35f751d5b30c75c6cb4fa4672c8ce0c2a2fb9b58891fcd804b2924c11`.

Each function index also contains a six-byte entry carrying the old
`sensor_read_scale_wordidx` name (`0x83346`, `0x860AA`, `0x928F6`, and
`0x92B7A` in C/D/G/L). Those entries are thunks whose sole call targets the
34-byte implementation; they are not independent conversion functions.

The ROM contains 12 literal aliases for `0xFFF890E6` and 42 for
`0xFFF890F4`. Most are read-only consumers; producer discovery therefore has
to distinguish stores through aliases and output parameters from ordinary
literal references. No corresponding writer for `0xFFF890E6` has yet been
confirmed, so its axis remains wholly unnamed.

`tools/rom_pointer_aliases.py` connects those raw big-endian pointer literals
back to decompiler symbols and containing functions. For LF9G, 11 of the 12
`0xFFF890E6` literals and 32 of the 42 `0xFFF890F4` literals have recovered
decompiler references, producing 12 and 38 function-reference rows
respectively. Unreferenced literals may be dead pools, dynamically indexed
data, or analysis gaps and are not treated as producers.

```powershell
python tools/rom_pointer_aliases.py LF9G_ROM.bin LF9G_DECOMPILE_DIR `
  0xFFF890E6 0xFFF890F4 --image-base 0x6000 `
  --out lf9g-input-aliases.json
```

```powershell
python tools/rom_symbol_usage.py LF9G_DECOMPILE_DIR `
  DAT_fff890e6 DAT_fff890f4 --out lf9g-input-usage.json
```

This establishes a concrete calibration-format boundary without assigning
physical meanings to the U16/U16 surfaces. The repeated descriptor contents
also show that descriptor identity and table-content identity must be tracked
separately. A seventh pointer/count structure initially passed the monotonic
axis checks, but its claimed 8-by-8 grid overlapped its Y-axis storage; the
scanner now rejects all such overlapping regions.

```powershell
python tools/rom_interp_inventory.py LF9G003T_ROM.bin --image-base 0x6000 `
  --x-type u16 --out lf9g-interp-u16.json
```

## Reproduction

```powershell
python tools/rom_subsystem_seams.py LF9G003T function_index_named.txt `
  DECOMPILE_DIR --out lf9g-seams.json

python tools/rom_subsystem_seam_parity.py lf9g-seams.json lf9l-seams.json `
  lf9g-vs-lf9l-parity.json --out lf9g-to-lf9l-seams.json
```
