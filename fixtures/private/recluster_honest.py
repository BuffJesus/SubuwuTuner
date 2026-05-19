"""
recluster_honest.py
===================

Re-cluster the remaining LOCKED ciphers (those not decrypted by any of
our 5 confirmed keystreams) using a stricter pairwise threshold.

The original cluster_survey.py used 1.5 % byte-equality — too loose; it
chained transitively-related pairs into mega-clusters. Real keystream
siblings score 5-15 % equality consistently, so we tighten to 4 %.

For each remaining cluster, list members, suggest a buy target.
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
PLAIN_RECOVERED = HERE / "plaintext_corpus" / "keystream-recovered"
OUT = HERE / "roms_extracted" / "recluster_locked.md"

SAMPLE_BYTES = 65536
THRESHOLD = 4.0  # percent — stricter than survey's 1.5

HEADER_BY_SIZE = {
    1_049_600: 1024,
    1_311_488: 768,
    1_573_632: 768,
    1_573_888: 1024,
    2_098_176: 1024,
    2_622_464: 1024,
    4_064_000: 1024,
    4_195_328: 1024,
}


class UF:
    def __init__(self, items): self.parent = {x: x for x in items}
    def find(self, x):
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]
            x = self.parent[x]
        return x
    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb: self.parent[ra] = rb


def normalize(stem: str) -> str:
    return stem.upper().replace("__ORI", "").replace("_ORI", "")


def collect_recovered() -> set[str]:
    """ECU IDs already recovered via keystream decrypt."""
    done: set[str] = set()
    if PLAIN_RECOVERED.exists():
        for p in PLAIN_RECOVERED.rglob("*.bin"):
            done.add(p.stem.upper())
    return done


def cluster_locked_bucket(files: list[Path], hdr: int) -> list[list[str]]:
    eids = sorted(normalize(p.stem) for p in files)
    samples: dict[str, bytes] = {}
    for p in files:
        eid = normalize(p.stem)
        samples[eid] = p.read_bytes()[hdr:hdr + SAMPLE_BYTES]
    uf = UF(eids)
    n = len(eids)
    for i, a in enumerate(eids):
        sa = samples[a]
        for b in eids[i + 1:]:
            sb = samples[b]
            eq = sum(1 for x, y in zip(sa, sb) if x == y)
            pct = 100 * eq / len(sa)
            if pct >= THRESHOLD:
                uf.union(a, b)
    groups: dict[str, list[str]] = defaultdict(list)
    for e in eids:
        groups[uf.find(e)].append(e)
    return sorted(groups.values(), key=len, reverse=True)


def main() -> int:
    recovered = collect_recovered()
    print(f"recovered ECU IDs so far: {len(recovered)}")

    by_size: dict[int, list[Path]] = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)

    out: list[str] = [f"# Honest re-clustering of LOCKED ciphers (threshold {THRESHOLD}%)\n"]
    out.append(f"Already recovered: **{len(recovered)} ciphers**\n")

    grand_locked = 0
    cluster_list: list[tuple[int, str, list[str]]] = []  # (size, bucket_label, members)

    for size in sorted(by_size):
        files = sorted(by_size[size])
        hdr = HEADER_BY_SIZE.get(size, 1024)
        locked_files = [p for p in files if normalize(p.stem) not in recovered]
        if not locked_files:
            continue
        out.append(f"\n## Bucket {size:,}B  (header {hdr})  {len(locked_files)} locked of {len(files)}\n")
        print(f"\n=== {size:,}B: {len(locked_files)} locked ===")
        if len(locked_files) > 400:
            out.append("  (skipping pairwise — bucket too large)")
            continue
        clusters = cluster_locked_bucket(locked_files, hdr)
        out.append(f"{len(clusters)} sub-cluster(s) at {THRESHOLD}% threshold\n")
        for i, members in enumerate(clusters, 1):
            grand_locked += len(members)
            cluster_list.append((len(members), f"{size:,}B", members))
            tag = " (singleton)" if len(members) == 1 else ""
            out.append(f"\n### cluster {i} — {len(members)} members{tag}")
            for j in range(0, len(members), 6):
                out.append("  " + "  ".join(f"{m:<10}" for m in members[j:j + 6]))
            print(f"  cluster {i}: {len(members)} members  (anchor={members[0]})")

    out.append(f"\n---\n**Total still locked: {grand_locked} ciphers**\n")

    # Ranked buy targets (largest clusters first)
    out.append(f"\n## Ranked buy targets (large clusters first)\n")
    cluster_list.sort(key=lambda x: -x[0])
    cum = 0
    for rank, (n, bucket, members) in enumerate(cluster_list, 1):
        cum += n
        if n < 2:
            continue  # singletons grouped at end
        out.append(f"\n### #{rank}  unlocks {n} ciphers (bucket {bucket})  cumulative={cum}")
        out.append(f"  buy any ONE of:")
        for j in range(0, len(members), 6):
            out.append("    " + "  ".join(f"{m:<10}" for m in members[j:j + 6]))
    sing = [c for c in cluster_list if c[0] == 1]
    out.append(f"\n### singletons ({len(sing)})")
    out.append(f"  Each requires its own purchase: {', '.join(m[0] for _, _, m in sing[:30])}{'...' if len(sing) > 30 else ''}")

    OUT.write_text("\n".join(out), encoding="utf-8")
    print(f"\n→ {OUT}")
    print(f"grand total still locked: {grand_locked}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
