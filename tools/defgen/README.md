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
`# COUSIN-SEED pack` header listing the validation checklist. `ecu_part`
is cleared in the `[[identification]]` block (the validator fills it from
the target ROM's ecuid bytes); the base's per-CID `../ecuparams/*.toml`
is stripped from the includes list because those SSM extended-PID
addresses are CID-specific.

## Verifying cousin seeds — localize.py

`localize.py` is the verification step that follows `cousin_seed.py`. For
each `[[table]]` in the seeded pack, it reads the sibling ROM at the
table's address, reads the target ROM at the same address, and classifies
the pair as HIGH / MED / LOW / ABSENT — that's the per-table verdict on
"is this address still valid in the target ROM?":

```bash
python tools/defgen/localize.py \
    --pack         definitions/legacy/a2tb002c.toml \
    --sibling-rom  fixtures/private/.../A2TB001C.bin \
    --target-rom   fixtures/private/.../A2TB002C.bin \
    --out-report   /tmp/a2tb002c_localize.tsv
```

Confidence labels:

  - **HIGH** — byte-identical, or both regions match in shape/range/
    monotonicity. Address is almost certainly still valid.
  - **MED** — content differs but the SHAPE is right (entropy band,
    monotonicity for axes, plausible range). Address usually still valid;
    values just recalibrated between sibling and target.
  - **LOW** — bytes look unrelated. Table has likely MOVED in the target.
  - **ABSENT** — target ROM too short for the address.

Commit rule of thumb: a seeded pack with **HIGH + MED ≥ 80%** of in-range
tables is solid. Below that, the cousin distance is too wide — either
find a closer sibling or fall through to manual RE.

### --relocate-low (pattern search for moved tables)

For LOW entries, `--relocate-low` runs a pattern search: anchor on the
sibling's first 16 bytes at the LOW table's address; `bytes.find` in
the target ROM (C-optimized, O(N)); re-score each candidate via
`classify_pair` and keep only HIGH/MED candidates within
`--relocate-max-distance` of the original (default 64 KB). Adds two
columns (`relocated_to`, `relocate_reason`) to the TSV.

```bash
python tools/defgen/localize.py --pack ... --sibling-rom ... \
    --target-rom ... --out-report ... --relocate-low
```

Empirically the relocator handles ~10-20% of LOW entries on cross-
revision packs — specifically the ones where the table moved while
keeping byte-identical content. Tables that BOTH moved AND were
recalibrated remain a manual-RE case. The relocator catches whole
sub-region shifts cleanly (look for clustered `+0x...` offsets in
the report).

### --patch-pack (apply the relocations)

`--patch-pack` (implies `--relocate-low`) writes each successful
relocation back into the pack TOML's `[[table]]` blocks in place.
Line-based rewriter preserves comments, blank lines, and indentation;
only `[[table]]` sections are touched. Idempotent.

```bash
python tools/defgen/localize.py --pack <new_pack.toml> --sibling-rom ... \
    --target-rom ... --patch-pack
```

After patching, evaluate the pack against the target ROM via
`subuwutuner-cli rom-info --def <patched_pack> <target.bin>`. Don't
re-run localize with the same sibling+target — the sibling no longer
has the table at the patched address, so `classify_pair` compares
unrelated bytes and the result is misleading.

## Cousin-seed lessons learned (empirical)

Pairing rules that work:

  - **Ones-digit deltas, same trim, same region**: typically 95%+ HIGH+MED,
    no relocation needed. (e.g. a2tb001c → a2tb002c, ez1g109k → ez1g108k)
  - **Tens-digit deltas, same trim, same region**: 90-100% HIGH+MED,
    sometimes a few patches help. (e.g. ez1d302b → ez1d301a)
  - **AT ↔ MT within same year + family**: usually works; ~5-10% LOW for
    transmission-specific cal cells. (e.g. a2wc500r → a2wc500s)
  - **Cross-platform same firmware-family**: works when the firmware is
    genuinely shared. (e.g. e2vg222b outback → e2vg204b legacy, both 2006
    2.5i USDM)

Pairings that don't:

  - **Cross-region** (ADM/EDM/JDM sibling → USDM target): regional
    calibration differences are too wide. Use a USDM sibling.
  - **Trim crossovers** (STI ↔ WRX in 2010-era Impreza): different ECU
    hardware. Don't pair across these.
  - **Hundreds-digit deltas** (400 → 500): typically major-revision
    boundary in Subaru naming. Address shifts are too large.

Post-seed metadata fixes are manual: `cousin_seed.py` doesn't update
`years` or `transmission` from filename hints. After running it, eyeball
the target ROM's filename and patch the pack's `[pack]` block if they
differ from the sibling.

## Testing

```bash
python -m unittest discover -s tools/defgen
```

The test suite uses a synthetic RomRaider-shaped fixture
(`tests/fixtures/minimal_rom.xml`), written from scratch for this project,
to validate every conversion path. CI runs the same suite on every push.
