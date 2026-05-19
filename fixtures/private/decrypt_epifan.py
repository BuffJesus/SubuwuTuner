"""
decrypt_epifan.py
=================

Use plaintext ROMs from bludgod/RomRaider as known-plaintext oracles to derive
per-family keystreams, cluster the epifan archive, and bulk-decrypt every cipher
file that's in a cluster we can crack.

Strategy:
  1. Catalog plaintexts (bludgod) and ciphers (roms_extracted/*_ori.hex).
  2. For each ECU-ID match where cipher_size - plain_size in {768, 1024}:
       a. Verify the plaintext looks like a real SH-2A ROM (vector table).
       b. Derive keystream = cipher[hdr:] XOR plaintext.
  3. For each derived keystream, find every other cipher in the same size bucket
     whose cipher bytes match heavily in known-empty (0xFF/0x00) regions —
     those share the keystream.
  4. Decrypt every clustered cipher.

Outputs go to roms_extracted/decrypted/<ECU_ID>.bin. Never touches the originals.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path
from collections import defaultdict

HERE = Path(__file__).resolve().parent
ENCRYPTED_DIR = HERE / "roms_extracted" / "encrypted" / "ori"
BLUDGOD_DIR = HERE / "plaintext_corpus" / "bludgod-roms"
FORUM_BINS_DIR = HERE / "plaintext_corpus" / "forum-bins"
JIMIHIMI_DIR = HERE / "plaintext_corpus" / "jimihimi-extras"
OUT_DIR = HERE / "roms_extracted" / "decrypted"
SUMMARY_PATH = OUT_DIR / "_decryption_summary.txt"


def entropy(b: bytes) -> float:
    if not b:
        return 0.0
    f = [0] * 256
    for x in b:
        f[x] += 1
    n = len(b)
    return -sum((c / n) * math.log2(c / n) for c in f if c > 0)


def is_real_subaru_rom(plain: bytes) -> tuple[bool, str]:
    """Heuristic check: real Subaru ROM has a plausible 32-bit BE vector table
    at offset 0, non-trivial entropy (~5-7 bits/byte), and < ~25% pure-padding."""
    if len(plain) < 256:
        return False, "too small"
    sp = int.from_bytes(plain[0:4], "big")
    rv = int.from_bytes(plain[4:8], "big")
    # SH-2A: stack pointer typically points into RAM (0xFFFF8000 region) or another
    # valid address; reset vector points into flash (low addresses, e.g. 0x00xxxx).
    # If both are 0x00000000, this is a placeholder.
    if sp == 0 and rv == 0:
        return False, "vector table all-zero (placeholder)"
    # Reject placeholders where the first 8 bytes are ASCII text (typically the
    # ECU ID embedded by someone uploading a request rather than a real dump).
    # Real SH-2A vector entries have the high byte either 0x00 (flash) or 0xFF.
    if all(0x20 <= b < 0x7f for b in plain[:8]):
        return False, "first 8 bytes are ASCII (placeholder with embedded ECU ID)"
    # Also reject if the high byte of SP or RV is in the ASCII range (suggests
    # an embedded string rather than an address).
    if 0x20 <= (sp >> 24) < 0x7f or 0x20 <= (rv >> 24) < 0x7f:
        return False, f"vector bytes look like ASCII (sp=0x{sp:08x} rv=0x{rv:08x})"
    pad_pct = 100 * (plain.count(0xFF) + plain.count(0)) / len(plain)
    e = entropy(plain[: min(len(plain), 65536)])
    return True, f"sp=0x{sp:08x} rv=0x{rv:08x} pad={pad_pct:.0f}% ent={e:.2f}"


_FILENAME_ID_RE = re.compile(r"^([A-Za-z][A-Za-z0-9]{5,8})(?:[-_.].*)?\.(?:hex|HEX|Hex|bin|BIN|Bin)$")


def catalog_plaintexts(*roots: Path) -> dict[str, Path]:
    """Map ECU_ID (upper) -> plaintext path. Filename starts with the ECU ID
    followed by '-', '_', '.' or extension. Walks each given root recursively."""
    out: dict[str, Path] = {}
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if not p.is_file():
                continue
            m = _FILENAME_ID_RE.match(p.name)
            if not m:
                continue
            eid = m.group(1).upper()
            if 6 <= len(eid) <= 9:
                # Don't overwrite a bludgod source with a possibly-placeholder forum file
                if eid not in out:
                    out[eid] = p
    return out


def catalog_ciphers(root: Path) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for p in root.glob("*_ori.hex"):
        eid = p.stem.replace("_ori", "").upper()
        out[eid] = p
    return out


def hdr_size(cipher_len: int, plain_len: int) -> int | None:
    delta = cipher_len - plain_len
    return delta if delta in (768, 1024) else None


def derive_keystreams(plaintexts: dict[str, Path], ciphers: dict[str, Path]):
    """Yield (eid, hdr, keystream, plaintext_sample) for each verified pair."""
    matches = sorted(set(plaintexts) & set(ciphers))
    for eid in matches:
        plain = plaintexts[eid].read_bytes()
        cipher = ciphers[eid].read_bytes()
        hdr = hdr_size(len(cipher), len(plain))
        if hdr is None:
            continue
        ok, why = is_real_subaru_rom(plain)
        if not ok:
            print(f"  SKIP {eid}: {why}", file=sys.stderr)
            continue
        ks = bytes(c ^ p for c, p in zip(cipher[hdr:], plain))
        yield eid, hdr, ks, plain


def cluster_zero_rate(cipher_a: bytes, cipher_b: bytes, hdr: int,
                       window_bytes: int = 8192) -> float:
    """Return byte-equality % between two cipher bodies. Two files share a
    keystream iff they have noticeable equality at the 0x2000-aligned all-empty
    flash regions. Sample over 64 KiB to keep it fast."""
    a = cipher_a[hdr:hdr + 65536]
    b = cipher_b[hdr:hdr + 65536]
    if not a or not b:
        return 0.0
    eq = sum(1 for x, y in zip(a, b) if x == y)
    return 100 * eq / min(len(a), len(b))


def try_decrypt_head(cipher_body: bytes, ks: bytes, n: int = 65536) -> tuple[float, bytes]:
    """Decrypt the first n bytes with keystream ks. Return (entropy, head)."""
    if len(ks) < n or len(cipher_body) < n:
        n = min(len(ks), len(cipher_body))
    head = bytes(c ^ k for c, k in zip(cipher_body[:n], ks[:n]))
    return entropy(head), head


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--min-zero-pct", type=float, default=1.5,
                    help="Cipher-byte equality pct threshold for cluster membership")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    plaintexts = catalog_plaintexts(BLUDGOD_DIR, FORUM_BINS_DIR, JIMIHIMI_DIR)
    ciphers = catalog_ciphers(ENCRYPTED_DIR)
    print(f"plaintexts: {len(plaintexts)}  ciphers: {len(ciphers)}")

    # Group ciphers by size for cluster search
    by_size: dict[int, list[Path]] = defaultdict(list)
    for p in ciphers.values():
        by_size[p.stat().st_size].append(p)
    for sz, files in sorted(by_size.items()):
        print(f"  cipher size bucket {sz:>10,}: {len(files):>4} files")

    keystreams: list[tuple[str, int, bytes, bytes]] = []
    for eid, hdr, ks, plain in derive_keystreams(plaintexts, ciphers):
        keystreams.append((eid, hdr, ks, plain))
        ok, why = is_real_subaru_rom(plain)
        print(f"  derived KS from {eid} (hdr={hdr}, plain={len(plain):,}, {why})")
    print(f"\n{len(keystreams)} usable keystreams.\n")

    if not keystreams:
        print("No usable keystreams. Aborting.", file=sys.stderr)
        return 1

    # Index keystreams by size bucket. Each keystream candidate only applies to
    # ciphers whose body size matches the plaintext's.
    ks_by_bucket: dict[int, list[tuple[str, int, bytes]]] = defaultdict(list)
    for eid, hdr, ks, plain in keystreams:
        ks_by_bucket[len(plain) + hdr].append((eid, hdr, ks))

    # Per-cipher: try every applicable keystream, pick the one with lowest entropy
    # of the decrypted head (lowest = most plaintext-like). Threshold 7.5 bits/byte.
    summary_lines: list[str] = []
    decrypted_count = 0
    ENT_THRESHOLD = 7.5
    HEAD_PROBE = 65536

    for size_bucket, files in sorted(by_size.items()):
        ks_candidates = ks_by_bucket.get(size_bucket, [])
        if not ks_candidates:
            print(f"\n[size {size_bucket:,}] no plaintext yet ({len(files)} files skipped)")
            continue
        print(f"\n[size {size_bucket:,}] trying {len(ks_candidates)} keystream(s) against {len(files)} ciphers")
        for cp in files:
            cid = cp.stem.replace("_ori", "").upper()
            cb = cp.read_bytes()
            best: tuple[float, str, int, bytes] | None = None
            for eid, hdr, ks in ks_candidates:
                body = cb[hdr:hdr + len(ks)]
                if len(body) < HEAD_PROBE:
                    continue
                e, _head = try_decrypt_head(body, ks, HEAD_PROBE)
                if best is None or e < best[0]:
                    best = (e, eid, hdr, ks)
            if best is None:
                continue
            ent_h, eid_src, hdr, ks = best
            if ent_h >= ENT_THRESHOLD:
                summary_lines.append(f"FAIL  {cid:<10}  best_ent={ent_h:.3f}  src={eid_src}")
                continue
            # Full decrypt — write into a family subdir (4-char prefix)
            body = cb[hdr:hdr + len(ks)]
            decrypted = bytes(b ^ k for b, k in zip(body, ks))
            family_dir = OUT_DIR / cid[:4].upper()
            family_dir.mkdir(exist_ok=True)
            out_path = family_dir / f"{cid}.bin"
            out_path.write_bytes(decrypted)
            ok, why = is_real_subaru_rom(decrypted)
            tag = "OK" if ok else "??"
            summary_lines.append(f"{tag}  {cid:<10}  ks={eid_src}  ent={ent_h:.2f}  {why if ok else ''}")
            decrypted_count += 1
        print(f"  decrypted in bucket so far: total={decrypted_count}")

    SUMMARY_PATH.write_text("\n".join(summary_lines) + "\n", encoding="utf-8")
    print(f"\nDone. {decrypted_count} files decrypted to {OUT_DIR}")
    print(f"Summary: {SUMMARY_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
