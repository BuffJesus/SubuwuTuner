# `.stune` format

A `.stune` is a directory containing a project: source ROM, working
ROM, edit history, optional datalogs, and metadata. The on-disk layout
is intentionally diffable in git.

This page is a brief overview; the authoritative spec is
[`docs/21-stune-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/21-stune-format.md){ target="_blank" }.

## Directory layout

```
my-tune.stune/
├── project.toml      # project metadata + def-pack reference
├── source.bin        # original ROM (read-only after creation)
├── working.bin       # source + applied edits (cache; rederived on open)
├── edits.toml        # structured edit history (source of truth)
├── histories/        # per-ROM history (multi-ROM projects)
│   └── <rom_id>.toml
├── datalogs/         # optional, drop CSVs here
├── snapshots/        # named snapshots for branching (optional)
└── audit.log         # CRC32-protected append-only audit (optional)
```

## `project.toml`

```toml
[project]
id           = "my-tune"
created      = "2026-06-19T18:42:11Z"
schema       = "v1"
jurisdiction = "motorsport-only"

[def_pack]
path = "../packs/lf79103p/"

[[rom]]
id     = "primary"
source = "source.bin"
working = "working.bin"
edits  = "edits.toml"
```

## `edits.toml`

Each `[[edit]]` is one structured mutation. Replayed in order on
project open.

```toml
schema = "v1"

[[edit]]
op    = "scale_mul"
table = "fuel_main"
rect  = { row_start = 3, row_end = 6, col_start = 4, col_end = 9 }
value = 1.02
tag   = "user"
ts    = "2026-06-19T18:42:11Z"

[[edit]]
op    = "set_value"
table = "fuel_main"
cell  = { row = 7, col = 8 }
value = 14.5
tag   = "user"
ts    = "2026-06-19T18:43:02Z"

[[edit]]
op    = "byte_edit"
address = 0x10080
bytes   = "0A 14 1E 28"
tag     = "ptm_import"
ts      = "2026-06-19T18:50:00Z"
```

## Ops

| `op` | Payload | Notes |
|---|---|---|
| `set_value` | `cell`, `value` | Single-cell write through scaling |
| `scale_mul` | `rect`, `value` | Multiply selected cells by a factor |
| `offset_add` | `rect`, `value` | Add a delta |
| `smooth` | `rect`, `sigma` | Gaussian smoothing |
| `interpolate` | `rect`, anchors | Linear interpolation between anchors |
| `byte_edit` | `address`, `bytes` | Raw byte write (used by `ptm import`) |
| `paste` | `rect`, `data` | Paste a serialized block |
| `dtc_bitmap_toggle` | `dtc_id`, `enabled` | Enable / disable a DTC bit |

Full op catalog at
[`docs/21-stune-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/21-stune-format.md){ target="_blank" }.

## The audit log

Append-only with CRC32 per record. NDJSON-exportable:

```bash
subuwutuner-cli audit export --format ndjson audit.log > events.ndjson
```

Records every: ROM identification, pack load, edit applied, UDS request
sent, Flasher state transition, AP3 vault op, etc.

## Deeper detail

- [`docs/21-stune-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/21-stune-format.md){ target="_blank" }
  — full schema spec.
- [Concepts → .stune projects](../concepts/stune-projects.md) —
  conceptual overview.
