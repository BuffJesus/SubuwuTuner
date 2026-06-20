# Reading your ROM

How to get a `.bin` file off your ECU. There are four supported sources
depending on what hardware you have.

## Source 1 — OBDX Pro VX (recommended)

The primary live-read path.

```bash
# Stock ECU (factory SA)
subuwutuner-cli rom-pull \
    --transport obdx --device COM5 \
    --authenticate --sa-variant ssmcan1-factory \
    -o my-rom.bin

# COBB-tuned ECU
subuwutuner-cli rom-pull \
    --transport obdx --device COM5 \
    --authenticate --sa-variant cobb-tuned \
    -o my-rom.bin
```

A 2 MB FA20DIT dump takes ~8 minutes — the RMBA chunk cap is 0x800
(2 KB) and that's the bottleneck on this ECU family.

!!! warning "Live-flash gate"
    `rom-pull` is read-only and safe on a stock or tuned ECU. The
    matching write path (`project-flash`) is currently gated on bench
    validation — see [Brick protection](../concepts/brick-protection.md)
    and
    [`docs/04-roadmap.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/04-roadmap.md){ target="_blank" }.

## Source 2 — COBB AccessPort V3 (your own AP)

If you own a married COBB AccessPort and want to read what it
currently holds, SubuwuTuner exposes the AP as a file-vault target
so you can list and pull files from it via USB. The AP keeps its
own backup of the current calibration that this surface can read.

```bash
subuwutuner-cli ap3 state           # AP serial, firmware version, MD5
subuwutuner-cli ap3 ls /backupcksum
subuwutuner-cli ap3 pull /backupcksum/<filename> stock.bin
```

SubuwuTuner respects the AP's own marriage state: an unmarried AP
refuses mutating operations through SubuwuTuner. `--allow-unmarried-ap`
overrides this for advanced workflows where you know what you're doing
(e.g., a fresh-from-the-box AP you're inspecting).

USB driver setup (Zadig WinUSB on Windows, udev rule on Linux):
[Install → USB hardware setup](../getting-started/installation.md#5-usb-hardware-setup-only-if-reading-or-flashing-a-real-ecu).

## Source 3 — `.ptm` tune file

A `.ptm` is the file format COBB-style tunes ship in. SubuwuTuner can
read one against a known base ROM to materialize the patched
calibration as a `.bin` you can open as a project:

```bash
# With explicit base ROM
subuwutuner-cli ptm import my-tune.ptm \
                           --base-rom path/to/stock-cid.bin \
                           --project my-tune.stune

# With ST_PTM_BASE_ROM_DIR auto-discovery
export ST_PTM_BASE_ROM_DIR=path/to/base-rom-library
subuwutuner-cli ptm import my-tune.ptm --project my-tune.stune
```

`ptm import` populates `edits.toml` with one `ByteEdit` per `.ptm`
patch, tagged `"ptm_import"` so you can `History::undo_while_tag` to
revert the whole import as a single batch.

## Source 4 — prior dump on disk

If you already have a `.bin` from anywhere — your own prior dump, a
known-good reference, a fixture in the repo — point a project at it
directly:

```bash
subuwutuner-cli project-new \
    --source path/to/your-rom.bin \
    --def path/to/pack-dir/ \
    my-tune.stune
```

The source ROM is copied into the project and never modified.

## Verifying you got the right one

```bash
subuwutuner-cli rom-info --def path/to/pack-dir/ path/to/rom.bin
```

Prints the matched CID (per the pack's `[[identification]]` records),
file size, CRC32, and any pack-specific metadata. If the CID doesn't
match the car you pulled from, you probably have the wrong pack — see
[Definition packs](../concepts/definition-packs.md).

## Deeper detail

- [`docs/13-transport.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/13-transport.md){ target="_blank" }
  — transport-level RMBA chunking, NRC handling.
- [`docs/23-security-access.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/23-security-access.md){ target="_blank" }
  — SA prelude detail and variant catalog.
- [`docs/34-cobb-ap-as-tune-vault.md`](https://github.com/BuffJesus/SubuwuTuner/blob/main/docs/34-cobb-ap-as-tune-vault.md){ target="_blank" }
  — AP3 vault protocol detail.
