# Installing definition packs

SubuwuTuner ships without bundled VA/VB calibration packs (see [`17-data-distribution-policy.md`](17-data-distribution-policy.md) for why). This guide covers where to put your own packs and how to generate them.

## TL;DR

- **Quickest start:** the repo includes [`fixtures/demo-pack/`](../fixtures/demo-pack/), a synthetic always-available example. Use it to learn the tool without any setup.
- **For real tuning:** put TOML packs anywhere on disk and pass the path explicitly to `subuwutuner-cli --def <path>` or via your `.stune` project's pack reference.
- **Recommended convention:** keep your packs in the per-platform user-data directory below for organization (the GUI's recents and settings already live there).

## Where SubuwuTuner looks (today vs. tomorrow)

**Today** — the CLI requires an explicit `--def <path>`. The GUI loads whatever pack a `.stune` project points at. There is no auto-discovery from a fixed location yet.

**Tomorrow** — the welcome panel will point at a per-platform convention directory and offer one-click access to packs found there. The convention dirs (already used by the GUI for `recents.txt` and `settings.txt`):

| OS | Convention dir for definitions |
|---|---|
| Windows | `%APPDATA%\SubuwuTuner\definitions\` |
| Linux | `$XDG_CONFIG_HOME/subuwutuner/definitions/` (default: `~/.config/subuwutuner/definitions/`) |
| macOS | `~/Library/Application Support/SubuwuTuner/definitions/` |

Even though auto-discovery isn't wired up, putting packs there now is forward-compatible.

## Installing a pack you already have

If someone hands you a TOML pack (or a directory containing `pack.toml` plus sibling TOML fragments):

1. Drop the pack into the convention dir for your OS, or any path you prefer.
2. Reference it explicitly:

```bash
# CLI: pass the pack path (file or directory)
subuwutuner-cli pack-info path/to/your-pack.toml
subuwutuner-cli dump-table --def path/to/your-pack-dir/ --table fuel_main path/to/rom.bin

# Project: reference it once at create time, then it's persisted
subuwutuner-cli project-new --source path/to/rom.bin \
                            --def path/to/your-pack-dir/ \
                            my-tune.stune
```

The pack format is documented in [`11-definition-format.md`](11-definition-format.md).

## Generating your own pack from a public RomRaider XML

`tools/defgen/` converts a RomRaider-format XML definition to SubuwuTuner's TOML schema:

```bash
# Single-ROM XML → single TOML
python tools/defgen/defgen.py path/to/rom.xml -o path/to/out.toml

# Multi-ROM XML → one TOML per <rom> in a directory
python tools/defgen/defgen.py path/to/multi-rom.xml -o path/to/out-dir/
```

Public sources:

- **Genuine RomRaider community XML:** [`github.com/RomRaider/RomRaider`](https://github.com/RomRaider/RomRaider) — the canonical upstream
- **Merp's curated pack:** [`github.com/Merp/SubaruDefs`](https://github.com/Merp/SubaruDefs) — `RomRaider/ecu/metric/ecu_defs.xml` covers most older Subarus

Anything traceable to commercial-tool circumvention (Atlas, COBB, EcuTek, dealer software) is **out of scope** for this repo as first-party content; see [`17-data-distribution-policy.md`](17-data-distribution-policy.md) §4 for the acceptance criteria.

## Generating from your own ROM dump

Planned: a hardware-capture workflow that builds a starter pack from a ROM you have read off your own car. This requires the OBDX Pro VX adapter (or equivalent) and is gated on the Phase 4 bench rig per [`04-roadmap.md`](04-roadmap.md). Until then, the defgen path above is the supported workflow.

## SSM datalogger fragments

The `definitions/pids.toml` (91 SSM PIDs + 68 switches) and `definitions/ecuparams/` (per-CID extended PIDs) are community-sourced from RomRaider's `logger.xml` and ship with the public repo. Your VA/VB pack will need to declare them as fragments via `[pack].includes`:

```toml
# in your pack.toml
[pack]
id       = "my-va-wrx-2019"
includes = ["../ecuparams/<your_cid>.toml", "../pids.toml"]
```

VA/VB packs use UDS rather than SSM and typically only need an ecuparams fragment, not `pids.toml`. See [`definitions/README.md`](../definitions/README.md) for the include conventions.

## Verifying a pack loaded correctly

```bash
subuwutuner-cli pack-info path/to/your-pack-dir/
```

Expected output: schema version, pack id, table count, axis count, scaling count, and any include resolutions. If the path is wrong or the pack is missing, the CLI emits a Path-B context hint pointing at this doc.

## Troubleshooting

**"No definition pack found at the path above."** — the CLI saw a path that doesn't exist. Either the pack file is somewhere else, or it was never put there. Check the path you passed.

**Pack loads but ROM doesn't identify.** — the pack's `[[identification]]` records didn't match any byte sequence in the ROM. Confirm you have the right pack for your CID (`subuwutuner-cli rom-info --def <pack> path/to/rom.bin` shows the matched CID, if any).

**Parse error on load.** — the TOML is malformed. If you generated with `defgen`, file an issue with the source XML attached and the defgen command line you used.
