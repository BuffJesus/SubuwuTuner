#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
localize — verify (and optionally relocate) a sibling pack's table
addresses against a different decoded ROM.

The complement to cousin_seed.py. Cousin-seed creates a draft pack
by cloning a sibling and swapping CID-bearing fields; it makes no
attempt to verify that the sibling's table addresses are still
correct in the target ROM. localize.py is that verification step:
for each `[[table]]` entry in the sibling pack, read bytes from the
sibling ROM at the table's address, read bytes from the target ROM
at the same address, and report whether the addresses still point at
something that looks reasonable.

The output is a per-table TSV report with a confidence label per
table:

  HIGH    — sibling and target byte patterns are very similar
            (range, entropy, monotonicity all match). The address
            is almost certainly still valid in the target ROM.

  MED     — byte content is different but the SHAPE looks right
            (entropy in the same band; monotonicity preserved for
            axes; reasonable range for the data type). This is the
            common case for calibration cells — the address is
            usually still valid, the values just differ between
            sibling and target.

  LOW     — bytes look unrelated (random noise vs structured data,
            or wildly different range). The table has likely moved
            in the target ROM. Pass `--relocate-low` to attempt
            pattern-search relocation.

  ABSENT  — target ROM is too short for the address (size mismatch
            between sibling and target).

Usage:
    python localize.py \\
        --pack         definitions/legacy/a2tb100k.toml \\
        --sibling-rom  fixtures/private/.../A2TB100K.bin \\
        --target-rom   fixtures/private/.../A2TB100Z.bin \\
        --out-report   /tmp/a2tb100z_localize.tsv

Run cousin_seed.py first to generate the draft TOML for the new CID;
then run localize.py to mark each table HIGH/MED/LOW/ABSENT against
the target ROM. The user reviews the report and either accepts the
draft as-is (if everything is HIGH/MED) or relocates the LOW entries
(automatically via `--relocate-low`, or by hand).

Relocation strategy (v2, --relocate-low):
  For each LOW entry, search the target ROM for the sibling's byte
  pattern at the pack's address. Tier 1 is exact-substring search
  using the sibling's first 16 bytes as the needle (C-optimized via
  bytes.find — O(N) per table). Tier 2 retries with interior anchors
  at offsets 8/16/24/32 if the leading bytes are uniform (e.g. zero
  padding). Each candidate is dtype-aligned and re-classified via
  classify_pair; only candidates that score HIGH or MED at the new
  address are reported. Distance from the original address is used
  to break ties — closer wins, capped at --relocate-max-distance
  (default 64 KB) to suppress coincidental matches in unrelated
  regions.

  The relocator finds tables that moved while keeping byte-identical
  content (the common case for cross-revision firmware where most cal
  bytes are unchanged). Tables that BOTH moved AND were recalibrated
  remain a manual-RE case.
"""

from __future__ import annotations

import argparse
import csv
import math
import struct
import sys
from collections import Counter
from pathlib import Path

# Read enough bytes to characterize the table region. Most Subaru
# tables are 16-256 bytes; 256 is a safe over-read.
SAMPLE_BYTES = 256

# Pattern-search relocation parameters
_RELOC_NEEDLE_LEN = 16          # bytes used as the "anchor" pattern
_RELOC_MAX_CANDIDATES = 32      # cap to avoid pathological scans
_RELOC_DEFAULT_MAX_DISTANCE = 0x10000  # 64 KB — most relocations are nearby

_TYPE_SIZE = {
    "uint8": 1, "int8": 1,
    "uint16_be": 2, "uint16_le": 2,
    "int16_be": 2, "int16_le": 2,
    "uint32_be": 4, "uint32_le": 4,
    "int32_be": 4, "int32_le": 4,
    "float32_be": 4, "float32_le": 4,
}


def shannon_entropy(data: bytes) -> float:
    """Bits-per-byte entropy. Random ~8.0; structured calibration
    typically 4-6; mostly-zero or mostly-FF regions much lower."""
    if not data:
        return 0.0
    counts = Counter(data)
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in counts.values() if c > 0)


def read_typed(data: bytes, dtype: str, count: int) -> list[float]:
    """Decode the first `count` typed values from `data`. Returns
    floats so caller can do shape analysis without type-switching."""
    out: list[float] = []
    size = {
        "uint8": 1, "int8": 1,
        "uint16_be": 2, "uint16_le": 2,
        "int16_be": 2, "int16_le": 2,
        "uint32_be": 4, "uint32_le": 4,
        "int32_be": 4, "int32_le": 4,
        "float32_be": 4, "float32_le": 4,
    }.get(dtype, 1)
    fmt = {
        "uint8":      "B",  "int8":       "b",
        "uint16_be":  ">H", "uint16_le":  "<H",
        "int16_be":   ">h", "int16_le":   "<h",
        "uint32_be":  ">I", "uint32_le":  "<I",
        "int32_be":   ">i", "int32_le":   "<i",
        "float32_be": ">f", "float32_le": "<f",
    }.get(dtype, "B")
    needed = size * count
    if len(data) < needed:
        return out
    for i in range(count):
        v = struct.unpack_from(fmt, data, i * size)[0]
        out.append(float(v))
    return out


def is_monotonic(values: list[float], tolerance: int = 1) -> bool:
    """Are the values monotonically increasing (with at most `tolerance`
    out-of-order pairs)? Used to detect axis tables, where the values
    must be strictly increasing for proper interpolation."""
    if len(values) < 3:
        return False
    inv = sum(1 for i in range(1, len(values))
              if values[i] <= values[i - 1])
    return inv <= tolerance


def classify_pair(sib_bytes: bytes, tgt_bytes: bytes,
                  dtype: str, count: int) -> tuple[str, str]:
    """Return (confidence, reason)."""
    if not sib_bytes:
        return ("ABSENT", "sibling ROM too short at address")
    if not tgt_bytes:
        return ("ABSENT", "target ROM too short at address")

    # Special case: if BOTH are all the same byte (0x00 or 0xFF), the
    # address is in unprogrammed flash for both — can't tell anything.
    if (len(set(sib_bytes)) == 1 and len(set(tgt_bytes)) == 1
        and sib_bytes[0] == tgt_bytes[0]):
        return ("HIGH", "both regions are padding")

    sib_ent = shannon_entropy(sib_bytes)
    tgt_ent = shannon_entropy(tgt_bytes)
    sib_vals = read_typed(sib_bytes, dtype, count)
    tgt_vals = read_typed(tgt_bytes, dtype, count)

    if not sib_vals or not tgt_vals:
        return ("LOW", "could not decode typed values")

    sib_min, sib_max = min(sib_vals), max(sib_vals)
    tgt_min, tgt_max = min(tgt_vals), max(tgt_vals)
    sib_rng = sib_max - sib_min
    tgt_rng = tgt_max - tgt_min

    # Exact-match case
    if sib_bytes == tgt_bytes:
        return ("HIGH", "byte-identical")

    # Monotonicity check (axis-like tables)
    sib_mono = is_monotonic(sib_vals)
    tgt_mono = is_monotonic(tgt_vals)
    if sib_mono and tgt_mono:
        return ("HIGH", "both monotonic (axis-like)")
    if sib_mono and not tgt_mono:
        return ("LOW", "sibling is monotonic axis; target is not")

    # Entropy comparison — values within 1.0 bit/byte = similar
    # structural complexity
    ent_diff = abs(sib_ent - tgt_ent)

    # Range comparison — for typical calibration cells, the value
    # RANGE is usually within an order of magnitude across siblings
    # (e.g. AFR target tables stay in the 12-16 range; timing tables
    # in 0-30°)
    if sib_rng == 0 and tgt_rng == 0 and sib_vals[0] == tgt_vals[0]:
        return ("HIGH", "both constant, same value")
    if sib_rng == 0 or tgt_rng == 0:
        # One side constant but not the other — moved?
        if abs((sib_vals[0] if sib_rng == 0 else 0) -
               (tgt_vals[0] if tgt_rng == 0 else 0)) < 1.0:
            return ("MED", "one side near-constant")
        return ("LOW", "one side constant, ranges diverge")

    rng_ratio = max(sib_rng, tgt_rng) / min(sib_rng, tgt_rng)

    if ent_diff < 1.0 and rng_ratio < 3.0:
        return ("MED", f"similar shape (ent Δ={ent_diff:.2f}, "
                       f"range ratio={rng_ratio:.2f})")
    if ent_diff > 2.5:
        return ("LOW", f"entropy diverges (Δ={ent_diff:.2f})")
    if rng_ratio > 10.0:
        return ("LOW", f"value range diverges ({rng_ratio:.1f}x)")
    return ("MED", f"differs (ent Δ={ent_diff:.2f}, "
                   f"range ratio={rng_ratio:.2f})")


def _find_all(haystack: bytes, needle: bytes,
              max_hits: int = _RELOC_MAX_CANDIDATES) -> list[int]:
    """All offsets where `needle` appears in `haystack`, capped.

    Uses bytes.find (C-optimized) repeatedly. Returns offsets in
    ascending order. Capped to keep pathological needles (e.g. a
    16-byte run of 0xFF that appears in every padding region) from
    causing million-hit scans.
    """
    hits: list[int] = []
    start = 0
    while len(hits) < max_hits:
        idx = haystack.find(needle, start)
        if idx == -1:
            break
        hits.append(idx)
        start = idx + 1
    return hits


def _is_anchorable(needle: bytes) -> bool:
    """Reject needles that are too uniform to anchor on.

    A needle of all-0x00, all-0xFF, or any single byte will match
    millions of positions and not actually pin a unique table. We
    require at least 4 distinct byte values to consider an anchor
    useful.
    """
    return len(set(needle)) >= 4


def relocate_low(sib_bytes: bytes, tgt_bytes: bytes, pack_address: int,
                 dtype: str, sample_count: int,
                 *, max_distance: int = _RELOC_DEFAULT_MAX_DISTANCE
                 ) -> tuple[int, str, str] | None:
    """Search target ROM for where pack's table at pack_address moved.

    Returns (new_address, confidence, reason) or None if no
    relocation candidate scored HIGH or MED at a dtype-aligned
    offset within max_distance of the original.

    Strategy is two-tier:
      1. Use sibling's first 16 bytes as a needle, find_all in tgt.
         Most tables that "moved" kept their content byte-identical;
         exact substring search catches them in O(N) with bytes.find.
      2. If the leading 16 bytes are uniform (zero padding, 0xFF
         padding, single-value table), retry with interior anchors at
         byte offsets 8/16/24/32 of the sibling slice. The candidate
         offset is then backed up by the anchor offset so it still
         points at the table's start.

    Every candidate is dtype-aligned (multiple of the data type's
    width) and self-verified via classify_pair against the sibling
    slice — only HIGH/MED candidates are kept. Ties are broken by
    confidence (HIGH > MED) then by distance from pack_address
    (closer wins). The max_distance cap rejects coincidental matches
    in unrelated ROM regions.
    """
    if pack_address < 0 or pack_address >= len(sib_bytes):
        return None

    sib_slice = sib_bytes[pack_address : pack_address + SAMPLE_BYTES]
    if len(sib_slice) < _RELOC_NEEDLE_LEN:
        return None

    type_size = _TYPE_SIZE.get(dtype, 1)

    # Try the leading anchor first, then interior anchors at typical
    # padding-skip offsets. Each entry is (anchor_offset_in_slice,
    # needle_bytes).
    anchors: list[tuple[int, bytes]] = []
    for off in (0, 8, 16, 24, 32):
        if off + _RELOC_NEEDLE_LEN > len(sib_slice):
            break
        needle = sib_slice[off : off + _RELOC_NEEDLE_LEN]
        if _is_anchorable(needle):
            anchors.append((off, needle))

    if not anchors:
        return None

    # Collect all candidate start-addresses, deduplicated.
    candidates: set[int] = set()
    for anchor_off, needle in anchors:
        for hit in _find_all(tgt_bytes, needle):
            cand_addr = hit - anchor_off
            if cand_addr < 0 or cand_addr + SAMPLE_BYTES > len(tgt_bytes):
                continue
            if cand_addr % type_size != 0:
                continue  # dtype misalignment — coincidental byte match
            if abs(cand_addr - pack_address) > max_distance:
                continue
            if cand_addr == pack_address:
                continue  # caller already classified the original
            candidates.add(cand_addr)
        if candidates:
            # Stop at the first anchor that produced any candidates —
            # leading-anchor hits are most reliable; falling through
            # to interior anchors only when the leading one finds
            # nothing.
            break

    if not candidates:
        return None

    # Score each candidate. classify_pair gives HIGH/MED/LOW; we keep
    # HIGH/MED only and prefer closer addresses on ties.
    best: tuple[int, int, int, str, str] | None = None  # (score, distance, addr, conf, reason)
    for addr in candidates:
        cand_slice = tgt_bytes[addr : addr + SAMPLE_BYTES]
        conf, reason = classify_pair(sib_slice, cand_slice, dtype, sample_count)
        if conf == "HIGH":
            score = 0
        elif conf == "MED":
            score = 1
        else:
            continue
        distance = abs(addr - pack_address)
        key = (score, distance, addr, conf, reason)
        if best is None or (score, distance) < (best[0], best[1]):
            best = key

    if best is None:
        return None

    score, distance, addr, conf, reason = best
    return (addr, conf, f"relocated +{addr - pack_address:#x} ({reason})")


def parse_pack(path: Path) -> list[dict]:
    """Minimal TOML parsing: extract table records as dicts. We
    don't need a full TOML parser — just enough to walk [[table]]
    blocks. Avoids adding a dependency."""
    text = path.read_text(encoding="utf-8")
    tables: list[dict] = []
    current: dict | None = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[[table]]"):
            if current is not None:
                tables.append(current)
            current = {}
            continue
        if s.startswith("[[") or s.startswith("["):
            # Some other section — close the current table block
            if current is not None and current:
                tables.append(current)
            current = None
            continue
        if current is None:
            continue
        if "=" not in s:
            continue
        key, _, val = s.partition("=")
        key = key.strip()
        val = val.strip()
        # Strip inline comments + quotes
        if "#" in val:
            val = val.split("#", 1)[0].strip()
        if val.startswith('"') and val.endswith('"'):
            val = val[1:-1]
        current[key] = val
    if current is not None and current:
        tables.append(current)
    return tables


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--pack",        required=True, type=Path,
                   help="Sibling pack TOML to verify against the target ROM.")
    p.add_argument("--sibling-rom", required=True, type=Path,
                   help="Decoded plaintext ROM matching the sibling pack.")
    p.add_argument("--target-rom",  required=True, type=Path,
                   help="Decoded plaintext ROM we want to localize for.")
    p.add_argument("--out-report",  type=Path,
                   help="TSV report path. Default: stdout.")
    p.add_argument("--max-tables",  type=int, default=0,
                   help="If >0, cap the number of tables checked.")
    p.add_argument("--sample-count",type=int, default=16,
                   help="Number of typed values to decode per table.")
    p.add_argument("--relocate-low", action="store_true",
                   help="For each LOW entry, search the target ROM for "
                        "the sibling's byte pattern and report relocation "
                        "candidates. Adds two columns to the TSV.")
    p.add_argument("--relocate-max-distance", type=lambda s: int(s, 0),
                   default=_RELOC_DEFAULT_MAX_DISTANCE,
                   help=f"Cap relocation candidates within ±N bytes of "
                        f"the original address (default {_RELOC_DEFAULT_MAX_DISTANCE:#x}).")
    args = p.parse_args()

    if not args.pack.is_file():
        print(f"localize: pack not found: {args.pack}", file=sys.stderr)
        return 2
    if not args.sibling_rom.is_file():
        print(f"localize: sibling ROM not found: {args.sibling_rom}",
              file=sys.stderr)
        return 2
    if not args.target_rom.is_file():
        print(f"localize: target ROM not found: {args.target_rom}",
              file=sys.stderr)
        return 2

    sib_bytes = args.sibling_rom.read_bytes()
    tgt_bytes = args.target_rom.read_bytes()
    if len(sib_bytes) != len(tgt_bytes):
        print(f"localize: WARNING — ROM sizes differ ({len(sib_bytes)} "
              f"vs {len(tgt_bytes)}); some addresses may be ABSENT.",
              file=sys.stderr)

    tables = parse_pack(args.pack)
    if args.max_tables > 0:
        tables = tables[:args.max_tables]

    header = ["id", "address_hex", "data_type", "confidence", "reason"]
    if args.relocate_low:
        header += ["relocated_to", "relocate_reason"]
    out_rows: list[list[str]] = [header]

    counts = Counter()
    reloc_counts = Counter()
    for t in tables:
        addr_s = t.get("address", "0x0")
        try:
            addr = int(addr_s, 0) if addr_s.startswith("0x") else int(addr_s)
        except ValueError:
            counts["BAD_ADDRESS"] += 1
            row = [t.get("id", "?"), addr_s, t.get("data_type", "?"),
                   "ABSENT", "address could not be parsed"]
            if args.relocate_low:
                row += ["", ""]
            out_rows.append(row)
            continue
        dtype = t.get("data_type", "uint8")

        sib_slice = sib_bytes[addr : addr + SAMPLE_BYTES]
        tgt_slice = tgt_bytes[addr : addr + SAMPLE_BYTES]
        confidence, reason = classify_pair(
            sib_slice, tgt_slice, dtype, args.sample_count)
        counts[confidence] += 1
        row = [t.get("id", "?"), addr_s, dtype, confidence, reason]

        if args.relocate_low and confidence == "LOW":
            reloc = relocate_low(
                sib_bytes, tgt_bytes, addr, dtype,
                args.sample_count,
                max_distance=args.relocate_max_distance)
            if reloc is not None:
                new_addr, new_conf, reloc_reason = reloc
                row += [f"0x{new_addr:08X}", f"{new_conf}: {reloc_reason}"]
                reloc_counts[new_conf] += 1
            else:
                row += ["", "no candidate"]
                reloc_counts["UNRELOCATED"] += 1
        elif args.relocate_low:
            row += ["", ""]

        out_rows.append(row)

    if args.out_report:
        args.out_report.parent.mkdir(parents=True, exist_ok=True)
        with args.out_report.open("w", encoding="utf-8", newline="") as f:
            w = csv.writer(f, delimiter="\t")
            w.writerows(out_rows)
        print(f"Wrote {len(out_rows) - 1} rows to {args.out_report}")
    else:
        for row in out_rows:
            print("\t".join(row))

    total = sum(counts.values())
    print()
    print("=" * 60)
    print(f"Localization summary for {args.pack.name}")
    print(f"  Target ROM: {args.target_rom.name}")
    print("=" * 60)
    print(f"  Total tables checked:  {total}")
    for label in ("HIGH", "MED", "LOW", "ABSENT", "BAD_ADDRESS"):
        n = counts.get(label, 0)
        pct = (100.0 * n / total) if total else 0.0
        print(f"  {label:<13}  {n:5d}  ({pct:5.1f}%)")
    if counts.get("LOW", 0) + counts.get("ABSENT", 0) > 0:
        print()
        print("LOW + ABSENT entries need manual review or pattern-search "
              "relocation (--relocate-low).")

    if args.relocate_low:
        total_low = counts.get("LOW", 0)
        if total_low > 0:
            print()
            print("=" * 60)
            print("Relocation pass (--relocate-low)")
            print("=" * 60)
            print(f"  LOW entries:           {total_low}")
            for label in ("HIGH", "MED", "UNRELOCATED"):
                n = reloc_counts.get(label, 0)
                pct = (100.0 * n / total_low) if total_low else 0.0
                tag = "relocated->" + label if label != "UNRELOCATED" else label
                print(f"  {tag:<22} {n:5d}  ({pct:5.1f}%)")

    return 0 if counts.get("ABSENT", 0) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
