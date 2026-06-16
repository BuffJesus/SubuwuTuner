# `tools/library_inventory/` — local tune-library index

Standalone Python script that walks your local disk for `.ptm` tune files and `.stune` projects, hashes everything (MD5 — matches the AP's `/backupcksum` format), dedups by content hash, and emits a unified library index.

Pre-stages the eventual in-tool "currently-flashed identification" feature ([UX handoff Tier 1 #2](../../../../findings/handoffs/HANDOFF-to-analyst-2026-06-13-ap-browser-ux.md)) without touching the C++ codebase.

## Run

```bash
# Default: scan canonical paths (D:/Subuwu/ap-maps, D:/Documents/Tuner, etc.)
python tools/library_inventory/inventory.py

# Custom paths:
python tools/library_inventory/inventory.py --scan D:/my-tunes --scan ~/tunes

# JSON-on-stdout instead of writing files:
python tools/library_inventory/inventory.py --json > library.json

# Specify output directory:
python tools/library_inventory/inventory.py --out-dir ./my-library
```

No deps beyond the Python stdlib. Python 3.10+ recommended (uses `tomllib` on 3.11+, has a minimal fallback for 3.10).

## Output

Two files in `--out-dir` (default: cwd):

### `library_index.toml`

Machine-readable, single-document TOML. Each `.ptm` becomes a `[[ptm]]` table; each `.stune` becomes a `[[stune]]` table.

```toml
schema = "subuwutuner.library-index.v1"
ptm_count = 56
stune_count = 3

[[ptm]]
  path        = "D:/Subuwu/ap-maps/Stage1 91 v401.ptm"
  size        = 58005
  mtime       = 1717891234
  md5         = "5ab18598343bc7d9dcd3afa5e0567d72"
  vendor      = "COBB"
  stage       = "Stage1"
  fuel_grade  = "91"
  variant     = "v401"

[[stune]]
  path             = "D:/Documents/Tuner/Projects/FehrTune-0606.stune"
  project_name     = "Fehr WRK3 base"
  ptm_vendor_id    = "Fehr"
  ptm_vehicle_id   = "SUBA_US_WRXM_CF_17_F"
  source_bin       = "D:/.../FehrTune-0606.stune/source.bin"
  source_bin_md5   = "fa67628fd8bcece66935ff1e0adf9e9a"
```

### `library_summary.md`

Human-readable report. Groups `.ptm` files by vendor, lists `.stune` projects with their import metadata, includes a quick "match `/backupcksum` against this report" hint.

## What it parses out of filenames

Heuristic vendor / stage / fuel / variant detection, observed conventions:

| Pattern | Output field | Examples |
|---|---|---|
| `Stage[0-9]` / "COBB" | vendor = "COBB", stage = "Stage1" | "Stage1 91 v401.ptm" |
| "NexGen" | vendor = "NexGen" | "NexGen Stage2 BigSF.ptm" |
| "Fehr" / "DMann" | vendor = "Fehr" | "Fehr_DMann CAN 93 ETune (Reflash-WRK3).ptm" |
| "NTM" / "Felix" / "Epifan" | vendor accordingly | — |
| `91` / `93` / `E85` / `Race` | fuel_grade | "Stage2 93 v401.ptm" |
| `WRK[N]` / `v[NNN]` / `Redline` / `SF` / `BigSF` etc. | variant | "Reflash-WRK3", "v401", "BigSF" |

These are weak signals — pattern match means "filename is consistent with this label", not "this tune is definitively from that vendor." For ground truth, decrypt the `.ptm` with `subuwutuner-cli --enable-cobb-ap-cipher ptm inspect` and read the `<vendorId>` / `<authorName>` from the PrivateData XML.

## Dedup convention

Two `.ptm` files with the same MD5 are the same tune, even if filenames differ (`Stage1 91 v401.ptm` and `Stage1 91 v401_UNLOCKED.ptm` for example). The script keeps one canonical entry per hash and records the dropped sibling's path in the canonical entry's `unlocked_sibling` field. Tie-break: prefer non-`_UNLOCKED` filenames; otherwise prefer the shortest path.

## Cross-referencing with a live AP

The script does not touch USB. To match the AP's currently-flashed identity against this index:

### Quick way — `--query`

```bash
MD5=$(subuwutuner-cli --enable-cobb-ap-cipher ap3 pull /backupcksum --into - | tr -d '\n\r ')
python tools/library_inventory/inventory.py --quiet --query "$MD5"
```

Outputs a single tab-separated line on match (`md5 \t vendor \t stage \t variant \t fuel_grade \t path`), exits 0. Exits 1 on no match (silent unless you drop `--quiet`), 2 on bad input. Pipeline-friendly:

```bash
# Use awk to pull just the vendor + variant:
python inventory.py --query "$MD5" | awk -F'\t' '{print $2 " " $4}'
```

### Slow way — grep the index file

```bash
# Pull the AP's MD5
subuwutuner-cli --enable-cobb-ap-cipher ap3 pull /backupcksum --into /tmp/cksum

# The file contains a single 32-char hex MD5 + newline. Grep the index:
grep "$(cat /tmp/cksum | tr -d '\n')" library_index.toml
```

If grep finds a hit, the corresponding `[[ptm]]` entry's `path` + `vendor` + `variant` identify the currently-flashed tune.

## What's NOT in scope

- **No AP querying.** Script is hardware-free. AP cross-ref is a downstream step (see above).
- **No .ptm decryption.** Filename heuristics only. For real metadata extraction, use `subuwutuner-cli ptm inspect`.
- **No GUI integration.** The eventual library-aware AP browser feature ([UX handoff Tier 1 #2](../../../../findings/handoffs/HANDOFF-to-analyst-2026-06-13-ap-browser-ux.md)) consumes this format but the integration is C++ side.
- **No re-hash skip / caching.** Re-running rehashes everything. For very large libraries (1000+ .ptm files) this might take a minute; add a `--cache` flag if it becomes painful.

## Why not in `tools/defgen/`?

`defgen` is the definition-pack generation pipeline (XML → TOML). Library inventory is unrelated — it's a host-side bookkeeping tool over user-owned tune files, not a build-time artifact. Separate directory keeps the responsibilities clean.
