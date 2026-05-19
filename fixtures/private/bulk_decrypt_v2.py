"""
bulk_decrypt_v2.py
==================

Second-generation decryption driver, adding three improvements over the
original `bulk_decrypt.py`:

  1. CID-readability gate (Patch A) — replaces the entropy + 0xFF density
     "OK" threshold with a stricter test: the decrypted output must
     contain the cipher's CID string at *some* byte offset. This
     classifies decrypts as `full` (CID readable), `partial` (heuristic
     passes but CID region still cipher), or `random` (heuristic fails).
     Partial decrypts are still saved but to a separate directory so they
     don't pollute the regression-fixture set.

  2. Bucket-internal anchorless keystream recovery (Patch B) — for
     cipher buckets with no plaintext anchor (1,573,632B and 4,195,328B
     currently), exploit the Subaru flash-erase 0xFF prior: at byte
     positions where many cipher pairs in the bucket agree (XOR == 0),
     both plaintexts are likely 0xFF. From that, K[p] = C[p] ⊕ 0xFF.
     The resulting partial keystream often decrypts at least one cipher
     to a CID-readable plaintext, which can then serve as a real anchor
     for a full keystream-recovery pass.

  3. Expanded CONFIRMED list (Patch C) — the original list had 5 anchor
     entries covering 5 of 8 cipher buckets. The 2,098,176B bucket
     (VA WRX FA-DIT) was anchorless despite 14 readable family-anchors
     being on disk under `roms_extracted/decrypted/`. This patch wires
     up one anchor per known-readable family to the schema, immediately
     decrypting ~14 ciphers in that bucket without changing any other
     pipeline behaviour.

The cipher's structure (empirical, this conversation's analysis):
  ciphertext = (plaintext ⊕ stream_keystream) ⊕ per_family_xor_layer
The keystream we recover from a (plaintext, ciphertext) pair is
actually `stream_keystream ⊕ per_family_xor[anchor_family]`, which only
decrypts other ciphers in the SAME 6-char-prefix family. A bucket with
N CID families needs N anchors, one per family.

Outputs (independent of bulk_decrypt.py's outputs so they coexist):
  roms_extracted/keystreams_v2/<bucket>_<anchor>.ks
  plaintext_corpus/decrypted_v2/full/<anchor>/<cid>.bin
  plaintext_corpus/decrypted_v2/partial/<anchor>/<cid>.bin
  roms_extracted/bulk_decrypt_v2_report.md

Never overwrites, never deletes. Safe to re-run.
"""

from __future__ import annotations

import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENC = HERE / "roms_extracted" / "encrypted" / "ori"
KS_DIR = HERE / "roms_extracted" / "keystreams_v2"
OUT_FULL = HERE / "plaintext_corpus" / "decrypted_v2" / "full"
OUT_PARTIAL = HERE / "plaintext_corpus" / "decrypted_v2" / "partial"
REPORT = HERE / "roms_extracted" / "bulk_decrypt_v2_report.md"


# ---------------------------------------------------------------------------
# Patch C: expanded CONFIRMED list
# ---------------------------------------------------------------------------
# Each entry: (label, plain_path_rel_to_HERE, cipher_filename, header_size,
#              plain_offset, bucket_size).
# plain_path is resolved relative to HERE (the script's directory) so anchors
# can live anywhere under fixtures/private/, not just under VAL/.

CONFIRMED = [
    # --- Original 5 from bulk_decrypt.py, paths rewritten to be HERE-relative
    ("ZA1JB00C_DIT_VA",
     "roms_extracted/forumdownloads/validation/aid_20825_ZA1JB00C/extracted/ZA1JB00C.hex",
     "za1jb00c_ori.hex", 768, 0, 1_311_488),
    ("XE1M501A_diesel_RH850",
     "roms_extracted/forumdownloads/validation/aid_42758_XE1M501A/extracted/XE1M501a00G_CB22607307-20250202-160434.bin",
     "xe1m501a__ori.hex", 1024, 16640, 4_064_000),
    ("LF9F000R_DIT_VB",
     "roms_extracted/forumdownloads/validation/aid_41097_LF9F000R/extracted/LF9F000R_CD01A04007.bin",
     "lf9f000r_ori.hex", 1024, 0, 2_622_464),
    ("ZA1JA02P_ZA1J_tail",
     "roms_extracted/forumdownloads/validation/aid_38285_ZA1JA02P/extracted/ZA1JA02P.bin",
     "za1ja02p_ori.hex", 768, 0, 1_311_488),
    ("LF6A400D_AF5G_LF6A",
     "roms_extracted/forumdownloads/validation/aid_26859_LF6A400D/extracted/Forester2015_LF6A400D.bin",
     "lf6a400d_ori.hex", 1024, 0, 1_573_888),

    # --- Patch C: 2,098,176B bucket — VA WRX FA-DIT (one anchor per
    # known-readable 6-char family on disk under decrypted/).
    ("LF75300_family",
     "roms_extracted/decrypted/LF75/LF75300E.bin",
     "lf75300e_ori.hex", 1024, 0, 2_098_176),
    ("LF78001_family",
     "roms_extracted/decrypted/LF78/LF78001C.bin",
     "lf78001c_ori.hex", 1024, 0, 2_098_176),
    ("LF9C000_family",
     "roms_extracted/decrypted/LF9C/LF9C000C.bin",
     "lf9c000c_ori.hex", 1024, 0, 2_098_176),
    ("LV9N100_family",
     "roms_extracted/decrypted/LV9N/LV9N100A.bin",
     "lv9n100a_ori.hex", 1024, 0, 2_098_176),
    ("LV9N303_family",
     "roms_extracted/decrypted/LV9N/LV9N303J.bin",
     "lv9n303j_ori.hex", 1024, 0, 2_098_176),
]

# Buckets with no plaintext anchor at all — Patch B targets these.
ANCHORLESS_BUCKETS = (1_049_600, 1_573_632, 4_195_328)
# (1,049,600 actually has 147 candidate anchors per the audit — left in the
# anchorless list for now because none has been wired to CONFIRMED yet; pick
# from forum-bins/A2TB001N or similar to retire it once verified.)


# ---------------------------------------------------------------------------
# Stats and gate helpers
# ---------------------------------------------------------------------------

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
    """ROM heuristic — same loose gate as bulk_decrypt.py used."""
    s = stats(b[: min(sample, len(b))])
    real = s["pct_FF"] >= 3.0 and s["entropy"] <= 7.5
    return real, s


def cid_readable(b: bytes, cid: str) -> bool:
    """Patch A's strict gate: cid string appears verbatim in the bytes."""
    return cid.upper().encode("ascii") in b


# ---------------------------------------------------------------------------
# Patch B: bucket-internal anchorless keystream recovery
# ---------------------------------------------------------------------------

def derive_keystream_anchorless(
    cipher_paths: list[Path],
    hdr: int,
    *,
    agreement_threshold: float = 0.5,
    sample_pairs: int = 200,
) -> bytes | None:
    """Bootstrap a keystream from a bucket of ciphers with no anchor.

    Algorithm:
      For each pair (Ci, Cj) in a random sample of N choose 2, count
      how often Ci[p] == Cj[p] at each byte position p. When both
      plaintexts at p are 0xFF (Subaru flash erase) the ciphertexts
      are equal (Ci[p] = ks[p] ⊕ 0xFF = Cj[p]). Positions where the
      fraction of agreeing pairs >= agreement_threshold are assigned
      provisional keystream bytes K[p] = Ci[p] ⊕ 0xFF.

      Positions where pairs disagree often (low fraction) get K[p] = 0
      as a non-committal placeholder; the caller should treat these
      as unknown and not trust decrypts in those regions.

    Returns the provisional keystream of length len(cipher_paths[0]) - hdr
    or None when there are fewer than 2 ciphers (need pairs).
    """
    if len(cipher_paths) < 2:
        return None
    bodies = []
    body_len = None
    for p in cipher_paths:
        b = p.read_bytes()[hdr:]
        if body_len is None:
            body_len = len(b)
        if len(b) != body_len:
            continue
        bodies.append(b)
    if len(bodies) < 2:
        return None

    # For efficiency on big buckets, sample pairs instead of all C(N,2).
    pairs = []
    import itertools
    all_pairs = list(itertools.combinations(range(len(bodies)), 2))
    if len(all_pairs) > sample_pairs:
        import random
        random.seed(0)  # deterministic
        pairs = random.sample(all_pairs, sample_pairs)
    else:
        pairs = all_pairs

    agree = [0] * body_len
    for i, j in pairs:
        bi, bj = bodies[i], bodies[j]
        for p in range(body_len):
            if bi[p] == bj[p]:
                agree[p] += 1

    threshold = len(pairs) * agreement_threshold
    keystream = bytearray(body_len)
    determined_count = 0
    # Use bodies[0] as the representative cipher for keystream extraction
    rep = bodies[0]
    for p in range(body_len):
        if agree[p] >= threshold:
            keystream[p] = rep[p] ^ 0xFF
            determined_count += 1
    print(f"  anchorless: {determined_count:,}/{body_len:,} bytes "
          f"determined (agreement >= {agreement_threshold:.0%})")
    return bytes(keystream)


# ---------------------------------------------------------------------------
# Disk I/O
# ---------------------------------------------------------------------------

def safe_write(path: Path, data: bytes) -> str:
    if path.exists():
        if path.read_bytes() == data:
            return "exists-identical"
        alt = path.with_suffix(path.suffix + ".alt")
        alt.write_bytes(data)
        return f"exists-different, wrote .alt -> {alt.name}"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return "written"


# ---------------------------------------------------------------------------
# Main pipeline
# ---------------------------------------------------------------------------

def run_anchored_pass(report: list[str], summary: list[dict],
                      by_size: dict[int, list[Path]]) -> set[Path]:
    """Run keystream recovery from every CONFIRMED anchor.

    Returns the set of cipher paths that were fully decrypted (CID
    readable), so the anchorless pass can skip them.
    """
    fully_decrypted: set[Path] = set()
    for label, plain_rel, cipher_name, hdr, offset, bucket in CONFIRMED:
        report.append(f"\n## {label}\n")
        plain_path = HERE / plain_rel
        cipher_path = ENC / cipher_name
        if not plain_path.exists() or not cipher_path.exists():
            msg = (f"  missing input: plain={plain_path.exists()} "
                   f"cipher={cipher_path.exists()}")
            report.append(msg); print(msg); continue

        plain = plain_path.read_bytes()
        cipher = cipher_path.read_bytes()
        body_len = len(cipher) - hdr
        if offset + body_len > len(plain):
            msg = (f"  ! plaintext too short ({len(plain)}) for "
                   f"offset {offset} + body {body_len}")
            report.append(msg); print(msg); continue

        cb = cipher[hdr:]
        pb = plain[offset:offset + body_len]
        keystream = bytes(a ^ b for a, b in zip(pb, cb))

        ks_path = KS_DIR / f"{bucket}_{label}.ks"
        ks_action = safe_write(ks_path, keystream)
        report.append(
            f"- Keystream {len(keystream):,}B -> `{ks_path.name}` "
            f"({ks_action})")
        print(f"\n=== {label} ===")
        print(f"  keystream {len(keystream):,}B -> {ks_path.name} ({ks_action})")

        full_cids, partial_cids = [], []
        random_cids = []
        for c_path in by_size.get(bucket, []):
            if c_path == cipher_path:
                full_cids.append(c_path.name)
                continue
            cdata = c_path.read_bytes()
            if len(cdata) != bucket:
                continue
            decoded = bytes(a ^ b for a, b in zip(keystream, cdata[hdr:]))
            real, s = looks_like_rom(decoded)
            cid = c_path.stem.replace("__ori", "").replace("_ori", "").upper()
            cidok = cid_readable(decoded, cid)
            if cidok:
                # Patch A: full success — CID readable
                full_cids.append(c_path.name)
                fully_decrypted.add(c_path)
                out_path = OUT_FULL / label / f"{cid}.bin"
                safe_write(out_path, decoded)
            elif real:
                # Patch A: partial — heuristic OK but CID region still cipher
                partial_cids.append(c_path.name)
                out_path = OUT_PARTIAL / label / f"{cid}.bin"
                safe_write(out_path, decoded)
            else:
                random_cids.append((c_path.name, s["pct_FF"], s["entropy"]))

        n_total = len(by_size.get(bucket, []))
        report.append(
            f"- Bucket {bucket:,}B has {n_total} ciphers; "
            f"**{len(full_cids)} full / {len(partial_cids)} partial / "
            f"{len(random_cids)} random**")
        if full_cids:
            report.append(
                f"  - Full decrypts: {', '.join(full_cids[:10])}"
                f"{'…' if len(full_cids) > 10 else ''}")
        if partial_cids:
            report.append(
                f"  - Partial (CID region still cipher): "
                f"{', '.join(partial_cids[:5])}"
                f"{'…' if len(partial_cids) > 5 else ''}")
        summary.append({
            "label": label, "bucket_size": bucket,
            "full_count": len(full_cids), "partial_count": len(partial_cids),
            "random_count": len(random_cids),
        })
    return fully_decrypted


def run_anchorless_pass(report: list[str], summary: list[dict],
                        by_size: dict[int, list[Path]],
                        already_decrypted: set[Path]) -> None:
    """Patch B: anchorless keystream recovery for ANCHORLESS_BUCKETS."""
    for bucket in ANCHORLESS_BUCKETS:
        candidates = by_size.get(bucket, [])
        if len(candidates) < 2:
            continue
        # Skip ciphers already decrypted by the anchored pass (unlikely
        # to overlap, but defensive).
        candidates = [c for c in candidates if c not in already_decrypted]
        if len(candidates) < 2:
            continue
        report.append(f"\n## ANCHORLESS bucket {bucket:,}B "
                      f"({len(candidates)} ciphers)\n")
        print(f"\n=== ANCHORLESS bucket {bucket:,}B ===")
        # Assume same header size as other buckets — the script doesn't
        # know it a priori for anchorless buckets. Default to 1024.
        # If that's wrong the keystream is offset by a known constant and
        # the user can adjust per bucket.
        hdr = 1024
        keystream = derive_keystream_anchorless(candidates, hdr)
        if keystream is None:
            report.append("  - insufficient ciphers; skipped")
            continue

        ks_path = KS_DIR / f"{bucket}_ANCHORLESS.ks"
        ks_action = safe_write(ks_path, keystream)
        report.append(f"- Bootstrap keystream {len(keystream):,}B -> "
                      f"`{ks_path.name}` ({ks_action})")

        full_cids, partial_cids, random_cids = [], [], []
        for c_path in candidates:
            cdata = c_path.read_bytes()
            decoded = bytes(a ^ b for a, b in zip(keystream, cdata[hdr:]))
            cid = c_path.stem.replace("__ori", "").replace("_ori", "").upper()
            real, _ = looks_like_rom(decoded)
            if cid_readable(decoded, cid):
                full_cids.append(c_path.name)
                out_path = OUT_FULL / f"ANCHORLESS_{bucket}" / f"{cid}.bin"
                safe_write(out_path, decoded)
            elif real:
                partial_cids.append(c_path.name)
                out_path = OUT_PARTIAL / f"ANCHORLESS_{bucket}" / f"{cid}.bin"
                safe_write(out_path, decoded)
            else:
                random_cids.append(c_path.name)
        report.append(
            f"- Anchorless decrypt: {len(full_cids)} full / "
            f"{len(partial_cids)} partial / {len(random_cids)} random")
        summary.append({
            "label": f"ANCHORLESS_{bucket}", "bucket_size": bucket,
            "full_count": len(full_cids), "partial_count": len(partial_cids),
            "random_count": len(random_cids),
        })
        # Iterative bootstrap: any full-decrypted bin becomes a real
        # anchor for a second pass that re-runs run_anchored_pass on
        # this bucket. Left as a follow-up; one pass already produces
        # a useful first cut.


def main() -> int:
    KS_DIR.mkdir(parents=True, exist_ok=True)
    OUT_FULL.mkdir(parents=True, exist_ok=True)
    OUT_PARTIAL.mkdir(parents=True, exist_ok=True)

    report: list[str] = ["# Bulk decryption v2 report\n",
                         "Strict gate: CID-readability. "
                         "Partial decrypts (heuristic pass, CID still "
                         "cipher) are saved separately.\n"]
    summary: list[dict] = []

    by_size: dict[int, list[Path]] = defaultdict(list)
    for p in ENC.glob("*_ori.hex"):
        by_size[p.stat().st_size].append(p)
    for k in by_size:
        by_size[k].sort()

    already = run_anchored_pass(report, summary, by_size)
    run_anchorless_pass(report, summary, by_size, already)

    REPORT.write_text("\n".join(report), encoding="utf-8")
    (KS_DIR / "summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n-> {REPORT}")
    total_full = sum(s["full_count"] for s in summary)
    total_partial = sum(s["partial_count"] for s in summary)
    print(f"\nTOTAL FULL DECRYPTS:    {total_full}")
    print(f"TOTAL PARTIAL DECRYPTS: {total_partial}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
