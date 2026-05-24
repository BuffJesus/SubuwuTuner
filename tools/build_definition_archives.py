#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Build per-platform ZIP archives of the in-tree definition packs for
ship-time install footprint reduction.

The bundled definitions in `definitions/<platform>/` are TOML — text,
heavily repetitive, compresses ~92% under DEFLATE. Shipping per-platform
zips instead of loose TOMLs cuts the install size from ~71 MB to ~6 MB
across the 9 currently populated directories (8 platforms + ecuparams),
**without** changing the at-runtime user experience: the future C++
loader (see DESIGN below) treats `<install>/definitions/<id>.toml` as an
override that wins over the same id inside the zip.

Per-platform (not one big zip) so:
  - Loading a single platform doesn't have to scan / decompress others.
  - Adding a new platform doesn't rewrite the existing archives.
  - A single corrupt zip only blocks one platform, not all of them.
  - Future incremental update workflows (download just the changed
    `impreza.zip`) become trivial.

USAGE
  python tools/build_definition_archives.py
      Builds `definitions/_archives/<platform>.zip` for every platform
      directory under `definitions/` that contains files. Skips the
      `_archives/` dir itself.

  python tools/build_definition_archives.py --output-dir <DIR>
      Write archives to <DIR> instead of `definitions/_archives/`.

  python tools/build_definition_archives.py --platforms impreza legacy
      Build only these platforms (relative names under definitions/).

  python tools/build_definition_archives.py --check
      Report what WOULD be built without writing files. Exits 0 if
      every existing archive is up-to-date relative to its source
      files (latest mtime), non-zero if any archive is stale or
      missing. Suitable as a CI gate.

  python tools/build_definition_archives.py --clean
      Remove _archives/<platform>.zip files that no longer have a
      matching source directory (e.g. after a platform was removed).

DESIGN — runtime overlay (planned, C++ side)
  The loader at `st::defs::PackLoader` is extended to consult two
  sources in priority order:

  1. `<install>/definitions/<id>.toml`          (top-level loose)
  2. `<install>/definitions/<platform>/<id>.toml` (per-platform loose)
  3. `<install>/definitions/<platform>.zip` entries

  Looking up a pack by id walks (1)..(3) and returns the first hit.
  Loose files trump zipped entries, so a user can drop in a custom
  pack to override the bundled one. Listing all packs for a platform
  is the union of (2) and (3) deduplicated by id, with loose entries
  winning on conflict.

  Library choice: miniz (single-file public-domain C zip lib), ~3000
  lines. FetchContent-ed and statically linked into `st::defs`. No
  pkg-config or system-libzip dependency.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from pathlib import Path
from typing import Optional


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DEFS = REPO_ROOT / "definitions"
DEFAULT_ARCHIVE_DIR_NAME = "_archives"


def list_platform_dirs(defs_dir: Path) -> list[Path]:
    """Return platform directories (any subdir of defs_dir that contains
    at least one regular file). Skips the _archives output dir and any
    .stfolder Syncthing marker dir."""
    SKIP = {DEFAULT_ARCHIVE_DIR_NAME, ".stfolder"}
    out: list[Path] = []
    for child in sorted(defs_dir.iterdir()):
        if not child.is_dir():
            continue
        if child.name in SKIP:
            continue
        if any(c.is_file() for c in child.iterdir()):
            out.append(child)
    return out


def latest_mtime(directory: Path) -> float:
    """Return the latest mtime across all regular files in the directory
    (not recursive — the loader treats archives as flat namespaces)."""
    mtimes = [c.stat().st_mtime for c in directory.iterdir() if c.is_file()]
    return max(mtimes) if mtimes else 0.0


def build_archive(platform_dir: Path, output: Path) -> tuple[int, int, int]:
    """Build `output` as a DEFLATE-compressed zip containing every regular
    file directly under `platform_dir`. Returns (n_files, raw_bytes, zip_bytes)."""
    files = [c for c in sorted(platform_dir.iterdir()) if c.is_file()]
    raw = sum(c.stat().st_size for c in files)
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = output.with_suffix(output.suffix + ".tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for c in files:
            z.write(c, arcname=c.name)
    tmp.replace(output)
    return len(files), raw, output.stat().st_size


def cmd_build(defs_dir: Path, archive_dir: Path,
              platforms: Optional[list[str]]) -> int:
    archive_dir.mkdir(parents=True, exist_ok=True)
    if platforms is not None:
        targets = [defs_dir / p for p in platforms]
        for t in targets:
            if not t.is_dir():
                print(f"build_definition_archives: {t} not a directory",
                      file=sys.stderr)
                return 1
    else:
        targets = list_platform_dirs(defs_dir)
    if not targets:
        print(f"build_definition_archives: no platforms found under {defs_dir}",
              file=sys.stderr)
        return 1
    total_raw = 0
    total_zip = 0
    total_files = 0
    name_w = max(len(t.name) for t in targets)
    print(f"Writing archives under {archive_dir}")
    print()
    for t in targets:
        out = archive_dir / f"{t.name}.zip"
        n, raw, zsize = build_archive(t, out)
        pct = 100 * zsize / raw if raw else 0
        print(
            f"  {t.name:<{name_w}}  {n:>4} files  raw={raw/1024:>8.1f} KB"
            f"  zip={zsize/1024:>7.1f} KB  ratio={pct:>5.1f}%"
        )
        total_raw += raw
        total_zip += zsize
        total_files += n
    print()
    saved = total_raw - total_zip
    print(
        f"Total: {total_files} files across {len(targets)} platforms; "
        f"raw {total_raw/1024/1024:.1f} MB -> zipped {total_zip/1024/1024:.2f} MB "
        f"(saves {saved/1024/1024:.1f} MB)"
    )
    return 0


def cmd_check(defs_dir: Path, archive_dir: Path) -> int:
    targets = list_platform_dirs(defs_dir)
    stale: list[str] = []
    missing: list[str] = []
    for t in targets:
        out = archive_dir / f"{t.name}.zip"
        if not out.exists():
            missing.append(t.name)
            continue
        src_m = latest_mtime(t)
        if src_m > out.stat().st_mtime:
            stale.append(t.name)
    if not missing and not stale:
        print(f"All {len(targets)} platform archives up to date.")
        return 0
    if missing:
        print("Missing archives:")
        for n in missing:
            print(f"  {n}.zip")
    if stale:
        print("Stale archives (source newer than zip):")
        for n in stale:
            print(f"  {n}.zip")
    return 2


def cmd_clean(defs_dir: Path, archive_dir: Path) -> int:
    if not archive_dir.is_dir():
        return 0
    current_platforms = {t.name for t in list_platform_dirs(defs_dir)}
    removed: list[str] = []
    for child in archive_dir.iterdir():
        if not child.is_file() or child.suffix != ".zip":
            continue
        platform = child.stem
        if platform not in current_platforms:
            child.unlink()
            removed.append(child.name)
    if removed:
        print(f"Removed {len(removed)} stale archive(s):")
        for n in removed:
            print(f"  {n}")
    else:
        print("No stale archives to remove.")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--defs-dir", type=Path, default=DEFAULT_DEFS,
        help="root definitions directory (default: definitions/)",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=None,
        help="archive output directory (default: <defs-dir>/_archives/)",
    )
    parser.add_argument(
        "--platforms", nargs="+", default=None,
        help="explicit platform names; default: every populated subdir",
    )
    parser.add_argument("--check", action="store_true",
                        help="report build status without writing")
    parser.add_argument("--clean", action="store_true",
                        help="remove archives whose source is gone")
    args = parser.parse_args(argv)
    archive_dir = args.output_dir or (args.defs_dir / DEFAULT_ARCHIVE_DIR_NAME)
    if args.check:
        return cmd_check(args.defs_dir, archive_dir)
    if args.clean:
        return cmd_clean(args.defs_dir, archive_dir)
    return cmd_build(args.defs_dir, archive_dir, args.platforms)


if __name__ == "__main__":
    sys.exit(main())
