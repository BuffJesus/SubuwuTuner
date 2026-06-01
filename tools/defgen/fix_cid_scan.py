#!/usr/bin/env python3
"""
fix_cid_scan.py — one-time fix to flip defgen-produced packs to cid_scan
mode for FA-DIT WRX CID families (LF*/LV*/LH*/AF*/AE*).

Per docs/11 §"Scan mode", these CID families live at variable per-firmware
offsets. The default `cid_address = 0x00000000` defgen used to emit never
matched any real ROM. This script edits in-tree pack TOMLs in place to:

    cid_address = 0x...        →   cid_scan  = true
    cid_length  = N            →   (removed; defaults to len(cid_match))

The defgen.py change in this same commit prevents the bug from coming
back on future regens. This script is for the existing in-tree packs.

Idempotent: re-running on an already-fixed pack is a no-op. Refuses to
touch packs whose [[identification]] block doesn't match the family
prefix.

Usage:
    python tools/defgen/fix_cid_scan.py [--root <definitions/>] [--dry-run]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CID_SCAN_PREFIXES: tuple[str, ...] = ("LF", "LV", "LH", "AF", "AE")

# Matches a complete [[identification]] block — captures the body so we
# can rewrite it in place. Greedy until the next [[ table-style header
# or end of file.
IDENT_BLOCK_RE = re.compile(
    r"(\[\[identification\]\][^\[]*?)(?=\[\[|\Z)",
    re.DOTALL,
)

CID_MATCH_RE   = re.compile(r'^\s*cid_match\s*=\s*"([^"]*)"\s*$',     re.MULTILINE)
CID_ADDRESS_RE = re.compile(r'^\s*cid_address\s*=\s*0x[0-9A-Fa-f]+\s*\n', re.MULTILINE)
CID_LENGTH_RE  = re.compile(r'^\s*cid_length\s*=\s*\d+\s*\n',            re.MULTILINE)
CID_SCAN_RE    = re.compile(r'^\s*cid_scan\s*=\s*(true|false)\s*$',      re.MULTILINE)


def should_scan(cid_match: str) -> bool:
    s = cid_match.strip().upper()
    return any(s.startswith(p) for p in CID_SCAN_PREFIXES)


def process_block(block: str) -> tuple[str, bool]:
    """Rewrite one [[identification]] block. Returns (new_block, changed)."""
    cid_m = CID_MATCH_RE.search(block)
    if cid_m is None:
        return block, False
    cid_value = cid_m.group(1)
    if not should_scan(cid_value):
        return block, False
    if CID_SCAN_RE.search(block):
        # Already has cid_scan declared — leave alone (idempotent).
        return block, False

    new_block = block
    # Replace `cid_address = 0xXXXXXXXX\n` with `cid_scan  = true\n` so
    # the indentation lines up with the surrounding fields (defgen
    # right-pads field names to 12 chars).
    new_block, addr_n = CID_ADDRESS_RE.subn("cid_scan     = true\n", new_block, count=1)
    if addr_n == 0:
        # Block didn't have a cid_address line. Insert cid_scan above
        # cid_match so the field order stays readable.
        new_block = CID_MATCH_RE.sub(
            lambda m: "cid_scan     = true\n" + m.group(0), new_block, count=1)
    # Drop cid_length — under scan mode the loader infers it from
    # len(cid_match), and keeping the literal would just be noise.
    new_block, _ = CID_LENGTH_RE.subn("", new_block, count=1)
    return new_block, new_block != block


def process_file(path: Path, dry_run: bool) -> int:
    """Returns count of identification blocks changed in this file."""
    try:
        original = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        # Some XML-derived TOMLs may have stray Latin-1 bytes
        original = path.read_text(encoding="utf-8", errors="replace")

    changed_total = 0
    def replacer(match: re.Match[str]) -> str:
        nonlocal changed_total
        new_block, changed = process_block(match.group(1))
        if changed:
            changed_total += 1
        return new_block

    new_text = IDENT_BLOCK_RE.sub(replacer, original)
    if changed_total > 0 and new_text != original:
        if dry_run:
            print(f"  WOULD UPDATE  {path}  ({changed_total} block(s))")
        else:
            path.write_text(new_text, encoding="utf-8", newline="\n")
            print(f"  updated       {path}  ({changed_total} block(s))")
    return changed_total


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=Path("definitions"),
                    help="Root directory holding the pack TOMLs (default: definitions/)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Report intended changes without writing")
    args = ap.parse_args()

    root: Path = args.root
    if not root.is_dir():
        print(f"fix_cid_scan: {root} is not a directory", file=sys.stderr)
        return 2

    files_changed = 0
    blocks_changed = 0
    for path in sorted(root.rglob("*.toml")):
        n = process_file(path, args.dry_run)
        if n > 0:
            files_changed += 1
            blocks_changed += n

    print(f"\n{files_changed} file(s), {blocks_changed} identification block(s) "
          f"{'would be ' if args.dry_run else ''}updated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
