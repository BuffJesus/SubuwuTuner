"""
keystream_analyze.py — multi-anchor agreement analysis for a cipher bucket.

For a bucket whose keystreams have been recovered from multiple anchors,
characterize the per-family layer structure:

  - Pairwise byte-equality rate per 64KB chunk (where do families agree?)
  - Bucket-wide consensus positions (where ALL anchors agree on the
    keystream byte; those positions decrypt any cipher in the bucket)
  - Per-pair byte-value histograms (does the XOR distribution show
    algebraic structure?)

Reports findings to stdout. Doesn't modify any decrypt outputs.

Usage:
    python keystream_analyze.py --bucket 2098176
"""

from __future__ import annotations

import argparse
import collections
import itertools
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KS_DIR = HERE / "roms_extracted" / "keystreams_v2"


def load_keystreams_for_bucket(bucket: int) -> dict[str, bytes]:
    """All recovered keystreams for the given bucket size."""
    streams: dict[str, bytes] = {}
    for f in KS_DIR.glob(f"{bucket}_*.ks"):
        if f.stem.endswith("_ANCHORLESS_partial"):
            continue
        if "_bootstrap_" in f.stem:
            continue
        name = (f.stem.replace(f"{bucket}_", "")
                       .replace("_family", "")
                       .replace("_DIT_VA", "")
                       .replace("_DIT_VB", "")
                       .replace("_diesel_RH850", "")
                       .replace("_ZA1J_tail", "")
                       .replace("_AF5G_LF6A", ""))
        streams[name] = f.read_bytes()
    return streams


def chunk_agreement(streams: dict[str, bytes], chunk: int = 65536) -> None:
    """Report per-chunk pairwise agreement rates."""
    items = list(streams.items())
    if len(items) < 2:
        print("  (need >= 2 keystreams for pairwise analysis)")
        return
    body_len = min(len(s) for s in streams.values())
    print(f"\n=== Per-chunk pairwise byte-equality rate "
          f"(across {len(items)} keystreams):\n")
    print(f"{'Offset':<12} {'AvgAgree':>9} {'MinAgree':>9} {'MaxAgree':>9}")
    for offset in range(0, body_len, chunk):
        rates = []
        for (_, s1), (_, s2) in itertools.combinations(items, 2):
            z = sum(1 for a, b in zip(s1[offset:offset+chunk],
                                       s2[offset:offset+chunk]) if a == b)
            rates.append(z / min(chunk, body_len - offset))
        avg = sum(rates) / len(rates)
        verdict = ""
        if avg > 0.5:
            verdict = "AGREE STRONG"
        elif avg > 0.05:
            verdict = "mixed"
        elif avg > 0.005:
            verdict = "near random"
        print(f"0x{offset:08X}  {avg:>8.2%}  {min(rates):>8.2%}  "
              f"{max(rates):>8.2%}  {verdict}")


def per_pair_chunk_breakdown(streams: dict[str, bytes], chunk: int = 65536,
                             min_agree: float = 0.05) -> None:
    """For each pair, list chunks where they agree >= min_agree."""
    items = list(streams.items())
    if len(items) < 2:
        return
    body_len = min(len(s) for s in streams.values())
    print(f"\n=== Per-pair agreement, chunks with >= {min_agree:.0%}:\n")
    for (n1, s1), (n2, s2) in itertools.combinations(items, 2):
        hits = []
        for offset in range(0, body_len, chunk):
            z = sum(1 for a, b in zip(s1[offset:offset+chunk],
                                       s2[offset:offset+chunk]) if a == b)
            rate = z / min(chunk, body_len - offset)
            if rate >= min_agree:
                hits.append((offset, rate))
        if hits:
            chunks_str = ", ".join(f"0x{o:08X}({r:.0%})" for o, r in hits)
            print(f"  {n1:10} XOR {n2:10}: {chunks_str}")


def unanimous_consensus(streams: dict[str, bytes]) -> bytes:
    """Build a 'bucket consensus' keystream: at each byte position, if
    ALL keystreams agree on the byte value, use it; else 0. The
    consensus is bucket-wide (no per-family layer) and decrypts those
    positions for any cipher in the bucket.
    """
    if not streams:
        return b""
    items = list(streams.values())
    body_len = min(len(s) for s in items)
    out = bytearray(body_len)
    determined = 0
    for p in range(body_len):
        b = items[0][p]
        if all(s[p] == b for s in items[1:]):
            out[p] = b
            determined += 1
    print(f"\nConsensus: {determined:,}/{body_len:,} bytes "
          f"({determined*100/body_len:.2f}%) where all "
          f"{len(streams)} keystreams agree byte-for-byte")
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(prog="keystream_analyze")
    ap.add_argument("--bucket", type=int, required=True,
                    help="Bucket size in bytes (e.g. 2098176 for VA WRX)")
    ap.add_argument("--write-consensus", action="store_true",
                    help="Write the bucket consensus keystream to "
                         "KS_DIR/<bucket>_consensus.ks for downstream use.")
    args = ap.parse_args()

    streams = load_keystreams_for_bucket(args.bucket)
    if not streams:
        print(f"No keystreams found for bucket {args.bucket:,}",
              file=sys.stderr)
        return 1
    print(f"Loaded {len(streams)} keystreams for bucket "
          f"{args.bucket:,}B:")
    for n in streams:
        print(f"  {n}")

    chunk_agreement(streams)
    per_pair_chunk_breakdown(streams)
    consensus = unanimous_consensus(streams)

    if args.write_consensus and consensus:
        out_path = KS_DIR / f"{args.bucket}_CONSENSUS.ks"
        out_path.write_bytes(consensus)
        print(f"\nConsensus keystream written: {out_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
