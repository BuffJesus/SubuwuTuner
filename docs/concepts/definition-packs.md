# Definition packs

A **definition pack** is the schema that turns a raw ROM into typed
tables. Without one, the ROM is opaque bytes; with one, you see Fuel
Main as a 16×16 grid of AFR targets indexed by RPM and MAP.

## Format

TOML, file-based or directory-based. A single `pack.toml` can describe
one ROM, or you can split the schema across multiple files joined by
`[pack].includes`:

```toml
[pack]
id      = "demo-pack"
version = "0.1.0"
includes = ["../pids.toml", "../ecuparams/lf79103p.toml"]

[[identification]]
address = 0x100
bytes   = "AB CD EF 01"
cid     = "LF79103P"

[[scaling]]
name = "afr_target"
expr = "raw * 0.1"
inverse = "scaled * 10"
unit = "AFR"

[[table]]
name    = "fuel_main"
address = 0x10000
type    = "uint8"
rows    = 16
cols    = 16
scaling = "afr_target"
x_axis  = "rpm_axis_16"
y_axis  = "map_axis_16"
```

Full schema at [Reference → Definition format](../reference/definition-format.md).

## Path B: packs are user-supplied

SubuwuTuner does **not** bundle VA/VB calibration packs in the public
repo. This is a distribution choice, not a technical limitation —
reasoning in
[`docs/17-data-distribution-policy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" }.

The demo pack (`fixtures/demo-pack/`) ships and is fully synthetic; use
it to learn the tool. For real ROMs you have three options:

1. **Drop a pack you already have.** Anywhere on disk; reference via
   `--def <path>`.
2. **Generate one from public XML.** `tools/defgen/` converts
   community-maintained ECU definition XML (e.g., RomRaider GPL repo,
   `Merp/SubaruDefs`) into the SubuwuTuner TOML schema:
   ```bash
   python tools/defgen/defgen.py path/to/rom.xml -o path/to/out-dir/
   ```
3. **Write one by hand.** Practical for one or two custom tables on top
   of an existing pack via `[pack].extends`.

## Linting

`pack-lint` runs the same hygiene checks the GUI uses on load — missing
axes, dangling scaling references, identification overlap, axis
monotonicity, address range overlap, etc.:

```bash
subuwutuner-cli pack-lint path/to/your-pack-dir/
```

CI-friendly with `--json` for downstream tooling.

## Identifying which pack matches a ROM

```bash
subuwutuner-cli rom-info --def path/to/pack-dir/ path/to/rom.bin
```

Reads the pack's `[[identification]]` records, matches against the ROM
bytes, and prints the matched CID (or "no match" if the pack doesn't
cover this CID).

## Convention dirs

Per-platform conventional location for packs:

| OS | Path |
|---|---|
| Windows | `%APPDATA%\SubuwuTuner\definitions\` |
| Linux | `~/.config/subuwutuner/definitions/` |
| macOS | `~/Library/Application Support/SubuwuTuner/definitions/` |

Auto-discovery from these dirs is planned (welcome panel surfacing).
Until then, pass `--def <path>` explicitly.

## Deeper detail

- [`docs/11-definition-format.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/11-definition-format.md){ target="_blank" }
  — full schema spec.
- [`docs/17-data-distribution-policy.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/17-data-distribution-policy.md){ target="_blank" }
  — Path B reasoning, source acceptance criteria.
- [`docs/15-clean-room-engineering.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/15-clean-room-engineering.md){ target="_blank" }
  — clean-room boundary that governs which sources are usable.
