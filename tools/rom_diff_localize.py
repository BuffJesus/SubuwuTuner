#!/usr/bin/env python3
"""
rom_diff_localize.py — locate likely calibration-table addresses by
diffing two plaintext Subaru ROMs of the same CID.

Hypothesis: tuners edit tables to tune. So a contiguous region of
differing bytes between a stock ROM and a tuned variant is, with
high prior, a calibration table. The output is a sorted list of
edit clusters with address + length + byte-edit-count, useful as a
shopping list for either:

  - hand-seeding [[table]] entries in a definition pack (raw FA-engine
    research where no RomRaider XML exists)
  - cross-referencing against an existing RR XML to spot tables the
    XML mis-addresses or omits

Edit clusters do NOT carry scaling / axis / units / dimensions. That
work is per-table research starting from the address. This tool
narrows the search space from "1 MiB of opaque flash" to "a few dozen
candidate addresses."

Usage:
    python tools/rom_diff_localize.py STOCK.bin TUNED.bin [options]

Options:
    --min-run N       Drop individual edited byte runs shorter than N (default 4)
    --merge-gap N     Cluster edit runs separated by ≤N unchanged bytes (default 64)
    --top N           Print only the N largest clusters (default: all)
    --json FILE       Emit machine-readable JSON alongside the text report
    --validate-pack TOML
                      Cross-check detected clusters against [[table]] addresses
                      in an existing pack. Prints overlap statistics.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, asdict
from pathlib import Path


@dataclass
class Cluster:
    """One contiguous candidate-table region."""
    start: int
    length: int
    edited_bytes: int

    @property
    def end(self) -> int:
        return self.start + self.length

    @property
    def density(self) -> float:
        """Fraction of bytes in [start, end) that actually differ."""
        return self.edited_bytes / self.length if self.length else 0.0


def find_edit_runs(a: bytes, b: bytes, min_run: int):
    """Yield (offset, length) for each contiguous run where a[i] != b[i],
    filtered to runs of length >= min_run."""
    n = min(len(a), len(b))
    i = 0
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            if j - i >= min_run:
                yield (i, j - i)
            i = j
        else:
            i += 1


def cluster_runs(runs, merge_gap: int) -> list[Cluster]:
    """Merge runs separated by ≤merge_gap unchanged bytes into a single
    Cluster. Tracks the edited-byte count vs cluster span so density
    can rank tables (high-density = tight edit, low-density = sparse
    or possibly a structural change with mixed unchanged bytes)."""
    clusters: list[Cluster] = []
    for off, length in runs:
        if clusters and off - clusters[-1].end <= merge_gap:
            last = clusters[-1]
            new_end = off + length
            clusters[-1] = Cluster(
                start=last.start,
                length=new_end - last.start,
                edited_bytes=last.edited_bytes + length,
            )
        else:
            clusters.append(Cluster(start=off, length=length, edited_bytes=length))
    return clusters


def parse_pack_table_addresses(pack_path: Path) -> list[tuple[str, int]]:
    """Extract [[table]] id + address pairs from a SubuwuTuner TOML pack.
    Quick regex pull — we don't need a full TOML parser for this, just
    the `id` + `address` fields. Returns (id, address) pairs."""
    text = pack_path.read_text(encoding="utf-8", errors="replace")
    # Walk [[table]] blocks
    tables: list[tuple[str, int]] = []
    blocks = re.split(r"^\[\[table\]\]\s*$", text, flags=re.MULTILINE)
    for block in blocks[1:]:  # skip preamble
        # Stop at next top-level section header
        block = re.split(r"^\[", block, flags=re.MULTILINE)[0]
        m_id = re.search(r"^\s*id\s*=\s*\"([^\"]+)\"", block, re.MULTILINE)
        m_addr = re.search(r"^\s*address\s*=\s*(0x[0-9a-fA-F]+|\d+)",
                           block, re.MULTILINE)
        if m_id and m_addr:
            addr = int(m_addr.group(1), 0)
            tables.append((m_id.group(1), addr))
    return tables


def overlap(cluster: Cluster, table_addr: int) -> bool:
    """A pack table address lies inside the cluster's address range."""
    return cluster.start <= table_addr < cluster.end


def fmt_size(n: int) -> str:
    if n < 1024:
        return f"{n}B"
    if n < 1024 * 1024:
        return f"{n/1024:.1f}KB"
    return f"{n/(1024*1024):.2f}MB"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Diff two ROMs of the same CID; report likely "
                    "calibration-table address regions.")
    ap.add_argument("stock", type=Path, help="Stock ROM path (.bin/.hex/raw)")
    ap.add_argument("tuned", type=Path, help="Tuned ROM path")
    ap.add_argument("--min-run", type=int, default=4,
                    help="Skip edited runs shorter than this (default 4)")
    ap.add_argument("--merge-gap", type=int, default=64,
                    help="Cluster runs separated by <=N unchanged bytes "
                         "(default 64)")
    ap.add_argument("--top", type=int, default=None,
                    help="Print only the top N clusters by size")
    ap.add_argument("--json", type=Path, default=None,
                    help="Also emit a JSON report at this path")
    ap.add_argument("--validate-pack", type=Path, default=None,
                    help="TOML pack to cross-check against — prints "
                         "precision/recall of cluster→table overlap")
    args = ap.parse_args(argv)

    stock = args.stock.read_bytes()
    tuned = args.tuned.read_bytes()
    if len(stock) != len(tuned):
        print(f"warning: ROM sizes differ ({len(stock)} vs {len(tuned)} "
              f"bytes); diff truncated to smaller", file=sys.stderr)

    runs = list(find_edit_runs(stock, tuned, args.min_run))
    total_edited = sum(length for _, length in runs)
    clusters = cluster_runs(runs, args.merge_gap)
    clusters.sort(key=lambda c: c.length, reverse=True)

    print(f"# rom_diff_localize report")
    print(f"# stock:        {args.stock} ({len(stock)} bytes)")
    print(f"# tuned:        {args.tuned} ({len(tuned)} bytes)")
    print(f"# min-run:      {args.min_run}    merge-gap: {args.merge_gap}")
    print(f"# edited runs:  {len(runs)} (total {total_edited} bytes)")
    print(f"# clusters:     {len(clusters)}")
    print()

    display = clusters if args.top is None else clusters[:args.top]
    print(f"{'address':>10}  {'length':>8}  {'edited':>8}  {'density':>7}")
    for c in display:
        print(f"  0x{c.start:08X}  {fmt_size(c.length):>8}  "
              f"{c.edited_bytes:>8}  {c.density:>6.1%}")
    if args.top is not None and len(clusters) > args.top:
        print(f"  ... {len(clusters) - args.top} more not shown")

    # Validation against an existing pack
    if args.validate_pack:
        print()
        print(f"# validating against pack: {args.validate_pack}")
        tables = parse_pack_table_addresses(args.validate_pack)
        print(f"# pack [[table]] entries: {len(tables)}")
        hit_tables = []
        miss_tables = []
        for table_id, addr in tables:
            if any(overlap(c, addr) for c in clusters):
                hit_tables.append((table_id, addr))
            else:
                miss_tables.append((table_id, addr))
        # For each cluster, did at least one documented table fall inside it?
        labeled = []
        unlabeled = []
        for c in clusters:
            matches = [tid for tid, addr in tables if overlap(c, addr)]
            (labeled if matches else unlabeled).append((c, matches))

        recall = len(hit_tables) / len(tables) if tables else 0.0
        precision = len(labeled) / len(clusters) if clusters else 0.0
        print(f"# pack-table recall:    {len(hit_tables)}/{len(tables)} "
              f"= {recall:.1%}  (documented tables that fell into a cluster)")
        print(f"# cluster precision:    {len(labeled)}/{len(clusters)} "
              f"= {precision:.1%}  (clusters containing >=1 documented table)")
        print(f"# unlabeled clusters:   {len(unlabeled)}  "
              f"(candidate-table addresses NOT in the pack — fresh research targets)")

        if labeled:
            print()
            print("Labeled clusters (cluster -> tables it contains):")
            for c, ids in sorted(labeled, key=lambda x: -x[0].length):
                shown = ", ".join(ids[:3])
                more = "" if len(ids) <= 3 else f" (+{len(ids)-3} more)"
                print(f"  0x{c.start:08X}  {fmt_size(c.length):>7}  "
                      f"density={c.density:>5.1%}  ->  {shown}{more}")

        if unlabeled:
            print()
            print("Top 10 unlabeled clusters (sorted by size — candidates for "
                  "tables the pack omits):")
            for c, _ in sorted(unlabeled, key=lambda x: -x[0].length)[:10]:
                print(f"  0x{c.start:08X}  {fmt_size(c.length):>7}  "
                      f"edited={c.edited_bytes}  density={c.density:.1%}")

    if args.json:
        out = {
            "stock_path": str(args.stock),
            "tuned_path": str(args.tuned),
            "stock_size": len(stock),
            "tuned_size": len(tuned),
            "min_run": args.min_run,
            "merge_gap": args.merge_gap,
            "total_edited_bytes": total_edited,
            "clusters": [asdict(c) for c in clusters],
        }
        args.json.write_text(json.dumps(out, indent=2), encoding="utf-8")
        print(f"\n# wrote JSON: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
