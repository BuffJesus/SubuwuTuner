"""
batch_harvest.py
================

Pipeline for batch-recovering all forum-available keystreams:

1. Re-cluster all still-locked ciphers (4 % threshold).
2. For each cluster, find every forum attachment whose filename
   contains any cluster member ECU ID.
3. For each attachment (newest first), download + extract + try to
   align with the cluster's primary cipher at multiple offsets.
   Validate: keystream must decrypt the primary AND ≥1 sibling cipher
   to something that looks like a real ROM (entropy ≤ 7.5).
4. First attachment that passes: save keystream, bulk-decrypt every
   cipher in the bucket that matches, save plaintexts. Move to next
   cluster.
5. Per-cluster verdict written incrementally to batch_progress.jsonl
   so progress is preserved across restarts.

Polite: 1.5 s between forum downloads. Resumable (skips cached files).
"""

from __future__ import annotations

import hashlib
import http.cookiejar
import json
import math
import shutil
import sqlite3
import sys
import time
import zipfile
from collections import Counter, defaultdict
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
PLAIN_RECOVERED = HERE / "plaintext_corpus" / "keystream-recovered"
KS_DIR = HERE / "roms_extracted" / "keystreams"
DL_CACHE = HERE / "roms_extracted" / "forumdownloads" / "batch"
PROGRESS = HERE / "roms_extracted" / "batch_progress.jsonl"
COOKIES = HERE / "roms_extracted" / "forumdownloads" / "cookies.txt"
FORUM_DB = HERE / "romraider_forum_index.sqlite"

SAMPLE_BYTES = 65536
THRESHOLD = 4.0
HEADER_BY_SIZE = {
    1_049_600: 1024, 1_311_488: 768, 1_573_632: 768, 1_573_888: 1024,
    2_098_176: 1024, 2_622_464: 1024, 4_064_000: 1024, 4_195_328: 1024,
}
SKIP_EXTENSIONS = {".xml", ".xls", ".xlsx", ".pdf", ".png", ".jpg", ".jpeg",
                   ".gif", ".txt", ".log", ".doc", ".docx", ".cmt"}
OFFSETS_TO_TRY = (0, 768, 1024, 4096, 16640, 16384, 8192, 2048, 512)


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


def stats(b: bytes, sample: int = SAMPLE_BYTES) -> dict:
    b = b[:sample]; n = len(b)
    if n == 0: return {"pct_FF": 0.0, "entropy": 0.0, "distinct": 0}
    c = Counter(b)
    H = -sum((cnt / n) * math.log2(cnt / n) for cnt in c.values())
    return {"pct_FF": round(100 * c[0xFF] / n, 2),
            "entropy": round(H, 3),
            "distinct": len(c)}


def looks_like_rom(b: bytes) -> bool:
    s = stats(b)
    return s["entropy"] <= 7.5 and s["distinct"] >= 200


def session() -> requests.Session:
    cj = http.cookiejar.MozillaCookieJar(str(COOKIES))
    cj.load(ignore_discard=True, ignore_expires=True)
    S = requests.Session()
    S.cookies = cj
    S.headers.update({
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
        "Referer": "https://www.romraider.com/forum/",
    })
    return S


def download(S: requests.Session, aid: int) -> Path | None:
    DL_CACHE.mkdir(parents=True, exist_ok=True)
    p = DL_CACHE / f"raw_{aid}"
    if p.exists() and p.stat().st_size > 1024:
        return p
    try:
        r = S.get(f"https://www.romraider.com/forum/download/file.php?id={aid}",
                  timeout=60)
        if r.status_code != 200 or len(r.content) < 1024:
            return None
        p.write_bytes(r.content)
        time.sleep(1.5)  # polite
        return p
    except Exception as e:
        print(f"    !! download err aid={aid}: {e}", flush=True)
        return None


def extract_payload(raw: Path) -> Path | None:
    """Extract archive, return path of best ROM-candidate file."""
    hd = raw.read_bytes()[:8]
    ed = raw.parent / f"ext_{raw.stem}"
    if not ed.exists():
        ed.mkdir(parents=True)
        try:
            if hd.startswith(b"PK"):
                with zipfile.ZipFile(raw) as z: z.extractall(ed)
            elif hd.startswith(b"7z\xbc\xaf\x27\x1c"):
                import py7zr
                with py7zr.SevenZipFile(raw) as z: z.extractall(path=ed)
            elif hd.startswith(b"Rar!"):
                import rarfile
                with rarfile.RarFile(raw) as r: r.extractall(ed)
            else:
                # Maybe raw bin
                if raw.stat().st_size > 100_000:
                    shutil.copy(raw, ed / "payload.bin")
                else:
                    return None
        except Exception as e:
            print(f"    !! extract err {raw.name}: {e}", flush=True)
            return None
    files = [p for p in ed.rglob("*") if p.is_file()
             and p.suffix.lower() not in SKIP_EXTENSIONS]
    if not files: return None
    # Pick largest candidate that looks ROM-like (size > 100K)
    files = [f for f in files if f.stat().st_size >= 100_000]
    if not files: return None
    files.sort(key=lambda f: -f.stat().st_size)
    return files[0]


def find_cipher(eid: str) -> Path | None:
    """Find encrypted file for ECU ID, handling __ori / _ori suffixes."""
    base = eid.lower().rstrip("_")
    for cand in (f"{base}_ori.hex", f"{base}__ori.hex"):
        p = ENC / cand
        if p.exists(): return p
    matches = sorted(ENC.glob(f"{base}*_ori.hex"))
    return matches[0] if matches else None


def cluster_locked_bucket(files: list[Path], hdr: int) -> list[list[str]]:
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


def try_validate(plain: bytes, primary: Path, sibling: Path,
                 hdr: int) -> int:
    """Try every offset. Return offset on success, -1 on fail.
    Success = both primary decode and sibling decode look like ROMs."""
    pdata = primary.read_bytes()
    sdata = sibling.read_bytes()
    body_len = len(pdata) - hdr
    cb = pdata[hdr:]; sb = sdata[hdr:]
    for off in OFFSETS_TO_TRY:
        if off + body_len > len(plain): continue
        pb = plain[off:off + body_len]
        ks = bytes(a ^ b for a, b in zip(pb, cb))
        prim_decoded = bytes(a ^ b for a, b in zip(ks, cb))
        if not looks_like_rom(prim_decoded): continue
        N = min(len(ks), len(sb))
        sib_decoded = bytes(a ^ b for a, b in zip(ks[:N], sb[:N]))
        if looks_like_rom(sib_decoded):
            return off
    return -1


def bulk_decrypt(keystream: bytes, bucket: int, hdr: int,
                 dest_label: str, all_files: list[Path]) -> int:
    PLAIN_RECOVERED.mkdir(exist_ok=True, parents=True)
    n = 0
    for c_path in all_files:
        if c_path.stat().st_size != bucket: continue
        body = c_path.read_bytes()[hdr:]
        decoded = bytes(a ^ b for a, b in zip(keystream, body))
        if not looks_like_rom(decoded): continue
        n += 1
        stem = normalize(c_path.stem)
        out = PLAIN_RECOVERED / dest_label / f"{stem}.bin"
        if out.exists(): continue
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(decoded)
    return n


def already_recovered() -> set[str]:
    out: set[str] = set()
    if PLAIN_RECOVERED.exists():
        for p in PLAIN_RECOVERED.rglob("*.bin"):
            out.add(p.stem.upper())
    return out


def log_progress(rec: dict) -> None:
    with PROGRESS.open("a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")


def already_processed_clusters() -> set[str]:
    """Re-load completed cluster anchors from progress log."""
    done: set[str] = set()
    if PROGRESS.exists():
        for line in PROGRESS.read_text(encoding="utf-8").splitlines():
            try:
                r = json.loads(line)
                if r.get("outcome") in ("USABLE", "SINGLETON", "NO_VALID_PLAINTEXT",
                                        "ALREADY_DONE", "NO_HITS"):
                    done.add(r["anchor"])
            except Exception:
                pass
    return done


def main() -> int:
    S = session()
    con = sqlite3.connect(FORUM_DB)
    recovered = already_recovered()
    done_clusters = already_processed_clusters()
    print(f"recovered ECU IDs already on disk: {len(recovered)}")
    print(f"clusters already processed (per progress log): {len(done_clusters)}")

    # Re-cluster all still-locked
    by_size = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)

    work: list[tuple[int, str, list[str], int, int, list[Path]]] = []
    for size in sorted(by_size):
        hdr = HEADER_BY_SIZE.get(size, 1024)
        locked = [p for p in sorted(by_size[size]) if normalize(p.stem) not in recovered]
        if not locked or len(locked) > 400:
            continue
        print(f"  bucket {size:,}B: {len(locked)} locked, re-clustering …", flush=True)
        for members in cluster_locked_bucket(locked, hdr):
            anchor = members[0]
            work.append((len(members), anchor, members, size, hdr, by_size[size]))
    work.sort(key=lambda x: -x[0])

    total_new_decrypted = 0
    cluster_idx = 0
    for n, anchor, members, size, hdr, bucket_files in work:
        cluster_idx += 1
        if anchor in done_clusters:
            print(f"\n[{cluster_idx}/{len(work)}] cluster {anchor} ({n}) — SKIP (already processed)")
            continue
        print(f"\n[{cluster_idx}/{len(work)}] cluster {anchor} ({n} ciphers, bucket {size:,}B)", flush=True)
        primary = find_cipher(anchor)
        if not primary:
            print(f"  ! no primary cipher for anchor {anchor}, skipping")
            log_progress({"anchor": anchor, "size_cluster": n, "outcome": "NO_PRIMARY"})
            continue
        # Pick a sibling (different from primary) — try ANY other cluster member
        sibling = None
        for m in members:
            if m == anchor: continue
            cand = find_cipher(m)
            if cand and cand != primary:
                sibling = cand; break

        # Gather forum hits for this cluster — newest first by aid
        hits: list[tuple[int, str]] = []
        seen_fnames: set[str] = set()
        for m in members:
            for r in con.execute(
                """SELECT a.aid, a.filename FROM attachments a
                   WHERE a.filename LIKE ? COLLATE NOCASE""",
                (f"%{m}%",)
            ):
                aid, fname = r
                if fname in seen_fnames: continue
                # Skip non-ROM extensions
                ext = Path(fname).suffix.lower()
                if ext in SKIP_EXTENSIONS: continue
                seen_fnames.add(fname)
                hits.append((aid, fname))
        if not hits:
            print(f"  no forum hits for cluster {anchor}")
            log_progress({"anchor": anchor, "size_cluster": n, "outcome": "NO_HITS"})
            continue
        hits.sort(key=lambda t: -t[0])  # newest first
        print(f"  {len(hits)} attachment candidates")

        won = False
        for aid, fname in hits[:8]:  # try up to 8 attachments per cluster
            print(f"    try aid={aid}  {fname[:60]}", flush=True)
            raw = download(S, aid)
            if not raw: continue
            payload = extract_payload(raw)
            if not payload:
                print(f"      no payload")
                continue
            plain = payload.read_bytes()
            if len(plain) < (size - hdr) * 0.5:
                print(f"      payload too small ({len(plain):,}B)")
                continue
            # First try strict (sibling-validated) — confirms shared keystream
            off = try_validate(plain, primary, sibling, hdr) if sibling else -1
            singleton_only = False
            if off < 0:
                # Fall back to self-validation only (recovers just the primary,
                # useful for buckets where ECU IDs don't share keystreams)
                pdata = primary.read_bytes(); body_len = len(pdata) - hdr; cb = pdata[hdr:]
                for o in OFFSETS_TO_TRY:
                    if o + body_len > len(plain): continue
                    pb = plain[o:o + body_len]
                    ks = bytes(a ^ b for a, b in zip(pb, cb))
                    prim_decoded = bytes(a ^ b for a, b in zip(ks, cb))
                    if looks_like_rom(prim_decoded):
                        off = o; singleton_only = True; break
            if off < 0:
                print(f"      no offset validates")
                continue
            # Bulk-decrypt
            pdata = primary.read_bytes()
            pb = plain[off:off + len(pdata) - hdr]
            ks = bytes(a ^ b for a, b in zip(pb, pdata[hdr:]))
            label = f"{anchor}_{size}"
            ks_path = KS_DIR / f"{size}_{anchor}.ks"
            if not ks_path.exists():
                ks_path.write_bytes(ks)
            decrypted = bulk_decrypt(ks, size, hdr, label, bucket_files)
            total_new_decrypted += decrypted
            tag = "SINGLETON" if singleton_only else "USABLE"
            print(f"      ✓ {tag} @ offset={off}  → {decrypted} ciphers recovered")
            log_progress({
                "anchor": anchor, "size_cluster": n, "outcome": tag,
                "winning_aid": aid, "winning_filename": fname,
                "offset": off, "decrypted_count": decrypted,
            })
            # For SINGLETON outcomes, keep iterating through remaining attachments
            # in the same "cluster" — each new ECU ID is its own singleton unlock
            if not singleton_only:
                won = True
                break
            # Track that we got at least one
            won = True
        if not won:
            print(f"  ✗ all {min(8, len(hits))} attempts failed for cluster {anchor}")
            log_progress({"anchor": anchor, "size_cluster": n, "outcome": "NO_VALID_PLAINTEXT",
                          "attempts_tried": min(8, len(hits))})

    print(f"\n\n=== batch harvest done. new ciphers decrypted: {total_new_decrypted} ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
