# defgen — RomRaider XML → SubuwuTuner TOML

A small Python tool that translates public RomRaider EcuFlash-style XML
definitions into the TOML schema documented in
[`docs/11-definition-format.md`](../../docs/11-definition-format.md).

This is a development tool, not part of the shipped product. Run it once per
RomRaider definition you want to incorporate; commit the resulting TOML.

## Why

Hand-writing 100+ table records per ECU would burn weeks of contributor time.
RomRaider's community has already mapped most factory Subaru maps in
machine-readable form. `defgen` extracts the *facts* — addresses, scalings,
axis breakpoints, calibration IDs — and re-encodes them in our format,
without copying any descriptive text or source code. The clean-room rules
are spelled out in
[`docs/01-reverse-engineering.md`](../../docs/01-reverse-engineering.md).

## Requirements

- Python 3.12+ (uses standard library only — no pip install needed)

## Usage

```bash
# Print TOML to stdout
python tools/defgen/defgen.py path/to/rom.xml

# Write to a file
python tools/defgen/defgen.py path/to/rom.xml -o path/to/out.toml

# Multi-rom XML: emit one .toml per <rom> into a directory
python tools/defgen/defgen.py path/to/multi.xml -o definitions/

# Filter to a single CID
python tools/defgen/defgen.py path/to/multi.xml --rom-id AS80U_2019
```

## What it strips

Per the clean-room rules:

- **Description text** on tables and scalings (long sentences). Short
  identifier-like names (e.g. "Boost Target", "RPM Axis") are kept; long
  prose descriptions are dropped. Edit them in by hand if you want them.
- **Comments** from the source XML.
- **Inherited fields via `<base>` references** are not chased — `defgen`
  flattens a single `<rom>` element only. Pre-flatten upstream if needed.

What `defgen` keeps: addresses, lengths, storage types, endianness, linear
scaling factors and offsets, axis references, dimensions, category labels.

## Non-linear scalings

`defgen` handles linear `toexpr` of the forms `x`, `x*K`, `x*K+B`, and
`(x±B)*K`. Anything else (e.g. piecewise, polynomial) is emitted as a
linear identity (`factor=1, offset=0`) so the table still loads; the user
must hand-edit the resulting `[[scaling]]` entry to the correct
`formula = "piecewise"` form (see
[`docs/11-definition-format.md`](../../docs/11-definition-format.md)).

## Cousin-pack seeding

For CIDs the community has stock bins for but no RomRaider XML — typically
post-2009 firmware revisions of a family already in `definitions/` — use
`cousin_seed.py` to clone an existing sibling pack as an unverified
starting point:

```bash
python cousin_seed.py --base definitions/legacy/a2tb100k.toml \
                      --cid A2TB100Z \
                      -o definitions/legacy/a2tb100z.toml
```

The output inherits every table address and scaling from the base pack
and replaces only the CID-bearing fields. It carries a prominent
`# COUSIN-SEED pack` header with the validation checklist needed before
the seed graduates to a verified definition (rom-info CID match,
representative `dump-table` sanity check, `rom_diff_localize.py` against
stock/tuned of the target). `ecu_part` is cleared in the
`[[identification]]` block (the validator fills it from the target ROM's
ecuid bytes); the base's per-CID `../ecuparams/*.toml` is stripped from
the includes list because those SSM extended-PID addresses are
CID-specific.

## Testing

```bash
python -m unittest discover -s tools/defgen
```

The test suite uses a synthetic RomRaider-shaped fixture
(`tests/fixtures/minimal_rom.xml`), written from scratch for this project,
to validate every conversion path. CI runs the same suite on every push.
