"""
cluster_survey.py
=================

For each cipher-size bucket in roms_extracted/encrypted/ori/, perform a
pairwise byte-equality scan over a 64 KB sample of each file's body
(stripped of the per-family header) and group files into keystream clusters
via union-find. Files within the same keystream share large stretches of
identical cipher bytes wherever both plaintexts have empty-flash padding
(0xFF or 0x00), so the cipher-XOR-cipher rate jumps to ~5-15% inside a
cluster vs. ~0.4% (chance) across clusters.

Output: for each bucket, list every cluster sorted by size, naming one
representative ECU ID per cluster. Useful for picking the highest-return
ecutune purchase — the ECU ID anchoring the biggest cluster unlocks the
most cipher files with a single known-plaintext acquisition.

We also flag which clusters are ALREADY broken (have a corresponding
plaintext in plaintext_corpus/) so we know where to spend money.

Defaults: 64 KB sample, equality-rate threshold 1.5%, header sizes 768 or
1024 (auto-detected from existing keystreams).
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
BLUDGOD = HERE / "plaintext_corpus" / "bludgod-roms"
FORUM_BINS = HERE / "plaintext_corpus" / "forum-bins"
JIMIHIMI = HERE / "plaintext_corpus" / "jimihimi-extras"

SAMPLE_BYTES = 65536
CLUSTER_THRESHOLD = 1.5  # percent

# Header per size bucket: determined by existing plaintext pairs
HEADER_BY_SIZE = {
    1_049_600: 1024,
    1_311_488: 768,
    1_573_632: 768,
    1_573_888: 1024,
    2_098_176: 1024,
    2_622_464: 1024,  # guess; no plaintext yet
    4_064_000: 1024,
    4_195_328: 1024,
}


class UF:
    """Tiny union-find."""

    def __init__(self, items: list):
        self.parent = {x: x for x in items}

    def find(self, x):
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[ra] = rb


def load_samples(bucket_files: list[Path], hdr: int) -> dict[str, bytes]:
    samples: dict[str, bytes] = {}
    for p in bucket_files:
        eid = p.stem.replace("_ori", "").upper()
        data = p.read_bytes()
        samples[eid] = data[hdr:hdr + SAMPLE_BYTES]
    return samples


def cluster_bucket(bucket_files: list[Path], hdr: int) -> dict[int, list[str]]:
    samples = load_samples(bucket_files, hdr)
    eids = sorted(samples)
    uf = UF(eids)
    # Pairwise compare. For 350 files this is 350*349/2 ≈ 61K comparisons,
    # each one zips two 64KB chunks — feasible in a couple minutes per bucket.
    for i, a in enumerate(eids):
        sa = samples[a]
        for b in eids[i + 1:]:
            sb = samples[b]
            eq = sum(1 for x, y in zip(sa, sb) if x == y)
            pct = 100 * eq / len(sa)
            if pct >= CLUSTER_THRESHOLD:
                uf.union(a, b)
    # Collect clusters
    groups: dict[str, list[str]] = defaultdict(list)
    for e in eids:
        groups[uf.find(e)].append(e)
    # Re-key by integer index for stable output
    sized = sorted(groups.values(), key=len, reverse=True)
    return {i: g for i, g in enumerate(sized)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--size", type=int, nargs="*", help="Only survey these sizes")
    ap.add_argument("--threshold", type=float, default=CLUSTER_THRESHOLD)
    args = ap.parse_args()

    # Catalog ciphers by size
    by_size: dict[int, list[Path]] = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)
    # Catalog plaintexts (just ECU IDs)
    have_plain: set[str] = set()
    for root in (BLUDGOD, FORUM_BINS, JIMIHIMI):
        for p in root.rglob("*"):
            if p.is_file() and p.suffix.lower() in (".bin", ".hex", ".rom"):
                eid = p.stem.upper().split("-")[0].split("_")[0][:8]
                have_plain.add(eid)

    print(f"Plaintexts cataloged: {len(have_plain)}")
    print(f"Cipher size buckets: {len(by_size)}")
    print()

    targets = sorted(by_size) if not args.size else sorted(args.size)
    for size in targets:
        files = sorted(by_size.get(size, []))
        if not files:
            continue
        hdr = HEADER_BY_SIZE.get(size, 1024)
        print(f"=== size {size:,} bytes  (header {hdr})  {len(files)} files ===")
        clusters = cluster_bucket(files, hdr)
        for i, members in clusters.items():
            broken = any(eid in have_plain for eid in members)
            tag = "[BROKEN]" if broken else "[LOCKED]"
            anchor = members[0]
            print(f"  {tag} cluster #{i:<2} size={len(members):>3}  "
                  f"anchor={anchor}  members={members[:8]}"
                  f"{'...' if len(members) > 8 else ''}")
        # Suggest a single highest-leverage acquisition target (largest LOCKED)
        locked = [m for m in clusters.values()
                  if not any(eid in have_plain for eid in m)]
        if locked:
            best = max(locked, key=len)
            print(f"  -> top buy target: any of {best[:3]}{'...' if len(best) > 3 else ''} "
                  f"unlocks {len(best)} ciphers")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
