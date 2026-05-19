"""
final_buy_direction.py
======================

Build a definitive buy-direction report:

1. Re-cluster all still-locked ciphers (4% threshold) — gives true
   keystream-cluster structure.
2. For each cluster, search:
   a. forum DB attachments → free downloads
   b. ecutune cached catalog → paid hits with USD price
3. Categorise every cluster:
   - FREE  → forum attachment available, just download
   - PAID  → ecutune has it, list cheapest URL + price
   - UNAVAILABLE → no known source (LHB, AE8M, niche stuff)
4. Emit roms_extracted/FINAL_BUY_DIRECTION.md
"""

from __future__ import annotations

import json
import re
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
PLAIN_RECOVERED = HERE / "plaintext_corpus" / "keystream-recovered"
FORUM_DB = HERE / "romraider_forum_index.sqlite"
ECUTUNE = HERE / "ecutune_subaru_index.json"
OUT = HERE / "roms_extracted" / "FINAL_BUY_DIRECTION.md"

SAMPLE_BYTES = 65536
THRESHOLD = 4.0
HEADER_BY_SIZE = {
    1_049_600: 1024, 1_311_488: 768, 1_573_632: 768, 1_573_888: 1024,
    2_098_176: 1024, 2_622_464: 1024, 4_064_000: 1024, 4_195_328: 1024,
}


class UF:
    def __init__(self, items): self.parent = {x: x for x in items}
    def find(self, x):
        while self.parent[x] != x:
            self.parent[x] = self.parent[self.parent[x]]; x = self.parent[x]
        return x
    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb: self.parent[ra] = rb


def normalize(stem): return stem.upper().replace("__ORI", "").replace("_ORI", "")


def cluster_locked(files, hdr):
    eids = sorted(normalize(p.stem) for p in files)
    samples = {normalize(p.stem): p.read_bytes()[hdr:hdr + SAMPLE_BYTES] for p in files}
    uf = UF(eids)
    for i, a in enumerate(eids):
        sa = samples[a]
        for b in eids[i + 1:]:
            sb = samples[b]
            eq = sum(1 for x, y in zip(sa, sb) if x == y)
            if 100 * eq / len(sa) >= THRESHOLD:
                uf.union(a, b)
    g = defaultdict(list)
    for e in eids: g[uf.find(e)].append(e)
    return sorted(g.values(), key=len, reverse=True)


def search_forum(con, eid):
    """Return [(filename, byte_size, topic_url)] for any matching attachment."""
    out = []
    for r in con.execute(
        """SELECT a.filename, a.byte_size, t.url FROM attachments a
           JOIN topics t ON a.t = t.t
           WHERE a.filename LIKE ? COLLATE NOCASE""",
        (f"%{eid}%",)
    ):
        out.append(r)
    return out


def search_ecutune(idx, eid):
    """Return list of {url, slug, price?} for matching products."""
    low = eid.lower()
    return [it for it in idx if it["slug"].startswith(low) or low in it["slug"]]


def main() -> int:
    recovered = set()
    if PLAIN_RECOVERED.exists():
        for p in PLAIN_RECOVERED.rglob("*.bin"):
            recovered.add(p.stem.upper())

    by_size = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)

    con = sqlite3.connect(FORUM_DB)
    ecutune_idx = json.loads(ECUTUNE.read_text(encoding="utf-8"))

    md: list[str] = []
    push = md.append
    push("# Final buy direction\n")
    push(f"- **Already recovered:** {len(recovered)} ciphers (via 5 free + 1 partial keystreams)")
    total_locked = sum(1 for p in ENC.glob("*_ori.hex") if normalize(p.stem) not in recovered)
    push(f"- **Still locked:** {total_locked} ciphers")
    push(f"- **Cluster threshold:** {THRESHOLD}% byte-equality (stricter than original 1.5%)\n")

    # For each bucket, cluster the locked files; for each cluster, resolve source.
    free_clusters: list = []   # (n, anchor, members, [forum hits], bucket_label)
    paid_clusters: list = []   # (n, anchor, members, [ecutune hits], bucket_label)
    nofind_clusters: list = [] # (n, anchor, members, bucket_label)

    for size in sorted(by_size):
        hdr = HEADER_BY_SIZE.get(size, 1024)
        locked_files = [p for p in sorted(by_size[size]) if normalize(p.stem) not in recovered]
        if not locked_files: continue
        print(f"=== bucket {size:,}B  {len(locked_files)} locked ===", flush=True)
        if len(locked_files) > 400:
            push(f"\n## Bucket {size:,}B ({len(locked_files)} locked) — too large to cluster, skipped\n")
            continue
        clusters = cluster_locked(locked_files, hdr)
        for members in clusters:
            anchor = members[0]
            forum_hits = []
            for m in members:
                forum_hits.extend((m, *h) for h in search_forum(con, m))
            ecut_hits = []
            for m in members:
                for it in search_ecutune(ecutune_idx, m):
                    ecut_hits.append((m, it))
            label = f"{size:,}B"
            if forum_hits:
                free_clusters.append((len(members), anchor, members, forum_hits, label))
            elif ecut_hits:
                paid_clusters.append((len(members), anchor, members, ecut_hits, label))
            else:
                nofind_clusters.append((len(members), anchor, members, label))

    # ---- FREE section ----
    free_clusters.sort(key=lambda x: -x[0])
    push("\n## 🟢 FREE — forum attachment available (download these next)\n")
    total_free = 0
    for n, anchor, members, hits, label in free_clusters:
        total_free += n
        push(f"\n### Cluster anchor `{anchor}` — {n} ciphers — bucket {label}")
        push(f"  Hits ({len(hits)}):")
        for m, fname, sz, url in hits[:5]:
            sz_s = f"{sz:,}B" if sz is not None else "?B"
            push(f"  - **{m}** → `{fname}` ({sz_s})  {url}")
        if len(members) > 5:
            push(f"  Cluster members ({len(members)}): {', '.join(members[:8])}{'…' if len(members) > 8 else ''}")
    push(f"\n**Free total:** ~{total_free} ciphers across {len(free_clusters)} clusters\n")

    # ---- PAID section ----
    paid_clusters.sort(key=lambda x: -x[0])
    push("\n## 💳 PAID — ecutune.shop has matching product (cheapest per cluster)\n")
    total_paid = 0
    paid_cost = 0.0
    for n, anchor, members, hits, label in paid_clusters:
        # pick cheapest hit if price is known
        priced = [(m, it) for m, it in hits if it.get("price_usd")]
        if priced:
            priced.sort(key=lambda x: x[1]["price_usd"])
            cheapest = priced[0]
            price = cheapest[1]["price_usd"]
            total_paid += n; paid_cost += price
            push(f"\n### Cluster anchor `{anchor}` — {n} ciphers — bucket {label}")
            push(f"  **Cheapest: ${price:.0f}** — `{cheapest[0]}`")
            push(f"  → {cheapest[1].get('title', cheapest[1]['slug'])}")
            push(f"  → {cheapest[1]['url']}")
            if len(hits) > 1:
                push(f"  All listings ({len(hits)}):")
                for m, it in hits[:6]:
                    p = f"${it['price_usd']:.0f}" if it.get('price_usd') else "?"
                    push(f"    - {p}  `{m}`  {it['url']}")
        else:
            # unpriced hits — list anyway
            total_paid += n
            push(f"\n### Cluster anchor `{anchor}` — {n} ciphers — bucket {label}")
            push(f"  **Pricing unknown** — hits exist but detail not fetched")
            for m, it in hits[:4]:
                push(f"    - `{m}`  {it['url']}")
    push(f"\n**Paid total:** {total_paid} ciphers for **${paid_cost:.0f}** ({len(paid_clusters)} clusters)\n")

    # ---- UNAVAILABLE section ----
    nofind_clusters.sort(key=lambda x: -x[0])
    push("\n## 🔴 UNAVAILABLE — no known source\n")
    total_nofind = 0
    singletons = []
    for n, anchor, members, label in nofind_clusters:
        total_nofind += n
        if n == 1:
            singletons.append((anchor, label)); continue
        push(f"\n### Cluster anchor `{anchor}` — {n} ciphers — bucket {label}")
        push(f"  Members: {', '.join(members[:12])}{'…' if len(members) > 12 else ''}")
    if singletons:
        push(f"\n### Singletons ({len(singletons)})")
        push(f"  Each requires its own plaintext source. Mostly low-volume cals.")
        sing_by_bucket = defaultdict(list)
        for anc, lbl in singletons: sing_by_bucket[lbl].append(anc)
        for lbl, ancs in sing_by_bucket.items():
            push(f"  - **{lbl}** ({len(ancs)}): {', '.join(ancs[:30])}{'…' if len(ancs) > 30 else ''}")
    push(f"\n**Unavailable total:** {total_nofind} ciphers\n")

    # ---- Bottom-line ----
    push("\n---\n## Bottom line\n")
    push(f"| Category | Ciphers | Cost |")
    push(f"|---|---|---|")
    push(f"| Already recovered | {len(recovered)} | $0 |")
    push(f"| Free (forum) | {total_free} | $0 |")
    push(f"| Paid (ecutune) | {total_paid} | ${paid_cost:.0f} |")
    push(f"| Unavailable | {total_nofind} | — |")
    push(f"| **Achievable total** | **{len(recovered) + total_free + total_paid}** | **${paid_cost:.0f}** |")

    OUT.write_text("\n".join(md), encoding="utf-8")
    print(f"\n→ {OUT}")
    print(f"already recovered: {len(recovered)}")
    print(f"free (forum):       {total_free}  ({len(free_clusters)} clusters)")
    print(f"paid (ecutune):    {total_paid}  for ${paid_cost:.0f}  ({len(paid_clusters)} clusters)")
    print(f"unavailable:        {total_nofind}  ({len(nofind_clusters)} clusters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
