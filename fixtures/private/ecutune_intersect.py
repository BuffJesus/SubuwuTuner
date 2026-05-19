"""
ecutune_intersect.py — generate a shopping shortlist of ECUTune CIDs
whose cipher exists in the EpifanSoft bucket.

The 2026-05-19 EP5L000J purchase revealed a gap: ECUTune sells some
CIDs whose ciphers are NOT in the EpifanSoft batch — those purchases
can't recover a keystream because the (plaintext, ciphertext) pair is
missing the ciphertext half. This tool filters ECUTune's catalog to
"actually unlockable" CIDs before any buy.

Per-CID layer note: empirically, EpifanSoft's encryption has two
generations:

  - "Older" families (LF75300, LF78001, LF9C000, LV9N100, LV9N303 and
    likely most 1MB EJ-era buckets): bucket-wide + 7-char-family
    stream cipher. One anchor unlocks N siblings sharing the 7-char
    prefix.

  - "Newer" FA-DIT families (LF79100 confirmed): bucket-wide stream
    cipher PLUS a per-CID layer concentrated in the calibration body
    (0x10000-0x60000 of the 2MB ROM). One anchor unlocks only itself
    for the calibration body; code regions still decode for siblings.

This script flags newer-firmware families (heuristic: LF79*, LF9*
prefixes excluding the 5 known-family ones) as "per-CID unlock only".

Outputs:
  ecutune_shortlist.tsv  — tab-separated table of (cid, family,
                            family_cipher_count, in_bucket, unlock_class,
                            url)
  ecutune_shortlist.md   — human-readable Markdown grouped by family
                            and ranked by unlock leverage

Usage:
    python ecutune_intersect.py
"""

from __future__ import annotations

import collections
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
INDEX = HERE / "ecutune_subaru_index.json"
OUT_TSV = HERE / "ecutune_shortlist.tsv"
OUT_MD = HERE / "ecutune_shortlist.md"

# Known-family CIDs (empirically verified to share a keystream across
# the 7-char-prefix family). Buying any one anchor in these families
# unlocks all siblings in the bucket.
FAMILY_SHARED_PREFIXES = {
    "LF75300", "LF78001", "LF9C000", "LV9N100", "LV9N303",
}

# Confirmed per-CID-only families (empirically verified that the
# anchor unlocks only itself for the calibration body region).
PER_CID_PREFIXES = {
    "LF79100",
}


def load_bucket_inventory() -> dict[int, set[str]]:
    """Return bucket_size -> {CID, ...} mapping from encrypted/ori/."""
    out: dict[int, set[str]] = collections.defaultdict(set)
    for p in ENC.glob("*_ori.hex"):
        size = p.stat().st_size
        cid = p.stem.replace("__ori", "").replace("_ori", "").upper()
        out[size].add(cid)
    return out


def load_ecutune_catalog() -> dict[str, list[str]]:
    """Return CID -> [url, ...] from the local ECUTune index."""
    out: dict[str, list[str]] = collections.defaultdict(list)
    data = json.loads(INDEX.read_text(encoding="utf-8"))
    for item in data:
        slug = item.get("slug", "")
        m = re.match(r"^([a-z0-9]{8})-", slug)
        if not m:
            continue
        cid = m.group(1).upper()
        out[cid].append(item.get("url", ""))
    return out


def unlock_class(cid: str, family_size: int) -> str:
    """Classify the expected unlock yield of buying this CID."""
    prefix7 = cid[:7]
    if prefix7 in FAMILY_SHARED_PREFIXES:
        return f"FAMILY-SHARED (verified, unlocks {family_size} CIDs)"
    if prefix7 in PER_CID_PREFIXES:
        return f"PER-CID-ONLY (verified, unlocks 1 CID)"
    # Heuristic guess for unverified families
    if cid.startswith(("LF79", "LF9D", "LF9G", "LF9L", "LV9N")) and prefix7 not in FAMILY_SHARED_PREFIXES:
        return f"LIKELY PER-CID (FA-DIT newer, untested — unlocks ~1)"
    return f"LIKELY FAMILY-SHARED (untested — unlocks up to {family_size})"


def main() -> int:
    if not INDEX.exists():
        print(f"ECUTune index not found: {INDEX}")
        return 1
    if not ENC.exists():
        print(f"Cipher bucket dir not found: {ENC}")
        return 1

    bucket_inv = load_bucket_inventory()
    cid_to_bucket: dict[str, int] = {}
    for size, cids in bucket_inv.items():
        for cid in cids:
            cid_to_bucket[cid] = size

    # Build the family-size map per bucket: for each 7-char prefix,
    # count how many CIDs in that bucket share the prefix.
    family_size: dict[int, dict[str, int]] = collections.defaultdict(
        lambda: collections.defaultdict(int))
    for size, cids in bucket_inv.items():
        for cid in cids:
            family_size[size][cid[:7]] += 1

    catalog = load_ecutune_catalog()
    rows: list[dict] = []
    for cid in sorted(catalog):
        bucket = cid_to_bucket.get(cid)
        in_bucket = bucket is not None
        fam7 = cid[:7]
        fam_count = family_size[bucket][fam7] if in_bucket else 0
        cls = unlock_class(cid, fam_count) if in_bucket else "NOT IN BUCKET (no cipher to anchor)"
        rows.append({
            "cid":          cid,
            "family7":      fam7,
            "fam_count":    fam_count,
            "bucket_size":  bucket or 0,
            "in_bucket":    in_bucket,
            "unlock_class": cls,
            "urls":         catalog[cid],
        })

    # Write TSV
    lines = ["cid\tfamily7\tfam_count\tbucket_size\tin_bucket\tunlock_class\turls"]
    for r in rows:
        lines.append(
            f"{r['cid']}\t{r['family7']}\t{r['fam_count']}\t"
            f"{r['bucket_size']}\t{r['in_bucket']}\t{r['unlock_class']}\t"
            f"{'|'.join(r['urls'])}"
        )
    OUT_TSV.write_text("\n".join(lines), encoding="utf-8")

    # Write Markdown — grouped by bucket, then by unlock class
    md = ["# ECUTune shopping shortlist (cross-referenced 2026-05-19)\n",
          "Filter rule: only CIDs whose cipher exists in the EpifanSoft "
          "bucket (`encrypted/ori/<cid>_ori.hex`) are unlock-eligible.\n",
          "Generated by `ecutune_intersect.py`.\n"]

    # Group by bucket size
    by_bucket: dict[int, list[dict]] = collections.defaultdict(list)
    for r in rows:
        by_bucket[r["bucket_size"]].append(r)

    # Eligible buckets first (sorted by size), then NOT-IN-BUCKET
    eligible_buckets = sorted(b for b in by_bucket if b > 0)
    for bucket in eligible_buckets:
        md.append(f"\n## Bucket {bucket:,}B\n")
        rows_b = sorted(by_bucket[bucket],
                        key=lambda r: (-r["fam_count"], r["cid"]))
        md.append("| CID | Family | Cipher siblings in family | Unlock class | URL |")
        md.append("|---|---|---|---|---|")
        for r in rows_b:
            url = r["urls"][0] if r["urls"] else ""
            md.append(f"| {r['cid']} | `{r['family7']}` | "
                      f"{r['fam_count']} | {r['unlock_class']} | "
                      f"<{url}> |")

    # Not in bucket — listed at the end as "do not buy" guard
    not_in = [r for r in rows if not r["in_bucket"]]
    if not_in:
        md.append(f"\n## NOT IN BUCKET — cipher missing, plaintext can't "
                  f"recover a keystream ({len(not_in)} CIDs)\n")
        md.append("These ECUTune listings can't unlock anything in the "
                  "EpifanSoft batch. Buy only as private-RE reference, "
                  "not as a decryption anchor.\n")
        md.append("| CID | URL |")
        md.append("|---|---|")
        for r in sorted(not_in, key=lambda r: r["cid"]):
            url = r["urls"][0] if r["urls"] else ""
            md.append(f"| {r['cid']} | <{url}> |")

    OUT_MD.write_text("\n".join(md), encoding="utf-8")

    # Console summary
    n_total = len(rows)
    n_eligible = sum(1 for r in rows if r["in_bucket"])
    n_not_in = n_total - n_eligible
    family_shared = sum(1 for r in rows
                        if r["in_bucket"]
                        and "FAMILY-SHARED" in r["unlock_class"])
    per_cid = sum(1 for r in rows
                  if r["in_bucket"]
                  and ("PER-CID" in r["unlock_class"]
                       or "LIKELY PER-CID" in r["unlock_class"]))
    print(f"ECUTune catalog: {n_total} CIDs")
    print(f"  in bucket (unlock-eligible): {n_eligible}")
    print(f"  not in bucket (no cipher): {n_not_in}")
    print(f"  -- of eligible, verified family-shared: "
          f"{sum(1 for r in rows if r['in_bucket'] and r['family7'] in FAMILY_SHARED_PREFIXES)}")
    print(f"  -- of eligible, verified per-CID-only: "
          f"{sum(1 for r in rows if r['in_bucket'] and r['family7'] in PER_CID_PREFIXES)}")
    print(f"  -- of eligible, likely family-shared (untested): "
          f"{family_shared - sum(1 for r in rows if r['in_bucket'] and r['family7'] in FAMILY_SHARED_PREFIXES)}")
    print(f"  -- of eligible, likely per-CID (untested FA-DIT): "
          f"{per_cid - sum(1 for r in rows if r['in_bucket'] and r['family7'] in PER_CID_PREFIXES)}")
    print()
    print(f"Wrote {OUT_TSV}")
    print(f"Wrote {OUT_MD}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
