# 11 — Definition Format

The SubuwuTuner definition pack tells the tool how to read and edit a ROM:
where the calibration ID lives, where each table sits, how to scale raw bytes
into engineering units, and which maps are emissions-relevant.

This document is a working sketch. Anything here is fair game to change until
v1.0 freezes the format. Schema versioning lets us evolve later.

## Goals

- **Human-editable.** A community contributor with a text editor can add a
  newly-discovered map without recompiling anything.
- **Diffable.** Git can merge two contributors' definition changes the way it
  merges code. No binary blobs in version control.
- **Inheritable.** A 2020 WRX is 95% the same as a 2019 WRX; the format should
  let one definition extend another.
- **Validatable.** Every field has a type and a unit. Bad data fails to load
  with a useful error, not silently displays wrong numbers.
- **Stable.** A pack written for v1.0 still loads on v1.5 unless we bump the
  schema version on purpose.

## File layout

A definition pack is a directory:

```
my-va-wrx-mt-2019/
├── pack.toml                       (pack metadata + [[identification]] entries)
├── tables/
│   ├── fuel.toml
│   ├── ignition.toml
│   ├── boost.toml
│   ├── throttle.toml
│   └── ...
├── axes.toml                       (shared axis definitions)
├── scalings.toml                   (shared scaling formulas)
├── pids.toml                       (datalogger PIDs)
├── hooks.toml                      (custom-feature splice points — docs/16)
└── primitives.toml                 (custom-feature primitive declarations — docs/16)
```

Multiple TOML files keep diffs small. `Definition::from_directory` walks
the directory and merges every `*.toml`; ordering does not matter.
`[[identification]]` lives in `pack.toml` alongside `[pack]`. Per-table
`emissions_relevant` / `engine_safety_critical` booleans replace the
earlier-planned `emissions.toml` — the flag travels with the table that
owns it.

## `pack.toml` — metadata

```toml
[pack]
schema_version = 1
id              = "va-wrx-mt-2019"
display_name    = "Subaru WRX 2015–2021 (VA, MT)"
platform        = "subaru.va.wrx"
transmission    = "manual"
years           = [2015, 2016, 2017, 2018, 2019, 2020, 2021]
endianness      = "big"           # Subaru ECUs are big-endian
rom_size_bytes  = 1572864         # 1.5 MB typical
checksum_type   = "subaru_std"    # see below; Phase 4 uses this for post-write repair
authors         = ["The SubuwuTuner Authors"]
data_sources    = [
    "Derived from RomRaider XML via tools/defgen (facts only, no code copied — see docs/01-reverse-engineering.md)",
    "Manual research on owner-supplied stock dumps",
]
license         = "Apache-2.0"

# Pack signing (`[pack.signature]` with `public_key` + `signature`) is
# a future concern — neither the loader nor the schema validate it
# today. Lands with the signed-update channel (docs/03 §"reserved for
# future").
```

### `checksum_type` values

Post-write checksum repair is a Phase 4 concern, but the pack should already
carry the selector so `defgen` can populate it from upstream data and we don't
have to revisit every pack later. Mirrors RomRaider's `maps/checksum/` family
(`ChecksumSTD`, `ChecksumALT`, `ChecksumALT2`, `ChecksumCOPY`, `ChecksumBYTEXOR`,
…) and ECUFlash's `<checksummodule>` strings (`subarudbw`, …):

| `checksum_type` | Meaning |
|---|---|
| `subaru_std`  | Standard Subaru SH7058 / 68HC16Y5 checksum (the common case). |
| `subaru_alt`  | Alternate variant on some 16-bit ROMs. |
| `subaru_alt2` | Alternate variant used by some 32-bit ROMs. |
| `none`        | No checksum repair needed (placeholder for ROMs that don't validate). |

`st::flash::make_checksum_repair` resolves this string to an
`IChecksumRepair` instance via the factory; `apply_checksum_repair(span,
def)` is the one-call wrapper. The three concrete kinds
(`subaru_std`/`subaru_alt`/`subaru_alt2`) stub-return `NotImplemented`
with citation pointers to RomRaider's `ChecksumSTD` family — algorithm
implementation waits on a known-good stock dump for byte validation
(see docs/04 §"Phase 4").

## `[[identification]]` — CID detection

Lives in `pack.toml` alongside `[pack]` (or in any other `*.toml` in the
pack dir — the loader merges everything).

### Fixed-offset mode (default — EJ-era convention)

EJ-engine Subaru ROMs (Impreza, Forester, Legacy, Outback, Baja, Tribeca,
Exiga) put the calibration ID at a stable offset, typically `0x00002000`.
For those packs the natural shape is a byte-exact compare at a known
address:

```toml
[[identification]]
# How to tell whether a given ROM matches this pack.
name        = "VA-WRX-MT 2019 (CID AS80U)"
cid_address = 0x00002000
cid_length  = 8
cid_match   = "AS80U   "          # ASCII, exact match (trailing spaces ok)
ecu_part    = "22765-XXXXX"       # optional, informational
```

### Scan mode (`cid_scan = true` — FA-DIT WRX / VB convention)

FA-DIT WRX firmware (`LF*`, `LV*`, `LH*`, `AF*`, `AE*` prefix CIDs) embeds
the CID in a descriptor block at a **variable per-firmware offset**.
Two empirical data points from clean plaintext dumps: `LF75300E` lives at
`0x0002F7DD`, `LF9C000C` at `0x00038035`. Same surrounding shape
(`\x00\x00 [letter] \x00 <CID 8 bytes> \x00\x00\x00\x00 2.0 [engine]`)
but no fixed address. Hardcoding `cid_address` per CID would mean a
fresh research step per firmware revision.

For those packs, set `cid_scan = true` instead. The loader searches the
entire ROM for an occurrence of `cid_match` and reports the discovered
offset back via `Definition::match_info` (surfaced by `rom-info --def`
as `Match: LF75300E @ 0x0002F7DD (scanned)`).

```toml
[[identification]]
name      = "LF75300E (FA-DIT WRX 2.0)"
cid_match = "LF75300E"
cid_scan  = true             # ignore cid_address; scan the whole ROM
ecu_part  = "22765-AA240"    # optional, informational
```

`cid_address` is ignored when `cid_scan = true`. `cid_length` defaults
to `len(cid_match)` if omitted.

Multiple `[[identification]]` blocks let one pack cover several near-identical
CIDs (e.g. the same model year in different markets). The CLI's `rom-info`
prints the first matching CID with its discovered offset; `rom-identify
--pack-dir <dir>` scans a pack collection for matches.

## `axes.toml` — shared axis definitions

```toml
[[axis]]
id        = "rpm_16"
name      = "Engine speed"
unit      = "rpm"
type      = "static"               # values live in the ROM at a known offset
address   = 0x00040000
length    = 16
data_type = "uint16_be"
scaling   = "rpm_x1"

[[axis]]
id        = "load_16"
name      = "Engine load"
unit      = "g/rev"
type      = "static"
address   = 0x00040040
length    = 16
data_type = "uint16_be"
scaling   = "load_x0_001"
```

## `scalings.toml` — shared scaling formulas

Scalings convert raw bytes ↔ engineering units. Linear is enough for ~95% of
Subaru tables; we add piecewise for the rest.

```toml
[[scaling]]
id        = "rpm_x1"
formula   = "linear"
factor    = 1.0
offset    = 0.0
unit      = "rpm"
min       = 0
max       = 9000
precision = 0
data_type = "uint16_be"

[[scaling]]
id        = "afr_x0_125"
formula   = "linear"
factor    = 0.125
offset    = 0.0
unit      = "AFR"
min       = 8.0
max       = 22.0
precision = 2
data_type = "uint8"

[[scaling]]
id        = "iat_celsius"
formula   = "linear"
factor    = 1.0
offset    = -40.0
unit      = "°C"
min       = -40
max       = 215
precision = 0
data_type = "uint8"

[[scaling]]
# Piecewise table when the ECU stores a lookup, not a formula.
id          = "ego_target_lambda"
formula     = "piecewise"
data_type   = "uint8"
breakpoints = [0,    32,   64,   96,   128,  160,  192,  224,  255]
values      = [0.70, 0.78, 0.85, 0.92, 1.00, 1.07, 1.14, 1.21, 1.28]
unit        = "λ"
precision   = 2
```

## `tables/*.toml` — the calibration maps

```toml
[[table]]
id          = "primary_open_loop_fuel"
name        = "Primary open-loop fueling"
category    = "fuel"
dimensions  = 2
address     = 0x00050000
data_type   = "uint16_be"
scaling     = "afr_x0_125"
axis_x      = "rpm_16"             # column axis
axis_y      = "load_16"            # row axis
notes       = "Target AFR commanded under open-loop (high-load) conditions."
emissions_relevant = false
engine_safety_critical = true

[[table]]
id          = "boost_target_high_octane"
name        = "Boost target — high octane map"
category    = "boost"
dimensions  = 2
address     = 0x000540A0
data_type   = "uint16_be"
scaling     = "boost_kpa"
axis_x      = "rpm_16"
axis_y      = "throttle_8"
emissions_relevant = false
engine_safety_critical = true

[[table]]
id          = "egr_duty"
name        = "EGR valve duty cycle"
category    = "emissions"
dimensions  = 2
address     = 0x00060000
data_type   = "uint8"
scaling     = "percent_x1"
axis_x      = "rpm_16"
axis_y      = "load_16"
emissions_relevant = true
```

Notes on the schema choices:

- `dimensions` = 1, 2, or 3. A 1D table has only `axis_x`; a 3D table adds `axis_z`.
- `axis_x` / `axis_y` reference `id` values defined in `axes.toml`. The loader resolves them.
- `scaling` references an `id` from `scalings.toml`.
- `emissions_relevant` drives the `EmissionsLinter` jurisdiction-profile UI (see `06-legal-ethics.md`). Default `false`.
- `engine_safety_critical` is **always** consulted regardless of jurisdiction profile. Editing one of these maps triggers the dangerous-tune linter.

## `dtcs.toml` — diagnostic-code enable bitmaps

Most modern ECUs gate each DTC behind a bit in an enable bitmap somewhere in
the calibration. Zeroing the bit disables the code: the ECU's monitoring
logic still runs (and may still set internal flags), but the cluster's MIL
stays off and the code doesn't surface to a scanner.

This pairs with EGR/TGV/cat-equipment edits — see `06-legal-ethics.md` for
the jurisdiction posture and how the linter treats `emissions_relevant`
DTCs.

```toml
[[dtc_bitmap]]
# Where the bitmap lives in the ROM and how big it is. Real ECUs may have
# multiple bitmaps (primary / pending / permanent storage); each gets its
# own entry.
id           = "primary_dtc_enable"
name         = "Primary DTC enable bitmap"
address      = 0x00100000
length_bytes = 32
endianness   = "big"   # bit ordering within a byte; "big" = MSB-first

[[dtc]]
code        = "P0401"
name        = "Insufficient EGR Flow Detected"
bitmap_id   = "primary_dtc_enable"
byte_offset = 12        # byte within the bitmap
bit         = 3         # bit within that byte (0 = LSB, 7 = MSB)
emissions_relevant = true

[[dtc]]
code        = "P0420"
name        = "Catalyst System Efficiency Below Threshold (Bank 1)"
bitmap_id   = "primary_dtc_enable"
byte_offset = 14
bit         = 0
emissions_relevant = true
```

The loader validates that every `[[dtc]]`'s `(byte_offset, bit)` falls within
its bitmap's declared `length_bytes`. The `emissions_relevant` flag drives
the jurisdiction-profile linter exactly like it does on tables.

The companion CLI is `project-disable-dtc` / `project-enable-dtc`, which
read the bitmap, flip the named bit, and write back through the same
infrastructure tables use. See `04-roadmap.md`.

## `pids.toml` — datalogger parameters

```toml
[[pid]]
id            = "rpm"
name          = "Engine speed"
ssm_address   = 0x000008
length        = 2
data_type     = "uint16_be"
scaling       = "rpm_x1"
unit          = "rpm"
default_log   = true                # included in the default gauge cluster

[[pid]]
id            = "iat"
name          = "Intake air temperature"
ssm_address   = 0x000012
length        = 1
data_type     = "uint8"
scaling       = "iat_celsius"
unit          = "°C"
default_log   = true
```

## `[[writable_region]]` — codegen address gate

```toml
[[writable_region]]
name        = "calibration_a"
kind        = "calibration"    # calibration | code | data
address     = 0x000F0000
length      = 0x00010000
bank        = "a"              # optional — RH850 dual-bank tag (v1.3+)
description = "Main cal table area, EEPROM bank A"   # optional
```

Pack-level array of flash address ranges that `st::feature::codegen` is
allowed to target. Per `docs/16` §Safety #6, the codegen address gate
(`gate_patch`) refuses to emit a `PatchObject` whose per-hook
`[splice_address, splice_address + code.size())` range is not fully
contained inside a *single* `[[writable_region]]`. Spanning two regions is
rejected even when the union covers the range — bridging two declared-safe
zones is exactly the foot-gun the gate exists to prevent.

**Field semantics:**

- `name` (required, unique within the pack) — short identifier; appears in
  gate-rejection messages so authors can fix the splice point by reference.
- `kind` (required, one of `calibration | code | data`) — informational for
  v1.0. The gate checks containment regardless of `kind`. Reserved for
  future per-kind policy (e.g. production-fleet profiles refusing patches
  into `code` regions); declaring it now means the policy can land without
  a schema break.
- `address` (required, unsigned integer) — first byte of the writable range
  in firmware address space.
- `length` (required, positive integer) — bytes. `address + length` must
  not overflow `size_t`.
- `bank` (optional) — for RH850 dual-bank platforms (`a` / `b`). v1.0
  ignores it; v1.3+ recovery design consumes it.
- `description` (optional) — human note, surfaced in `pack-info` output.

**Fail-closed behavior.** A pack with zero `[[writable_region]]` entries
rejects *every* non-empty patch. This is a security boundary, not a sanity
check: silent fail-open would let a buggy or malicious `.stmod` graph reach
`st::flash` with an arbitrary target address through the same pipeline
table edits use. An empty `PatchObject` (no hooks) passes the gate
vacuously — matches the "empty patch = no-op flash" semantic the SH-2A
backend produces for an empty module.

**Scope.** The gate is `feature_codegen`-only; `st::flash::execute` is
unaffected. Table edits flow through `edit::History` and are bounded by
`table.address + length` at edit time, so they don't need the gate.

## Inheritance

```toml
# definitions/va-wrx-mt-2020/pack.toml
[pack]
schema_version = 1
id             = "va-wrx-mt-2020"
extends        = "va-wrx-mt-2019"      # inherit everything

# Anything declared here overrides the 2019 pack. Pure additions are common;
# overrides should be rare.
```

The loader merges by `id`: a table, scaling, axis, or pid with the same `id`
in the child replaces the parent's version entirely. New `id`s are added.
`[[identification]]` entries have no `id` field — child's are appended to the
parent's so a packaged child can match additional CIDs without obsoleting the
parent's matches. The child's `[pack]` header wins entirely; `extends` is
consumed and does not propagate.

Resolution: `extends = "va-wrx-mt-2019"` is resolved by scanning sibling
directories of the child pack for one whose `pack.toml` declares
`[pack].id = "va-wrx-mt-2019"`. The convention is therefore one pack per
directory under a common root. Chains of any depth are supported; cycles
are detected at load time and rejected.

Only directory-loaded packs resolve inheritance. `Definition::from_toml_string`
and single-file `Definition::from_file` leave `extends` on the `Pack` struct
unresolved — there is no filesystem context to search.

## Schema version policy

- `schema_version = 1` is the current version.
- Additive changes (new optional fields) keep the same version.
- Removing or repurposing a field bumps the version. The loader rejects packs
  whose version it does not recognize, with a "please update SubuwuTuner"
  message — never silently misinterpret old data.

## Validation rules the loader enforces

- Every `axis_x` / `axis_y` / `axis_z` reference resolves to an existing axis.
- Every `scaling` reference resolves to an existing scaling.
- `address + length * sizeof(data_type) <= rom_size_bytes` for every table and axis.
- `dimensions == 1` ⇒ no `axis_y`; `dimensions == 2` ⇒ `axis_x` and `axis_y`; `dimensions == 3` ⇒ all three.
- `precision >= 0`, `min <= max` on all scalings.
- `emissions_relevant` and `engine_safety_critical` are booleans (no fuzzy "maybe").

Failures produce a structured `Error` listing every violation, not just the first.
