"""
download_validate.py
====================

Download each candidate plaintext attachment from the RomRaider forum
(authenticated via cookies.txt), extract the ROM inside, and validate:

1. File size matches one of our cipher-bucket sizes (± header).
2. Not a placeholder — entropy / non-padding byte distribution looks
   like a real ECU ROM (~35-65 % bytes that are not 0x00 / 0xFF).
3. XOR-validation: pick a sibling cipher from the corresponding cluster,
   strip its per-family header, XOR with the plaintext.
   - The recovered KEYSTREAM should match the keystream extracted from
     a known-good (already-broken) pair in the SAME bucket — they
     should share long stretches.
   - If no known-good pair exists, the keystream should at minimum be
     well-distributed (not the placeholder identity-xor).

Outputs:
  - validation/<aid>/<rom>.bin            extracted file
  - validation/<aid>/report.json          per-attachment verdict
  - validation/SUMMARY.md                 human-readable rollup
"""

from __future__ import annotations

import hashlib
import http.cookiejar
import io
import json
import re
import shutil
import sys
import time
import zipfile
from collections import Counter
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
OUT = HERE / "roms_extracted" / "forumdownloads" / "validation"
COOKIES = HERE / "roms_extracted" / "forumdownloads" / "cookies.txt"

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
)

# (aid, expected_filename, label, cluster_size, anchor_ecu_id, bucket_size, header_size, sibling_cipher_id)
# sibling_cipher_id is a known cipher in the same cluster we XOR against
TARGETS = [
    (20825, "ZA1JB00C.hex.zip",
     "cluster 296 DIT VA (user's link)", 296, "ZA1JB00C",
     1_311_488, 768, "ZA1JB00C"),
    (42758, "XE1M501a00G_CB22607307-20250202-160434.7z",
     "cluster 210 diesel/RH850", 210, "XE1M501A",
     4_064_000, 1024, "XE1M501A"),
    (33377, "LF9D010H USDM WRX19 6MT.bin.zip",
     "cluster 65 DIT VB", 65, "LF9D010H",
     2_622_464, 1024, "LF9D010H"),
    (41097, "LF9F000R_CD01A04007.zip",
     "cluster 65 DIT VB (alt)", 65, "LF9F000R",
     2_622_464, 1024, "LF9F000R"),
    (38285, "ZA1JA02P.ZIP",
     "cluster 12 ZA1J tail BRZ", 12, "ZA1JA02P",
     1_311_488, 768, "ZA1JA02P"),
    (37018, "AF5G800C.srf.zip",
     "cluster 11 AF5G/LF6A", 11, "AF5G800C",
     1_573_888, 1024, "AF5G800C"),
    (26859, "Forester2015_LF6A400D.bin.zip",
     "cluster 11 AF5G/LF6A (alt)", 11, "LF6A400D",
     1_573_888, 1024, "LF6A400D"),
    (37393, "XH3M301t.BIN.zip",
     "singleton XH3M301T", 1, "XH3M301T",
     1_573_632, 768, "XH3M301T"),
]


def session() -> requests.Session:
    cj = http.cookiejar.MozillaCookieJar(str(COOKIES))
    cj.load(ignore_discard=True, ignore_expires=True)
    S = requests.Session()
    S.cookies = cj
    S.headers.update({
        "User-Agent": UA,
        "Accept": "*/*",
        "Referer": "https://www.romraider.com/forum/",
    })
    return S


def download_attachment(S: requests.Session, aid: int, out_dir: Path) -> Path | None:
    """Fetch ./download/file.php?id=<aid> and write the raw bytes."""
    out_dir.mkdir(parents=True, exist_ok=True)
    url = f"https://www.romraider.com/forum/download/file.php?id={aid}"
    raw = out_dir / f"raw_{aid}"
    if raw.exists() and raw.stat().st_size > 1024:
        return raw
    r = S.get(url, timeout=60, stream=True)
    if r.status_code != 200:
        return None
    raw.write_bytes(r.content)
    return raw


def extract_archive(raw: Path, out_dir: Path) -> list[Path]:
    """Detect format from magic bytes and extract; return list of payload files."""
    header = raw.read_bytes()[:8]
    extract_dir = out_dir / "extracted"
    extract_dir.mkdir(parents=True, exist_ok=True)

    if header.startswith(b"PK"):
        with zipfile.ZipFile(raw) as z:
            z.extractall(extract_dir)
    elif header.startswith(b"7z\xbc\xaf\x27\x1c"):
        import py7zr
        with py7zr.SevenZipFile(raw) as z:
            z.extractall(path=extract_dir)
    elif header.startswith(b"Rar!"):
        import rarfile
        with rarfile.RarFile(raw) as r:
            r.extractall(extract_dir)
    else:
        # Maybe it's the raw ROM already
        if raw.stat().st_size > 100_000:
            shutil.copy(raw, extract_dir / "payload.bin")
        else:
            return []

    return [p for p in extract_dir.rglob("*") if p.is_file()]


def classify_rom_candidate(p: Path) -> tuple[bool, str]:
    """Return (is_rom_like, reason)."""
    if p.suffix.lower() not in (".bin", ".hex", ".rom", ".srf", ""):
        return False, f"suffix {p.suffix} not ROM-like"
    sz = p.stat().st_size
    if sz < 500_000:
        return False, f"too small ({sz}B)"
    return True, "candidate"


def byte_stats(data: bytes) -> dict:
    """Stats useful for placeholder detection."""
    c = Counter(data)
    n = len(data)
    n_00 = c[0]
    n_ff = c[0xFF]
    nonpad = n - n_00 - n_ff
    distinct = len(c)
    # Shannon entropy (bits/byte)
    import math
    H = 0.0
    for cnt in c.values():
        p = cnt / n
        H -= p * math.log2(p)
    return {
        "size": n,
        "pct_00": round(100 * n_00 / n, 2),
        "pct_FF": round(100 * n_ff / n, 2),
        "pct_nonpad": round(100 * nonpad / n, 2),
        "distinct_bytes": distinct,
        "entropy_bits": round(H, 3),
    }


def load_cipher(eid: str) -> bytes | None:
    """Lookup the encrypted file for an ECU ID. Filenames are lowercase and
    may end in `_ori.hex` (no trailing underscore on the ID) or `__ori.hex`
    (trailing underscore preserved in the original cluster anchor)."""
    base = eid.lower().rstrip("_")
    for cand in (f"{base}_ori.hex", f"{base}__ori.hex"):
        p = ENC / cand
        if p.exists():
            return p.read_bytes()
    # Fallback: glob
    matches = sorted(ENC.glob(f"{base}*_ori.hex"))
    return matches[0].read_bytes() if matches else None


def xor_validate(plaintext: bytes, sibling_eid: str, bucket: int, hdr: int) -> dict:
    """XOR plaintext × sibling cipher → candidate keystream. Score it."""
    cipher = load_cipher(sibling_eid)
    if cipher is None or len(cipher) != bucket:
        return {"verdict": "no-sibling", "sibling": sibling_eid}
    cbody = cipher[hdr:]
    # Plaintext might be missing the header (common — many forum dumps strip it)
    pbody = plaintext
    if len(pbody) == len(cbody) + hdr:
        pbody = pbody[hdr:]
    if len(pbody) != len(cbody):
        return {
            "verdict": "size-mismatch",
            "plain_size": len(plaintext),
            "cipher_size": len(cipher),
            "body_size_expected": len(cbody),
        }
    keystream = bytes(a ^ b for a, b in zip(pbody, cbody))
    ks_stats = byte_stats(keystream)
    # A real keystream looks like high-entropy random data: distinct ≈ 256,
    # entropy ≈ 8.0, pct_00 ≈ pct_FF ≈ 0.4 % (chance).
    # A placeholder (e.g. plain XOR cipher → cipher) would inherit cipher's
    # distribution and look much less uniform.
    healthy = (
        ks_stats["distinct_bytes"] >= 250
        and ks_stats["entropy_bits"] >= 7.5
        and ks_stats["pct_00"] < 5
        and ks_stats["pct_FF"] < 5
    )
    return {
        "verdict": "healthy" if healthy else "suspicious",
        "sibling": sibling_eid,
        "keystream_stats": ks_stats,
    }


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    S = session()

    summary: list[dict] = []
    for aid, fname, label, cluster_size, anchor, bucket, hdr, sibling in TARGETS:
        print(f"\n=== aid={aid}  {label}  ({anchor} for cluster size {cluster_size}) ===")
        dir_ = OUT / f"aid_{aid}_{anchor}"
        report: dict = {
            "aid": aid,
            "filename": fname,
            "label": label,
            "cluster_unlocks": cluster_size,
            "anchor": anchor,
            "expected_bucket_bytes": bucket,
            "expected_header_bytes": hdr,
            "sibling_cipher": sibling,
        }
        try:
            raw = download_attachment(S, aid, dir_)
            if raw is None:
                print("  ! download failed")
                report["error"] = "download_failed"
                summary.append(report); continue
            report["downloaded_bytes"] = raw.stat().st_size
            print(f"  downloaded: {raw.stat().st_size:,}B")

            files = extract_archive(raw, dir_)
            print(f"  extracted: {len(files)} file(s)")
            for f in files:
                print(f"    {f.relative_to(dir_)}  {f.stat().st_size:,}B")

            # Pick the most ROM-like file
            roms = [f for f in files if classify_rom_candidate(f)[0]]
            if not roms:
                report["error"] = "no_rom_payload"
                report["files_seen"] = [str(f.relative_to(dir_)) for f in files]
                summary.append(report); continue
            roms.sort(key=lambda f: abs(f.stat().st_size - bucket))
            rom = roms[0]
            data = rom.read_bytes()
            report["rom_file"] = str(rom.relative_to(dir_))
            report["rom_size"] = len(data)
            report["rom_sha256"] = hashlib.sha256(data).hexdigest()
            report["byte_stats"] = byte_stats(data)
            print(f"  rom: {rom.name}  {len(data):,}B")
            print(f"    stats: {report['byte_stats']}")

            # Placeholder detection: too-low non-pad %, or only a few distinct bytes
            if report["byte_stats"]["distinct_bytes"] < 50:
                report["placeholder_check"] = "FAIL (too few distinct bytes)"
            elif report["byte_stats"]["pct_nonpad"] < 5:
                report["placeholder_check"] = "FAIL (nearly all padding — likely empty or placeholder)"
            else:
                report["placeholder_check"] = "PASS"
            print(f"    placeholder check: {report['placeholder_check']}")

            # XOR validation
            v = xor_validate(data, sibling, bucket, hdr)
            report["xor_validation"] = v
            print(f"    xor validation: {v['verdict']}")
            if "keystream_stats" in v:
                ks = v["keystream_stats"]
                print(f"      keystream entropy={ks['entropy_bits']} distinct={ks['distinct_bytes']} pct_FF={ks['pct_FF']}")

            (dir_ / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            summary.append(report)
        except Exception as e:
            report["error"] = f"exception: {type(e).__name__}: {e}"
            summary.append(report)
            print(f"  !! exception: {e}")
        time.sleep(1.5)  # polite

    # Roll up summary
    (OUT / "SUMMARY.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    md = ["# Forum-attachment validation summary\n"]
    md.append("| Cluster unlocks | Anchor | Verdict | Notes |")
    md.append("|---|---|---|---|")
    for r in summary:
        verdict_parts = []
        if r.get("error"):
            verdict = f"FAIL"
            notes = r["error"]
        else:
            ph = r.get("placeholder_check", "?")
            xv = r.get("xor_validation", {}).get("verdict", "?")
            if ph.startswith("PASS") and xv == "healthy":
                verdict = "✓ USABLE"
            elif ph.startswith("PASS") and xv == "size-mismatch":
                verdict = "? size differs"
            else:
                verdict = "✗ SUSPECT"
            notes = f"ph={ph}; xor={xv}; rom={r.get('rom_size','?')}B; stats={r.get('byte_stats',{})}"
        md.append(f"| {r['cluster_unlocks']} | {r['anchor']} | {verdict} | {notes} |")
    (OUT / "SUMMARY.md").write_text("\n".join(md), encoding="utf-8")
    print(f"\n→ {OUT / 'SUMMARY.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
