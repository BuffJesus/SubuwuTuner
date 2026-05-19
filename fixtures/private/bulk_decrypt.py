"""
bulk_decrypt.py
===============

For each confirmed plaintext attachment, recover its keystream and try to
decrypt every cipher in the same bucket. A decryption "succeeds" if the
result looks like a real ECU ROM (mostly 0xFF padding, entropy 4–7.5).

Outputs:
  roms_extracted/keystreams/<bucket>_<anchor>.ks          raw keystream bytes
  plaintext_corpus/keystream-recovered/<anchor>/<eid>.bin recovered plaintexts
  roms_extracted/bulk_decrypt_report.md                   per-cluster verdict

Never overwrites existing files. Never deletes anything. Safe to re-run.
"""

from __future__ import annotations

import json
import math
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
VAL = HERE / "roms_extracted" / "forumdownloads" / "validation"
KS_DIR = HERE / "roms_extracted" / "keystreams"
PLAIN_DIR = HERE / "plaintext_corpus" / "keystream-recovered"
REPORT = HERE / "roms_extracted" / "bulk_decrypt_report.md"

# (label, plaintext_path, primary_cipher_filename, header_size, plain_offset, bucket_size)
CONFIRMED = [
    ("ZA1JB00C_DIT_VA",
     "aid_20825_ZA1JB00C/extracted/ZA1JB00C.hex",
     "za1jb00c_ori.hex", 768, 0, 1_311_488),
    ("XE1M501A_diesel_RH850",
     "aid_42758_XE1M501A/extracted/XE1M501a00G_CB22607307-20250202-160434.bin",
     "xe1m501a__ori.hex", 1024, 16640, 4_064_000),
    ("LF9F000R_DIT_VB",
     "aid_41097_LF9F000R/extracted/LF9F000R_CD01A04007.bin",
     "lf9f000r_ori.hex", 1024, 0, 2_622_464),
    ("ZA1JA02P_ZA1J_tail",
     "aid_38285_ZA1JA02P/extracted/ZA1JA02P.bin",
     "za1ja02p_ori.hex", 768, 0, 1_311_488),
    ("LF6A400D_AF5G_LF6A",
     "aid_26859_LF6A400D/extracted/Forester2015_LF6A400D.bin",
     "lf6a400d_ori.hex", 1024, 0, 1_573_888),
]


def stats(b: bytes) -> dict:
    n = len(b); c = Counter(b)
    H = -sum((cnt / n) * math.log2(cnt / n) for cnt in c.values())
    return {
        "pct_FF": round(100 * c[0xFF] / n, 2),
        "pct_00": round(100 * c[0] / n, 2),
        "entropy": round(H, 3),
        "distinct": len(c),
    }


def looks_like_rom(b: bytes, sample: int = 65536) -> tuple[bool, dict]:
    """ROM heuristic: ≥5 % 0xFF padding AND entropy <7.5 on a 64KB sample."""
    s = stats(b[: min(sample, len(b))])
    real = s["pct_FF"] >= 3.0 and s["entropy"] <= 7.5
    return real, s


def safe_write(path: Path, data: bytes) -> str:
    """Write only if path doesn't exist. Return action taken."""
    if path.exists():
        if path.read_bytes() == data:
            return "exists-identical"
        # Don't overwrite — write to .alt
        alt = path.with_suffix(path.suffix + ".alt")
        alt.write_bytes(data)
        return f"exists-different, wrote .alt → {alt.name}"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return "written"


def main() -> int:
    KS_DIR.mkdir(parents=True, exist_ok=True)
    PLAIN_DIR.mkdir(parents=True, exist_ok=True)

    report: list[str] = ["# Bulk decryption report\n"]
    summary: list[dict] = []

    # Index ciphers by bucket size
    by_size: dict[int, list[Path]] = {}
    for p in ENC.glob("*_ori.hex"):
        by_size.setdefault(p.stat().st_size, []).append(p)
    for k in by_size:
        by_size[k].sort()

    for label, plain_rel, cipher_name, hdr, offset, bucket in CONFIRMED:
        report.append(f"\n## {label}\n")
        plain_path = VAL / plain_rel
        cipher_path = ENC / cipher_name
        if not plain_path.exists() or not cipher_path.exists():
            msg = f"  missing input: plain={plain_path.exists()} cipher={cipher_path.exists()}"
            report.append(msg); print(msg); continue

        plain = plain_path.read_bytes()
        cipher = cipher_path.read_bytes()
        body_len = len(cipher) - hdr
        if offset + body_len > len(plain):
            msg = f"  ! plaintext too short ({len(plain)}) for offset {offset} + body {body_len}"
            report.append(msg); print(msg); continue

        # Recover keystream
        cb = cipher[hdr:]
        pb = plain[offset:offset + body_len]
        keystream = bytes(a ^ b for a, b in zip(pb, cb))

        ks_path = KS_DIR / f"{bucket}_{label}.ks"
        ks_action = safe_write(ks_path, keystream)
        ks_meta = {
            "label": label,
            "bucket_size": bucket,
            "header_size": hdr,
            "body_size": body_len,
            "primary_cipher": cipher_path.name,
            "plaintext_source": str(plain_path.relative_to(HERE)),
            "plaintext_offset": offset,
            "keystream_stats": stats(keystream),
        }
        (KS_DIR / f"{bucket}_{label}.json").write_text(json.dumps(ks_meta, indent=2), encoding="utf-8")
        report.append(f"- Keystream {len(keystream):,}B saved → `{ks_path.name}` ({ks_action})")
        print(f"\n=== {label} ===")
        print(f"  keystream {len(keystream):,}B  →  {ks_path.name} ({ks_action})")

        # Try decryption on every cipher in the same bucket
        candidates = by_size.get(bucket, [])
        results = {"real": [], "random": [], "size_skip": []}
        for c_path in candidates:
            if c_path == cipher_path:
                results["real"].append(c_path.name)  # by definition
                continue
            cdata = c_path.read_bytes()
            if len(cdata) != bucket:
                results["size_skip"].append(c_path.name); continue
            body = cdata[hdr:]
            decoded = bytes(a ^ b for a, b in zip(keystream, body))
            real, s = looks_like_rom(decoded)
            if real:
                results["real"].append(c_path.name)
                # Save plaintext (use clean stem, drop the cipher's _ori suffix)
                stem = c_path.stem.replace("__ori", "").replace("_ori", "").upper()
                out_path = PLAIN_DIR / label / f"{stem}.bin"
                safe_write(out_path, decoded)
            else:
                results["random"].append((c_path.name, s["pct_FF"], s["entropy"]))

        n_real = len(results["real"])
        n_total = len(candidates)
        print(f"  decrypted: {n_real}/{n_total} ciphers in bucket")
        report.append(f"- Bucket {bucket:,}B has {n_total} ciphers; **{n_real} decrypted as real ROMs**")
        if results["real"]:
            report.append(f"  - Sample of decrypted: {', '.join(results['real'][:10])}{'…' if n_real > 10 else ''}")
        if results["random"]:
            # show worst-fitting (highest entropy = most random)
            r = sorted(results["random"], key=lambda x: -x[2])[:5]
            report.append(f"  - Non-matching (random) examples: " +
                          ", ".join(f"{n} (FF={ff}%, H={h})" for n, ff, h in r))
        summary.append({"label": label, "decrypted_count": n_real, "bucket_total": n_total,
                        "decrypted_files": results["real"]})

    REPORT.write_text("\n".join(report), encoding="utf-8")
    (KS_DIR / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n→ {REPORT}")
    total_decrypted = sum(s["decrypted_count"] for s in summary)
    print(f"\nTOTAL CIPHERS DECRYPTED: {total_decrypted}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
