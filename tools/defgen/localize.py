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
            in the target ROM and needs to be located manually OR
            via the (future) pattern-search relocation pass.

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
draft as-is (if everything is HIGH/MED) or manually relocates the
LOW entries.

Relocation logic (pattern search for moved tables) is intentionally
NOT in this version. The MED + LOW labels surface what needs
attention; the human RE work fills in the gaps. A follow-up tool
can automate relocation for tables whose axis values give a unique
byte fingerprint.
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

    out_rows: list[list[str]] = [[
        "id", "address_hex", "data_type", "confidence", "reason"
    ]]

    counts = Counter()
    for t in tables:
        addr_s = t.get("address", "0x0")
        try:
            addr = int(addr_s, 0) if addr_s.startswith("0x") else int(addr_s)
        except ValueError:
            counts["BAD_ADDRESS"] += 1
            out_rows.append([
                t.get("id", "?"), addr_s, t.get("data_type", "?"),
                "ABSENT", "address could not be parsed"])
            continue
        dtype = t.get("data_type", "uint8")

        sib_slice = sib_bytes[addr : addr + SAMPLE_BYTES]
        tgt_slice = tgt_bytes[addr : addr + SAMPLE_BYTES]
        confidence, reason = classify_pair(
            sib_slice, tgt_slice, dtype, args.sample_count)
        counts[confidence] += 1
        out_rows.append([
            t.get("id", "?"), addr_s, dtype, confidence, reason])

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
              "relocation.")
    return 0 if counts.get("ABSENT", 0) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
