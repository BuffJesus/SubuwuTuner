# Definition format

The TOML schema that describes a calibration pack. This page is a
brief overview; the authoritative spec is
[`docs/11-definition-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/11-definition-format.md){ target="_blank" }.

## Top-level structure

A pack is one or more TOML files. The root file is `pack.toml`; sibling
files can be split out and joined via `[pack].includes` and `extends`.

```toml
[pack]
id       = "demo-pack"
version  = "0.1.0"
extends  = "base/lf79103p.toml"   # optional inheritance
includes = ["../pids.toml"]       # optional fragment merge

# Bytes that identify a ROM as matching this pack
[[identification]]
address = 0x100
bytes   = "AB CD EF 01"
cid     = "LF79103P"

# Raw -> engineering-unit transforms
[[scaling]]
name    = "afr_target"
expr    = "raw * 0.1"
inverse = "scaled * 10"
unit    = "AFR"
min     = 10.0
max     = 20.0
format  = "%.2f"

# Axis (an array of values used as table row/col labels)
[[axis]]
name    = "rpm_axis_16"
address = 0x0F800
type    = "uint16_be"
length  = 16
scaling = "rpm_x1"

# Table (the actual map)
[[table]]
name        = "fuel_main"
address     = 0x10000
type        = "uint8"
rows        = 16
cols        = 16
scaling     = "afr_target"
x_axis      = "rpm_axis_16"
y_axis      = "map_axis_16"
description = "Primary AFR target lookup"
policy_flags = ["emissions:euro_5"]
```

## Sections

| Section | Purpose |
|---|---|
| `[pack]` | Pack metadata, includes, extends |
| `[[identification]]` | Per-CID byte-pattern match records |
| `[[scaling]]` | Raw → scaled transform, inverse, unit, format, bounds |
| `[[axis]]` | Axis array description (address, type, length, scaling) |
| `[[table]]` | The actual table (address, dimensions, type, scaling, axes) |
| `[[dtc_bitmap]]` | DTC enable/disable bitmap addresses |
| `[[workflow]]` | Registered tuning workflows (e.g., FA24 swap) |

## Types

| `type` | Bytes | Endianness |
|---|---|---|
| `uint8` / `int8` | 1 | — |
| `uint16_be` / `uint16_le` / `int16_be` / `int16_le` | 2 | as named |
| `uint32_be` / `uint32_le` / `int32_be` / `int32_le` | 4 | as named |
| `float32_be` / `float32_le` | 4 | as named |

## Inheritance via `extends`

A pack can `extends` another pack. The child's records override or
extend the parent's. Idiomatic for VA/VB packs that share most tables
across CIDs but differ on a few.

```toml
[pack]
id      = "wrx-2017-usdm-mt"
extends = "lf79103p-base.toml"

# Override a single table's address for this CID
[[table]]
name    = "fuel_main"
address = 0x10080   # parent had 0x10000
```

## Linting

```bash
subuwutuner-cli pack-lint path/to/pack-dir/
```

Checks: dangling references (scaling on a missing axis), identification
overlap, axis monotonicity, address range overlap, missing inverse on a
write-target scaling, etc.

## Deeper detail

- [`docs/11-definition-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/11-definition-format.md){ target="_blank" }
  — full spec, including the workflow registry, DTC bitmaps, and
  policy-flag taxonomy.
- [Concepts → Definition packs](../concepts/definition-packs.md) —
  conceptual overview.
