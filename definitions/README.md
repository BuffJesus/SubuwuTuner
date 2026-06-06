# SubuwuTuner definition packs

One TOML per ROM CID. Each file is a self-contained `[pack]` with its own
`[[identification]]`, `[[scaling]]`, `[[axis]]`, and `[[table]]` records.

```
definitions/
├── pids.toml          — Shared SSM datalogger payload: 91 PIDs + 68 switches
├── ecuparams/         — 331 per-CID extended-PID fragments (load alongside the pack)
├── va/         8 packs — WRX VA (2015–2021), fact-only XML → defgen
├── vb/        18 packs — WRX VB (2022+, through MY2026), fact-only XML → defgen
├── impreza/  150 packs — older Impreza (incl. WRX/STi pre-VA), Merp pack
├── legacy/    68 packs — Legacy + Legacy/Outback variants, Merp pack
├── forester/  61 packs — Forester (XT, 2.0, 2.5i, Turbo), Merp pack
├── liberty/   25 packs — JDM Legacy (Liberty), Merp pack
├── outback/   12 packs — US-market Outback, Merp pack
├── baja/      10 packs — Baja + Baja Turbo, Merp pack
├── tribeca/    4 packs — B9 Tribeca + Tribeca, Merp pack
└── exiga/      2 packs — Exiga (JDM), Merp pack
```

332 ROMs from Merp + 26 from VA/VB fact-only XML = **358 ECU packs total**,
plus one cross-cutting `pids.toml` for SSM datalogger params/switches and
331 per-CID `ecuparams/<rom_id>.toml` fragments for the ROM-specific
extended logger parameters.

## Provenance

Two source pipelines, both clean-room per `docs/01-reverse-engineering.md`
and `docs/15-clean-room-engineering.md`:

**Current WRX (`va/`, `vb/`)** — generated from `*.facts.xml` inputs in the
developer's analyst-side workspace (`D:\Subuwu\atlas-personal\`, kept
off-repo). Those inputs are fact-stripped derivatives of public RomRaider
XML: names replaced with `va_tNNNN`/`vb_tNNNN` slugs, descriptive prose
dropped, only addresses, axis sizes, storagetype, endian, and scaling
expressions retained.

**Everything else (`impreza/`, `forester/`, `legacy/`, …)** — generated
from Merp's canonical RomRaider community pack at
`github.com/Merp/SubaruDefs` (`RomRaider/ecu/metric/ecu_defs.xml`, 7.9 MB).
defgen retains short human-readable names (≤ 64 chars, no commas/periods)
because they're functional identifiers — "Boost Target", "MAF Voltage
Sensor" — not expressive prose. Long descriptive paragraphs (`<description>`
text) are stripped per the clean-room rule. Facts (addresses, scaling
math, axis breakpoints) flow through as-is.

```
public RomRaider XML
  → (atlas-personal scrubber, off-repo)   ← only for va/vb
    → *.facts.xml (off-repo)
      → tools/defgen/defgen.py
        → definitions/{va,vb}/*.toml

github.com/Merp/SubaruDefs
  → tools/defgen/defgen.py
    → definitions/{impreza,legacy,…}/*.toml
```

## Status caveats

- **Scaling math** is exact for all linear expressions, including the
  `(x)/N`, chained `((x)-A)/B*C`, and `/*RSHIFT(N)*/x` hardware-shift hints
  (interpreted as `2^-N` factors per Subaru fixed-point convention;
  re-verify against real ROM data when a bench rig lands).
- **`/*INVERSE_DIVIDE*/` and `/*AND*/` markers** are flagged as non-linear
  and emit identity scaling with a defgen warning — these need hand-edit
  to model as piecewise or AND-mask scaling.
- **Unresolved axis storage** — some 2D tables have an X/Y axis named in
  the source XML but no `storageaddress` after inheritance flatten. defgen
  demotes these to scalar with a warning rather than emitting a broken
  reference. For Merp's pack the demote count is high because many
  tables declared `sizey="1"` with a static label — those ARE scalars in
  our model, so the demotion is correct even though the warning is loud.
- **Names** — VA/VB packs use generated slugs; Merp-derived packs use the
  short functional names from the source XML. Friendly long-form names
  remain hand-edit territory.

## Regenerating

```
# VA / VB (analyst-side fact-XML, kept off-repo).
python tools/defgen/defgen.py D:/.../va_wrx.facts.xml -o definitions/va/
python tools/defgen/defgen.py D:/.../vb_wrx.facts.xml -o definitions/vb/

# Older Subarus from Merp's pack (clone the repo first):
git clone --depth 1 https://github.com/Merp/SubaruDefs build/scratch/SubaruDefs
python tools/defgen/defgen.py \
    build/scratch/SubaruDefs/RomRaider/ecu/metric/ecu_defs.xml \
    -o build/scratch/legacy/
# Then split by [pack].platform into definitions/{impreza,forester,...}/.

# SSM datalogger PIDs + switches + per-CID ecuparams. The ecuparam pass
# resolves each <ecu id=…> against the `ecu_part` field in existing
# packs, so run defgen first, then loggergen with --packs-dir.
python tools/defgen/loggergen.py \
    build/scratch/SubaruDefs/RomRaider/logger/metric/logger.xml \
    -o definitions/pids.toml \
    --ecuparams-into definitions/ecuparams/ \
    --packs-dir definitions/
```

`--apply-to-pack` is the right mode if you've hand-edited a pack and want
to layer in new records from an updated XML without losing your edits.

## Loading from C++ / CLI

```
subuwutuner-cli pack-info definitions/va/lf9c102p.toml
subuwutuner-cli table-list definitions/impreza/a4sg900c.toml
subuwutuner-cli dump-table --def definitions/impreza/a4sg900c.toml \
                           --table target_boost_mt stock.bin
subuwutuner-cli pack-info definitions/pids.toml    # 91 SSM PIDs, no tables
```

## How fragments wire into ECU packs

Every SSM-capable ECU pack declares an `includes` list in its `[pack]`
header pointing at the universal PID/switch payload and (where one
exists) at its per-CID ecuparam fragment. For example:

```
# definitions/impreza/a4sg900c.toml
[pack]
…
includes = ["../ecuparams/a4sg900c.toml", "../pids.toml"]
```

The C++ loader (`Definition::from_file`) walks `includes` after parsing
the main TOML, ingests each fragment's `[[scaling]]`/`[[axis]]`/
`[[table]]`/`[[pid]]`/`[[switch]]` arrays (the fragment's own `[pack]`
header is ignored — only records flow through), and recurses depth-first
into the fragment's own `includes` if it has any. Paths are resolved
relative to the pack file's directory; cycles raise ParseError.

VA/VB packs live on UDS, not SSM, so they don't include `../pids.toml`.

## What's NOT here

- **Bit-level breakdowns** of multi-flag bytes that aren't already
  modelled as individual switches.
