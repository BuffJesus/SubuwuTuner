#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Verify the Subaru DI flash-image checksum invariant across one or
more plaintext ROMs.

Invariant (per fixtures/private/findings_algorithms/checksum-recompute.md):
  - Magic `0x55 0x55` at file offset 0.
  - Sum of all 16-bit shorts across the entire image equals `0x5AA5`.
  - Endianness: big-endian for A-series (SH-2A, `LF7x` / `LF9x`,
    2015-2021), little-endian for B-series (RH850, `LHB*`, 2022+).

Usage:
  python tools/verify_flash_checksum.py path/to/rom.bin [--endian be|le|auto]
  python tools/verify_flash_checksum.py --sweep fixtures/private/roms_plaintext_by_cid/

`--endian auto` infers from the directory name (CID prefix `LF` -> BE,
`LHB` -> LE), or falls back to trying both.

`--sweep <DIR>` walks <DIR> and verifies every `rom.bin` it finds under
a `<year>_<CID>` subfolder. Exit 0 if every ROM passes; non-zero if any
fails.

Per-CID slot-offset note:
  The brute-force approach described in
  fixtures/private/findings_algorithms/flash-disassembly-roadmap.md
  doesn't actually work — `stored_value == 0x5AA5 - sum_excluding_slot`
  is tautological for any slot in a well-formed ROM. Identifying the
  unique slot requires disassembly of the bootloader's checksum-
  write routine, or edit-pair analysis (two ROMs of the same CID
  differing only by an edit and the recomputed slot). This script
  pins the invariant; finding the slot is a separate task.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable, Optional


CHECKSUM_TARGET = 0x5AA5
MAGIC_HEADER = b"\x55\x55"


def sum16(data: bytes, endian: str) -> int:
    """Sum of 16-bit shorts over the entire image, mod 2^16.

    `endian` is 'big' or 'little'. The image must have even length.
    """
    if len(data) % 2 != 0:
        raise ValueError(f"image length {len(data)} is not even")
    total = 0
    if endian == "big":
        for i in range(0, len(data), 2):
            total += (data[i] << 8) | data[i + 1]
    elif endian == "little":
        for i in range(0, len(data), 2):
            total += data[i] | (data[i + 1] << 8)
    else:
        raise ValueError(f"unknown endian {endian!r}")
    return total & 0xFFFF


def verify(image_bytes: bytes, endian: str) -> tuple[bool, str]:
    """Return (ok, diagnostic). `ok=True` iff magic + checksum both pass."""
    if not image_bytes.startswith(MAGIC_HEADER):
        return False, (
            f"magic mismatch at offset 0: got {image_bytes[:2].hex().upper()}, "
            f"expected {MAGIC_HEADER.hex().upper()}"
        )
    if len(image_bytes) % 2 != 0:
        return False, f"odd image length {len(image_bytes)} bytes (must be even)"
    s = sum16(image_bytes, endian)
    if s != CHECKSUM_TARGET:
        return False, (
            f"sum16(endian={endian}) = 0x{s:04X}, "
            f"expected 0x{CHECKSUM_TARGET:04X}"
        )
    return True, f"ok ({len(image_bytes):,} bytes, sum16 = 0x{CHECKSUM_TARGET:04X})"


def infer_endian(name: str) -> Optional[str]:
    """Guess endian from a folder name like '2017_LF79103P' or '2024_LHBP301b00G'.
    Returns 'big' for A-series (LF prefix), 'little' for B-series (LHB prefix),
    or None if no confident guess.
    """
    parts = name.split("_", 1)
    cid = parts[1] if len(parts) > 1 else parts[0]
    cid_upper = cid.upper()
    if cid_upper.startswith("LHB"):
        return "little"
    if cid_upper.startswith("LF"):
        return "big"
    return None


def _try_both_endians(data: bytes) -> tuple[Optional[str], str]:
    """Return (winning_endian, diagnostic). Used when --endian auto can't
    infer from the path."""
    ok_be, _ = verify(data, "big")
    ok_le, _ = verify(data, "little")
    if ok_be and not ok_le:
        return "big", "auto-detected big-endian"
    if ok_le and not ok_be:
        return "little", "auto-detected little-endian"
    if ok_be and ok_le:
        return "big", "BOTH endians sum to 0x5AA5 (rare collision); reporting BE"
    return None, "neither endian sums to 0x5AA5"


def verify_one(path: Path, endian_arg: str) -> tuple[bool, str, str]:
    """Verify a single rom.bin path. Returns (ok, endian_used, message)."""
    try:
        data = path.read_bytes()
    except OSError as e:
        return False, "?", f"read error: {e}"

    if endian_arg == "auto":
        guess = infer_endian(path.parent.name)
        if guess is not None:
            ok, msg = verify(data, guess)
            return ok, guess, msg
        winner, note = _try_both_endians(data)
        if winner is None:
            return False, "?", note
        ok, msg = verify(data, winner)
        return ok, winner, f"{msg} ({note})"
    ok, msg = verify(data, endian_arg)
    return ok, endian_arg, msg


def iter_sweep_roms(root: Path) -> Iterable[Path]:
    """Yield every `rom.bin` directly under a `<year>_<CID>/` subfolder of root."""
    for child in sorted(root.iterdir()):
        if not child.is_dir():
            continue
        rom = child / "rom.bin"
        if rom.is_file():
            yield rom


def cmd_sweep(root: Path, endian_arg: str) -> int:
    """Walk a directory of <year>_<CID>/rom.bin layouts; verify each."""
    if not root.is_dir():
        print(f"verify_flash_checksum: {root} is not a directory", file=sys.stderr)
        return 1
    roms = list(iter_sweep_roms(root))
    if not roms:
        print(
            f"verify_flash_checksum: no rom.bin found under {root}/<year>_<CID>/",
            file=sys.stderr,
        )
        return 1

    passes = 0
    fails = 0
    max_name = max(len(r.parent.name) for r in roms)
    for rom in roms:
        ok, endian, msg = verify_one(rom, endian_arg)
        marker = "OK     " if ok else "FAIL   "
        name = rom.parent.name.ljust(max_name)
        endian_disp = endian.upper() if endian != "?" else "?"
        print(f"  {marker} {name}  endian={endian_disp:6s}  {msg}")
        if ok:
            passes += 1
        else:
            fails += 1

    print()
    total = passes + fails
    print(f"Summary: {passes}/{total} pass, {fails}/{total} fail.")
    if fails == 0:
        print(
            f"RESULT: All {total} ROMs match the Findings checksum spec exactly "
            f"(magic 0x5555 at offset 0; sum16 = 0x{CHECKSUM_TARGET:04X})."
        )
        return 0
    print(
        "RESULT: Some ROMs failed - either the image isn't a Subaru DI ECU "
        "flash, the wrong endian was forced, or the image is truncated."
    )
    return 2


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("rom", nargs="?", type=Path, help="single rom.bin to verify")
    g.add_argument(
        "--sweep",
        type=Path,
        default=None,
        help="directory containing <year>_<CID>/rom.bin layout (verify all)",
    )
    p.add_argument(
        "--endian",
        choices=("be", "le", "auto"),
        default="auto",
        help="byte order for 16-bit shorts (default: infer from CID prefix)",
    )
    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    endian_arg = {"be": "big", "le": "little", "auto": "auto"}[args.endian]

    if args.sweep is not None:
        return cmd_sweep(args.sweep, endian_arg)

    ok, endian, msg = verify_one(args.rom, endian_arg)
    marker = "OK" if ok else "FAIL"
    print(f"{marker}  {args.rom}  endian={endian.upper()}  {msg}")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
