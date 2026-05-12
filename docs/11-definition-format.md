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
definitions/va-wrx-mt-2019/
├── pack.toml                       (pack metadata)
├── identification.toml             (CID detection rules)
├── tables/
│   ├── fuel.toml
│   ├── ignition.toml
│   ├── boost.toml
│   ├── throttle.toml
│   └── ...
├── axes.toml                       (shared axis definitions)
├── scalings.toml                   (shared scaling formulas)
├── pids.toml                       (datalogger PIDs)
└── emissions.toml                  (which addresses are emissions-relevant)
```

Multiple TOML files keep diffs small. The loader walks the directory and
concatenates everything; ordering does not matter.

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
authors         = ["The SubuwuTuner Authors"]
data_sources    = [
    "Derived from RomRaider XML via tools/defgen (facts only, no code copied — see docs/01-reverse-engineering.md)",
    "Manual research on owner-supplied stock dumps",
]
license         = "Apache-2.0"

[pack.signature]
# Optional: maintainer signature over the pack contents. Loader warns if absent.
public_key  = "..."
signature   = "..."
```

## `identification.toml` — CID detection

```toml
[[identification]]
# How to tell whether a given ROM matches this pack.
name        = "VA-WRX-MT 2019 (CID AS80U)"
cid_address = 0x00002000
cid_length  = 8
cid_match   = "AS80U   "          # ASCII, exact match (trailing spaces ok)
ecu_part    = "22765-XXXXX"       # optional, informational
```

Multiple `[[identification]]` blocks let one pack cover several near-identical
CIDs (e.g. the same model year in different markets).

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

The loader merges by `id`: a table or scaling with the same `id` in the child
replaces the parent's version entirely. New `id`s are added.

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
