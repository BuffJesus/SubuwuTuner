# Tuning Knowledge Atlas

## Overview

The **Tuning Knowledge Atlas** is a single machine-readable TOML file that
consolidates everything the project has learned from its decrypted-`.ptm`
corpus about how Subaru ECUs are actually tuned in the wild. It bundles:

- **170 per-table records** — display name, ROM address, size, dimensions,
  units, narrative purpose, FA24 portability classification, and the
  observed clusters that touch each table
- **9 tuner-cluster fingerprints** — typical patch count, mean tables
  touched, narrative philosophy, signature tables, skipped tables
- **8 corpus-derived safety pairs** — the "raised X without touching Y"
  anti-patterns the corpus actually exhibits
- **3 ROM-offset anchors** — RE-pinned surface ranges (HPFP cluster, MAF
  Sensor Scale, FA24 displacement constant) that let any analyser map a
  raw `rom_offset` to a known surface even when the underlying def
  doesn't (yet) name the table

The atlas exists so that SubuwuTuner can carry corpus knowledge **in
its own binary**, the way it carries definition packs. Until this
landed, the same facts lived as prose scattered across `findings/`
markdown — useful for analysts, invisible to the runtime. The atlas
turns those facts into something the C++ library, the CLI, and (soon)
the GUI can all consume from a single load.

It is a **shipping artefact**, not an internal scratchpad: the TOML
ships in the repo under `fixtures/tuner_atlas/`, the loader has a
clean Result-typed API, and the schema is versioned.

## When to use the atlas vs the LF79103P def

These two things look adjacent but serve different jobs and **must not
be conflated**.

| | LF79103P def (`definitions/LF79103P.toml`) | Tuning Knowledge Atlas |
|---|---|---|
| **What it describes** | Structural facts: where each table lives, how its cells are laid out, what scaling converts raw -> physical | Corpus-derived facts: which tunes touched this table, what direction they moved it, what other tables co-edit |
| **Source of truth** | ECU firmware + factory documentation | 60 decrypted `.ptm` files + ROM diffs across the LF79103P variant set |
| **Required for tuning?** | Yes — without the def you cannot read or write a cell | No — purely advisory; SubuwuTuner can edit ROMs with the def alone |
| **Changes when?** | A new CID is added, or a table's address/scaling is corrected | Corpus is re-analysed, or a new tuner cluster is characterised |
| **Authoritative for cell width / address?** | Yes | No — atlas records are derived and may be stale |
| **Authoritative for "is this safe?"** | No | Advisory — see `safety_pairs` and `co_edits` |

**The atlas augments the def; it does not replace any part of it.** If
the atlas and the def disagree on a table's address, the def wins. The
atlas's `address` field is a convenience for offline tools that don't
want to load the full def — runtime code should resolve table identity
via the def, then enrich with the atlas record by id.

## Where it lives

```
fixtures/tuner_atlas/
  tuner_atlas.toml      # the atlas itself (schema v1)
```

The file is loaded at runtime by `st::library::Atlas::load_from_file`.
The CLI subcommand looks for it at this path by default; pass
`--atlas <path>` to override.

### Schema version model

```toml
[atlas]
schema_version = 2
```

The loader asserts `schema_version <= Atlas::schema_version_supported()`
and returns `Result<Atlas, UnsupportedVersion>` when a newer atlas is
fed to an older binary. New fields added in a backwards-compatible way
keep the current major; breaking changes (renames, type changes,
deletions) bump the major.

**v2 is current.** The current shipped atlas (`fixtures/tuner_atlas/tuner_atlas.toml`)
is schema v2 — multi-CID-aware. The v2 loader is a strict superset of
the v1 loader: both v1 and v2 atlases parse identically, and a v2
atlas without any `[[table.cid_address]]` sub-tables is semantically
identical to a v1 atlas. The archived v1 atlas (used by the
forward-compat integration test) lives at
`fixtures/tuner_atlas/v1.archived.toml`.

**v1 was single-CID** (LF79103P / FA20DIT only). v2 adds:

- An optional per-table `primary_cid` field declaring which CID this
  record's `address` / `size_bytes` apply to (defaults to the
  atlas-level `primary_cid` for forward-compat with v1)
- An optional `[[table.cid_address]]` sub-array binding the same logical
  table to additional CIDs at potentially different addresses / sizes
- A top-level `[[cid_coverage]]` summary listing every CID the atlas
  knows about, with table-counts and a free-form note (e.g. "not
  covered: RH850 Gen-B uses RAM-mapped pointers")

## Schema

The atlas TOML has four collection sections plus a header. Every
collection uses `[[...]]` arrays-of-tables; the loader iterates them
in document order.

### `[atlas]` header

```toml
[atlas]
schema_version    = 2
generated         = "2026-06-13"
platform          = "subaru.va.wrx.fa20dit"
primary_cid       = "LF79103P"
table_count       = 170
tuner_count       = 9
safety_pair_count = 8
anchor_count      = 3
cid_coverage_count = 24   # v2 only
```

The counts are advisory — they exist to let a hand-validator confirm
the file isn't truncated. The C++ loader recomputes them from the
parsed collections; the canonical counts come from `tables().size()`
etc.

### `[[table]]` — per-table records

Each record describes one ECU calibration table the corpus has touched.
`id` is the slug of the display name (lowercase, non-alphanumerics
collapsed to `_`).

```toml
[[table]]
id              = "airflow_turbo_boost_boost_target_main"
display_name    = "Airflow - Turbo - Boost - Boost Target Main"
address         = 202856        # 0x31868
size_bytes      = 608
dimension       = "3d"          # "1d" / "2d" / "3d" / ""
storage         = "uint16"      # "uint8" / "uint16" / "float" / ""
units           = ""
purpose         = "The headline boost-target table: requested manifold pressure (or load proxy) versus RPM and gear/throttle. This is the most-modified surface in any performance tune."
fa24_portability = "needs_retune"  # "portable" / "needs_retune" / "fa20_specific" / "unclear"
needs_def_promotion = true
common_core     = true
high_variance   = true
clusters        = [
  "COBB Stage 1 +Redline/+SF (byte-identical)",
  "NexGen Stage 2 +BigSF",
  "Fehr/DMann WRK1/2/3 ETune (user's car)",
  # ...
]
co_edits        = [
  "Engine - Rev Limit (Fuel Cut) - Rev Limit C",
  "Fuel - Injectors - Pulse - Injector Offset Table",
  # ...
]
```

Field notes:

- `address` is decimal with a hex comment for readability. The hex is
  for humans; the loader reads the decimal.
- `needs_def_promotion = true` flags tables where the LF79103P def
  still has placeholder coverage (typically missing scaling or axis
  links). Useful for prioritising the def-promotion backlog.
- `common_core` means the table appears in **most** corpus clusters
  (i.e. tuners across all vendors edit it). `high_variance` means the
  cell-value distribution across the corpus is wide — both are inputs
  to the CLI's `--common-core` and ranking filters.
- `clusters` and `co_edits` are not exhaustive truth — they are
  whatever the analyser observed across the corpus. Absence of a
  cluster from a table's `clusters` list means **the corpus didn't
  show that tuner touching this table**, not "this tuner avoids it
  by design."

### `[[tuner]]` — per-cluster fingerprints

```toml
[[tuner]]
id                  = "cobb_stage1"
display_name        = "COBB Stage 1 (base)"
patches_typical     = 4228
mean_tables_touched = 87
philosophy          = "Performance-budget tune: top of the surface is Closed-Loop Lambda Target Base (TGV split, EGR split) and Lean Limit/Adder Alternate. Universal Stage signature: every Requested Torque B variant (Group 2, Group 2 Redundancy) is raised in lockstep. Doesn't retune intake AVCS cam targets — that's Felix territory."
signature_tables    = [
  "Boost Control - Boost Targets/Limits - Requested Torque",
  "Airflow - Turbo - Wastegate - Frequency - Wastegate PWM Cycle Time",
  # ... top-12 most-touched tables for this cluster
]
skipped_tables      = [
  "AVCS - Exhaust - Barometric Multiplier Low - Exhaust Cam Target (TGV Open)",
  # ... top-10 tables the cluster conspicuously does NOT touch
]
```

The 9 v1 clusters: `cobb_stage0`, `cobb_stage1`, `cobb_stage1_bigsf`,
`cobb_stage1_redline_sf`, `nexgen_stage2_bigsf`,
`nexgen_stage2_redline`, `fehr_wrk` (the user's installed tune),
`felix_on_fa24` (synthetic), and `ntm_fa24` (FA24 swap basemap).

### `[[safety_pair]]` — corpus-derived anti-patterns

Each safety pair codifies a "raised X without touching Y" pattern the
corpus actually exhibits — these are not hand-authored rules, they're
observations the safety-pair analyser pulled from the patch diffs.

```toml
[[safety_pair]]
id        = "boost_without_wastegate"
title     = "Raises boost target without touching wastegate duty"
severity  = "high"                       # "high" / "med" / "low"
lhs_patterns = [
  "Boost Target Main",
  "Boost Limit Base",
  "Boost Limit Adder",
]
rhs_patterns = [
  "Wastegate Duty Initial",
  "Wastegate Duty Maximum",
  "Wastegate PWM Duty Time",
  "Wastegate Barometric",
]
rationale = "Boost target/limit raised without adjusting wastegate duty — ECU will demand boost the actuator cannot reliably deliver (or, conversely, the existing duty schedule will overshoot the new target)."
```

`lhs_patterns` are display-name substrings that mark "the thing that
got raised"; `rhs_patterns` are substrings that mark "the thing that
should have moved with it." Consumers (e.g. `interpret_diff`) match
patches against both sides and flag rows where LHS fired but RHS
didn't. The substring match is intentional — display names are
hierarchical and the patterns target the meaningful prefix.

### `[[table.cid_address]]` — multi-CID address bindings (v2+)

A v2 `[[table]]` record may carry zero or more `[[table.cid_address]]`
sub-tables that bind the same logical calibration table to additional
CIDs. The top-level `address` / `size_bytes` fields on the parent
`[[table]]` still describe the binding for the table's `primary_cid`
(or the atlas-level `primary_cid` when the per-table field is absent).

```toml
[[table]]
id              = "airflow_turbo_wastegate_wastegate_duty_maximum"
display_name    = "Airflow - Turbo - Wastegate - Wastegate Duty Maximum"
address         = 202290       # 0x31632 on LF79103P
size_bytes      = 476
primary_cid     = "LF79103P"
# ...

[[table.cid_address]]
cid        = "LF75404S"
address    = 168106             # 0x292AA
size_bytes = 8670                # different table size on LF75404S

[[table.cid_address]]
cid        = "LF9C102P"
address    = 200096
size_bytes = 476
```

Lookup helper:

```cpp
auto pc = atlas.find_table_for_cid("airflow_turbo_wastegate_wastegate_duty_maximum",
                                    "LF75404S");
if (pc) {
    // pc->cid == "LF75404S", pc->address == 168106, pc->size_bytes == 8670
}
```

`find_table_for_cid` returns the primary binding (synthesised from the
parent table's `address` / `size_bytes`) when the requested CID is the
table's primary CID, otherwise looks up `cid_addresses`. Returns
`std::nullopt` when there is no binding.

Honest scope caveat: per-table `units`, `purpose`, `clusters`,
`co_edits`, `dimension`, `storage`, and `fa24_portability` are
**primary-CID-only** at v2. The cross-CID heatmap data that drives the
cid_address bindings doesn't carry per-CID unit/scaling information, so
v2 deliberately doesn't pretend to. A future v3 may grow per-CID metadata
once the def-promotion sweep catches up on the secondary CIDs.

### `[[cid_coverage]]` — atlas scope summary (v2+)

The top-level `[[cid_coverage]]` array advertises which CIDs the atlas
knows about, and is honest about which it does not:

```toml
[[cid_coverage]]
cid         = "LF79103P"
table_count = 170
note        = "primary CID (all atlas table records bind here by default)"

[[cid_coverage]]
cid         = "LF75404S"
table_count = 269
note        = ""

[[cid_coverage]]
cid         = "LHBH800B00G"
table_count = 0
note        = "not covered: RH850 Gen-B hybrid def storageaddress entries are RAM-mapped pointers (0xFEEFxxxx range), not flash offsets; needs a per-CID virtual-to-physical fixup before atlas can bind"
```

The v2 generator emits one row per CID seen in the cross-CID heatmap
(`heatmap/cross_cid_table_touch.csv`) plus one explicit "not covered"
row for each RH850 Gen-B CID. The 15 RH850 Gen-B CIDs are listed with
`table_count = 0` and a `note` explaining why — they are **out of
scope** for v2, not silently dropped.

### `[[anchor]]` — RE-pinned ROM-offset surfaces

```toml
[[anchor]]
id              = "hpfp_cluster"
display_name    = "HPFP cluster (FA24 cam-lobe phase compensation)"
rom_offset_start = 301976       # 0x49B98
rom_offset_end   = 301992       # 0x49BA8
length          = 16
prose           = "Patches HPFP cluster (rom 0x49B98..0x49BA8) — FA24 cam-lobe phase compensation."
```

Anchors are the escape hatch for the case where a `.ptm` patch lands
inside a region the def doesn't have a table for, but the RE notes
do. `anchor_for_offset(off)` returns the anchor whose
`[start, end)` covers `off`, so `interpret_inspect` can still emit
prose like "Patches MAF Sensor Scale at 0x30F64 (256B) — intake
recalibration" instead of "unknown ROM region."

v1 has three anchors: `hpfp_cluster`, `maf_sensor_scale`, and
`engine_displacement_constant`. The displacement-constant anchor uses
`rom_offset_start = rom_offset_end = 0` as a sentinel (the offset
varies per CID and isn't pinned at v1).

## How to consume

### C++ API

```cpp
#include "st/library/atlas.hpp"

using st::library::Atlas;

auto atlas_result = Atlas::load_from_file(
    "fixtures/tuner_atlas/tuner_atlas.toml");
if (!atlas_result) {
    // ParseError / UnsupportedVersion / IoFailure
    return atlas_result.error();
}
auto const &atlas = *atlas_result;

// Header
std::cout << "Atlas v" << atlas.schema_version()
          << " for " << atlas.primary_cid()
          << " (" << atlas.tables().size() << " tables)\n";

// Lookup by id
if (auto const *t = atlas.find_table(
        "airflow_turbo_boost_boost_target_main")) {
    std::cout << t->display_name << " @ 0x" << std::hex
              << t->address << " (" << t->size_bytes << "B)\n";
}

// Look up an anchor by raw ROM offset (for inspecting an unknown patch)
if (auto const *a = atlas.anchor_for_offset(0x49B9E)) {
    std::cout << a->prose << "\n";
    // "Patches HPFP cluster (rom 0x49B98..0x49BA8) — ..."
}
```

All accessors are `const noexcept`. Lookup returns `nullptr` for no
match — the API does not throw, and lookup is O(n) over the
collections (n ≤ 170; called once per inspect/diff row, not per
cell).

The atlas is a **read-only value type**. Hold it by `const &`. There
is no in-place editor — to change the atlas, regenerate it from the
KB (see *How to extend*).

### CLI examples

The `subuwutuner-cli tuner-atlas` subcommand exposes the atlas to
the command line. (Being added in parallel; see the CLI help when in
doubt.)

```bash
# Header counts + schema version
subuwutuner-cli tuner-atlas stats

# Dump one table by id
subuwutuner-cli tuner-atlas show-table airflow_turbo_boost_boost_target_main

# List all 170 tables sorted by address
subuwutuner-cli tuner-atlas list-tables

# Filter to only common-core tables (touched by most clusters)
subuwutuner-cli tuner-atlas list-tables --common-core

# Filter to tables flagged for def-promotion backlog
subuwutuner-cli tuner-atlas list-tables --needs-def-promotion

# Show one tuner cluster's fingerprint
subuwutuner-cli tuner-atlas show-tuner fehr_wrk

# All safety pairs, highest-severity first
subuwutuner-cli tuner-atlas list-safety-pairs

# All anchors with their offset ranges
subuwutuner-cli tuner-atlas list-anchors

# Resolve a raw ROM offset against the anchor set
subuwutuner-cli tuner-atlas resolve-offset 0x49B9E
```

All subcommands accept `--format text|json|toml` for scripted
consumption. The default format is text; JSON is stable enough to
drive an external GUI or a CI check.

## How to extend

The TOML is **generated, not hand-edited**. The source of truth is
the markdown knowledge base at:

```
findings/tuning-knowledge-2026-06-13/knowledge-base/
  tables/             # 170 per-table pages
  tuners/             # 9 cluster pages
  README.md           # ranking + factory-immutable list
findings/tuning-knowledge-2026-06-13/risk/
  safety_pairs.md     # 8 codified pairs
```

To extend or correct the atlas:

1. **Edit the markdown KB.** Update the affected `tables/<slug>.md`
   page, `tuners/<id>.md` page, or `risk/safety_pairs.md` entry. The
   page templates are documented in the KB's own README.
2. **Rerun the generator.**
   ```bash
   python findings/tuning-knowledge-2026-06-13/atlas-build/build_atlas.py
   ```
   This rewrites `fixtures/tuner_atlas/tuner_atlas.toml` in place. Diff
   the result, verify it looks sane, commit both the KB edit and the
   regenerated TOML in the same commit.
3. **Bump the schema version only when the schema changes.** Adding a
   new optional field is backwards-compatible (still v1). Renaming a
   field, changing a type, or removing a field is breaking (v2).

The generator is intentionally simple — it's a Python script with no
build-system dependencies. CI does not regenerate the atlas; it
verifies the committed TOML loads cleanly via
`Atlas::load_from_file` in the unit test suite.

### Why no hand-editing?

The atlas's contents are *derived* facts. If we let people hand-tweak
the TOML, the markdown KB stops being source-of-truth and we lose
the audit trail back to the corpus analyses that produced each
record. Hand-editing the TOML is the same kind of mistake as
hand-editing a generated `.pb.cc`.

If the generator produces something wrong, the fix is to correct the
generator or the KB page, not the output.

## What's in the atlas v1

Headline counts straight from the committed file:

| Section | Count |
|---|---|
| `[[table]]` records | 170 |
| `[[tuner]]` records | 9 |
| `[[safety_pair]]` records | 8 |
| `[[anchor]]` records | 3 |

Scope: **single CID, single platform.** Primary CID is `LF79103P`
(2017 USDM WRX MT, FA20DIT). Platform field is
`subaru.va.wrx.fa20dit`. The 170 tables are every LF79103P table the
corpus's 60 decrypted `.ptm` files actually touched; the 118
factory-immutable tables that exist in the def but never appear in
any tune are intentionally omitted (a separate list lives in the KB
README).

The 9 tuner clusters cover the entire decrypted corpus: every COBB
SKU (Stage 0 / Stage 1 base / Stage 1 +BigSF / Stage 1
+Redline/+SF), both NexGen Stage 2 variants (+BigSF, +Redline), the
user's installed Fehr/DMann WRK lineage, the Felix-on-FA24
synthetic, and the NTM FA24 swap basemap. The cluster fingerprints
are derived from 259,757 ROM patches across those 60 tunes.

The 8 safety pairs are the anti-patterns the safety-pair analyser
flagged across the corpus, ordered roughly by how often the corpus
itself exhibits them (`boost_without_wastegate` is the most common
high-severity violator).

The 3 anchors cover the surfaces the RE notes pin tightly: the HPFP
phase-compensation cluster (FA24 swap requirement), the MAF Sensor
Scale region (intake recalibration / SF/BigSF differentiator), and
the FA24 displacement constant (NTM-only sentinel).

Pointer to deeper context: see
`findings/tuning-knowledge-2026-06-13/knowledge-base/README.md` for
the full 170-row ranking table and the factory-immutable list. The
philosophy essay at `knowledge-base/philosophy.md` and the FA24
briefing at `knowledge-base/fa24_swap_briefing.md` are not consumed
by the atlas but cover the same ground in narrative form.

## Open follow-ups

These are the things v1 deliberately does **not** do. Each is a real
gap, not a posture choice.

### Multi-CID coverage — DONE in v2 (with caveats)

v2 (current) lands the per-CID address-resolution layer and the
cross-CID identity hook via `[[table.cid_address]]` and
`Atlas::find_table_for_cid`. The atlas now binds 170 LF79103P tables
to 7 additional SH-2A CIDs (LF75404S, LF75404H, LF75600H, LF9C102P,
LF9D012H, LF9G003T, LF9L000E) — 1,102 cross-CID bindings in total.
Each binding carries that CID's own `address` + `size_bytes`.

What's still primary-CID-only at v2:

- **per-CID `units` / `purpose` / `clusters` / `co_edits` /
  `fa24_portability` / `dimension` / `storage`** — the cross-CID
  heatmap data doesn't carry per-CID semantic / observational
  metadata, so v2 only records per-CID address bindings. A future v3
  may grow per-CID semantic overrides once the def-promotion sweep
  produces consistent data for the secondary CIDs.
- **RH850 Gen-B CIDs** (LHBH/LHBK/LHBP/LHBT × 15 SKUs) — their hybrid
  def storageaddress entries are RAM-mapped pointers (0xFEEFxxxx
  range), not flash offsets, and require a per-CID virtual-to-physical
  fixup table not yet built. v2 lists them in `[[cid_coverage]]` with
  `table_count = 0` and a "not covered" note rather than silently
  dropping them.

### True min/max ranges per table

The atlas records `address` and `size_bytes` but **not** the
observed min/max raw values across the corpus, nor the safe-range
guidance a tuner would use for a "this cell looks out of family"
check. The KB has this data per-page but the generator doesn't yet
emit it. Adding `observed_min` / `observed_max` / `safe_min` /
`safe_max` is a backwards-compatible v1 extension once the KB pages
gain consistent ranges.

### Atlas-driven UI surface

The atlas is currently consumed by `st::library::interpret_inspect`
and `interpret_diff` (heuristic prose) and by the CLI. The GUI
**does not yet** surface atlas data — no per-table context badges,
no co-edit suggestions, no safety-pair warnings in the editor.
Wiring the atlas into the GUI is a separate Phase-5 item; the
loader and TOML are ready for it.

### Refresh cadence

There is no automated "rebuild atlas when corpus changes" pipeline.
The current model is: an analyst rerun the corpus diffs, regenerates
the KB pages, reruns `build_atlas.py`, and commits the result. This
is fine for the current pace of corpus growth (a new tune every few
weeks); if the corpus scales to hundreds of tunes a refresh cadence
will need to be defined.

## Related

- `findings/tuning-knowledge-2026-06-13/knowledge-base/` — source-of-truth
  markdown KB (170 table pages + 9 tuner pages + philosophy +
  FA24 briefing)
- `findings/tuning-knowledge-2026-06-13/atlas-build/build_atlas.py` —
  the generator that produces `tuner_atlas.toml` from the KB
- `src/library/include/st/library/atlas.hpp` +
  `src/library/src/atlas.cpp` — C++ loader and lookup API
- `tests/unit/library/test_atlas.cpp` — round-trip + lookup tests
- `docs/11-definition-format.md` — the **structural** def the atlas
  augments (see *When to use the atlas vs the LF79103P def* above)
- `docs/34-cobb-ap-as-tune-vault.md` — the `.ptm` ingestion path that
  produces the corpus the atlas summarises
- `docs/36-tune-as-patch-set.md` — the patch-set model the atlas's
  cluster + co-edit observations are built on top of
