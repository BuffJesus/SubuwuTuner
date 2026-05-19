"""
ecutune_match.py
================

1) Walk /downloads/category/subaru/ pages — collect every product URL + slug.
2) Slug looks like "<ecuid>_<calid>-<flavor>-<chk>" (lowercase). First
   underscore-delimited or dash-delimited token starts with the ECU ID.
3) Cross-reference with shopping_list anchors to find which clusters have
   ecutune coverage.
4) Fetch detail page (title + price) ONLY for matching products.
5) Emit ecutune_matches.md: per cluster, the cheapest available product.
"""

from __future__ import annotations

import json
import re
import sys
import time
from collections import defaultdict
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
INDEX = HERE / "ecutune_subaru_index.json"
MATCHES = HERE / "roms_extracted" / "ecutune_matches.md"

sys.path.insert(0, str(HERE))
from cluster_survey import (  # type: ignore
    ENC, BLUDGOD, FORUM_BINS, JIMIHIMI,
    HEADER_BY_SIZE, cluster_bucket,
)

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
)
S = requests.Session()
S.headers.update({"User-Agent": UA, "Accept": "text/html,application/xhtml+xml"})

CATEGORIES = [
    "subaru",
    "subaru/immo-off-subaru",
    "subaru/impreza-denso",
    "subaru/xv-denso",
    "subaru/outback-denso-diesel",
    "cvt-subaru",
    "cvt-subaru/cvt-legacy-outback",
]


def get(url: str) -> str:
    r = S.get(url, timeout=30)
    r.raise_for_status()
    return r.text


def extract_price_usd(html: str) -> float | None:
    m = re.search(r'data-price="([0-9.]+)"', html)
    if m:
        return float(m.group(1))
    m = re.search(r'"price":"?([0-9.]+)"?,\s*"priceCurrency":"USD"', html)
    return float(m.group(1)) if m else None


def extract_title(html: str) -> str:
    m = re.search(r"<title>([^<]+)</title>", html)
    return (m.group(1) if m else "").replace("&#8226;", "•").strip()


def walk_category(cat: str) -> list[dict]:
    base = f"https://ecutune.shop/downloads/category/{cat}/"
    out: list[dict] = []
    seen: set[str] = set()
    page = 1
    while page <= 100:
        url = base if page == 1 else f"{base}page/{page}/"
        try:
            html = get(url)
        except requests.HTTPError as e:
            if e.response is not None and e.response.status_code == 404:
                break
            print(f"  ! {url} → {e}")
            break
        links = re.findall(r'href="(https://ecutune\.shop/downloads/[^/"]+/)"', html)
        new = [u for u in dict.fromkeys(links) if u not in seen]
        if not new:
            break
        for u in new:
            seen.add(u)
            slug = u.rsplit("/", 2)[-2]
            out.append({"url": u, "slug": slug, "category": cat})
        print(f"  {cat} p{page}: +{len(new)} (total {len(out)})", flush=True)
        page += 1
        time.sleep(2.5)
    return out


def harvest_index() -> list[dict]:
    out: list[dict] = []
    if INDEX.exists():
        out = json.loads(INDEX.read_text(encoding="utf-8"))
        print(f"loaded cached index: {len(out)} products")
    seen: set[str] = {it["url"] for it in out}
    cats_done = {it["category"] for it in out}
    for cat in CATEGORIES:
        if cat in cats_done and cat == "subaru" and len(seen) < 200:
            # subaru main got cut off at p17 — let it continue
            pass
        print(f"=== {cat} ===")
        new_this_cat = 0
        for item in walk_category(cat):
            if item["url"] not in seen:
                out.append(item)
                seen.add(item["url"])
                new_this_cat += 1
        print(f"  category {cat}: added {new_this_cat} new")
        INDEX.write_text(json.dumps(out, indent=2), encoding="utf-8")
    return out


def build_anchor_clusters() -> list[tuple[int, list[str]]]:
    """Re-derive the LOCKED clusters from the existing survey logic."""
    have_plain: set[str] = set()
    for root in (BLUDGOD, FORUM_BINS, JIMIHIMI):
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if p.is_file() and p.suffix.lower() in (".bin", ".hex", ".rom"):
                eid = p.stem.upper().split("-")[0].split("_")[0][:8]
                have_plain.add(eid)
    by_size: dict[int, list[Path]] = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)
    clusters: list[tuple[int, list[str]]] = []
    for size in sorted(by_size):
        files = sorted(by_size[size])
        hdr = HEADER_BY_SIZE.get(size, 1024)
        for members in cluster_bucket(files, hdr).values():
            if not any(eid in have_plain for eid in members):
                clusters.append((len(members), members))
    clusters.sort(key=lambda x: -x[0])
    return clusters


def slug_first_token(slug: str) -> str:
    """ECU IDs sit at the start of the slug, uppercase."""
    head = re.split(r"[_-]", slug, maxsplit=1)[0]
    return head.upper()


def main() -> int:
    idx = harvest_index()
    print(f"\ntotal products indexed: {len(idx)}")

    # Map ecu_id (uppercase first token) → list of products
    by_ecu: dict[str, list[dict]] = defaultdict(list)
    for it in idx:
        by_ecu[slug_first_token(it["slug"])].append(it)
    print(f"unique ecu-id heads: {len(by_ecu)}")

    clusters = build_anchor_clusters()
    print(f"locked clusters: {len(clusters)}")

    # For each cluster, find which members ecutune offers, fetch detail, pick cheapest
    out: list[str] = []
    push = out.append
    push("# ecutune.shop matches for locked clusters\n")
    push("Each cluster: every member ECU ID that ecutune sells, with the cheapest")
    push("listing per ID. Buying ONE recovers the cluster keystream → unlocks all")
    push("N ciphers in the cluster.\n")
    push("Pricing observed: simple E2 deletes ≈ $50, Stage1 ≈ $60, Stage1+E2 ≈ $80.\n")

    grand_total_price = 0.0
    grand_total_unlocks = 0

    for rank, (count, members) in enumerate(clusters, 1):
        push(f"\n## Cluster #{rank} — unlocks **{count}** ciphers\n")
        # For each member ID, look up product(s)
        hits: list[tuple[str, dict, float]] = []  # (eid, product, price)
        for eid_raw in members:
            eid = eid_raw.rstrip("_")
            products = by_ecu.get(eid, [])
            # Also try prefix match (some slugs concat ECU+CAL with no separator)
            if not products:
                low = eid.lower()
                products = [it for it in idx if it["slug"].startswith(low)]
            for p in products:
                # Fetch detail if missing
                if "price_usd" not in p:
                    try:
                        h = get(p["url"])
                    except Exception as e:
                        p["price_usd"] = None
                        p["title"] = f"(fetch err: {e})"
                        continue
                    p["price_usd"] = extract_price_usd(h)
                    p["title"] = extract_title(h)
                    time.sleep(0.8)
                if p.get("price_usd"):
                    hits.append((eid, p, float(p["price_usd"])))
        # Save updated index periodically
        INDEX.write_text(json.dumps(idx, indent=2), encoding="utf-8")

        if not hits:
            push(f"  No ecutune coverage — none of the {len(members)} member IDs found.")
            push(f"  Members: {', '.join(members[:12])}{'...' if len(members) > 12 else ''}")
            continue

        # Sort cheapest first
        hits.sort(key=lambda t: t[2])
        cheapest = hits[0]
        push(f"  Cheapest: **${cheapest[2]:.0f}** — `{cheapest[0]}`")
        push(f"  → {cheapest[1]['title']}")
        push(f"  → {cheapest[1]['url']}\n")
        grand_total_price += cheapest[2]
        grand_total_unlocks += count

        # All listings for that cluster
        push(f"  All listings ({len(hits)} for {len(set(h[0] for h in hits))} ECU IDs):")
        for eid, p, price in hits:
            push(f"    - ${price:.0f}  `{eid}`  {p['url']}")

    push(f"\n---\n")
    push(f"**Total cost to unlock everything ecutune covers: ${grand_total_price:.0f}**")
    push(f"**Total ciphers unlocked: {grand_total_unlocks}**")
    push(f"**Per-cipher cost: ${grand_total_price / max(grand_total_unlocks, 1):.2f}**")

    MATCHES.write_text("\n".join(out), encoding="utf-8")
    INDEX.write_text(json.dumps(idx, indent=2), encoding="utf-8")
    print(f"\nwrote {MATCHES}")
    print(f"total cost: ${grand_total_price:.0f}  →  {grand_total_unlocks} ciphers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
