"""
checksum_discover.py — empirical discovery of Subaru ROM checksum
byte locations.

Why
---
src/flash/include/st/flash/checksum.hpp ships a clean seam for three
Subaru checksum algorithms (subaru_std, subaru_alt, subaru_alt2) but
every concrete impl is a NotImplemented stub. Before the algorithm
can land, we need to know:
  1. Where in the ROM the checksum bytes live (offset + length)
  2. What input bytes feed the checksum (which calibration regions)
  3. What the algorithm shape is (sum / xor / CRC / poly / etc.)

This tool answers (1) by comparing sibling decoded ROMs from
fixtures/private/roms_extracted/decrypted/. Sibling ROMs from the
same family (LF79xxx, LF9Cxxx, etc.) differ only in their
calibration content + their checksum bytes. By diffing two siblings
byte-by-byte, the resulting "changed offsets" set is mostly
calibration; the checksum bytes are a SMALL SUBSET that change in
correlation with the calibration changes.

The heuristic:
  - For each family with 2+ decoded ROMs, diff every pair
  - Build a histogram: offset -> how many pairs differ at this offset
  - Bytes that change in ~every pair are either:
       (a) calibration cells that always differ across siblings
       (b) the checksum slot
  - The checksum slot is distinguished by being a SMALL CONTIGUOUS
    REGION (typically 2-8 bytes) at a known low-frequency-of-change
    position (often near 0x10000 or at the END of the calibration
    region — Subaru convention)
  - This tool emits candidates sorted by their "checksum-likeness"
    score, ranked highest first

Output is a TSV report per family at
`fixtures/private/checksum_discovery/<family>_candidates.tsv`.
Manual review picks the actual slot from the top N candidates.

This is a discovery tool, not a validator. Once we know the byte
location + a few stock examples, the algorithm-shape investigation
(probably manual, possibly assisted by a brute-force-over-known-
algorithms helper) is a separate phase.
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from itertools import combinations
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECRYPTED = ROOT / "fixtures" / "private" / "roms_extracted" / "decrypted"
BLUDGOD = ROOT / "fixtures" / "private" / "plaintext_corpus" / "bludgod-roms"
OUTDIR = ROOT / "fixtures" / "private" / "checksum_discovery"


def load_families() -> dict[str, list[Path]]:
    """Group decoded ROMs by 4-character family prefix.

    Scans two corpora:
      - `fixtures/private/roms_extracted/decrypted/<FAMILY>/*.bin`
        (our pak-decode outputs, named like LF79103P.bin)
      - `fixtures/private/plaintext_corpus/bludgod-roms/*/*/CID-*.hex`
        (community bludgod corpus — .hex extension but raw binary)
    """
    fams: dict[str, list[Path]] = defaultdict(list)

    # Pak-decode corpus
    if DECRYPTED.is_dir():
        for sub in DECRYPTED.iterdir():
            if not sub.is_dir():
                continue
            for f in sub.glob("*.bin"):
                stem = f.stem.upper()
                fams[stem[:4]].append(f)

    # Bludgod corpus — files named like
    # A2WC400H-2005-ADM-Subaru-Forester-XT-AT.hex; first 4 chars = family
    if BLUDGOD.is_dir():
        for f in BLUDGOD.rglob("*.hex"):
            stem = f.stem.upper()
            # Some bludgod filenames start with a letter+digit pattern; take
            # the first 4 chars regardless of separator
            family = stem[:4]
            # Guard against the .git directory's pack/idx files
            if "/.git/" in str(f).replace("\\", "/"):
                continue
            fams[family].append(f)

    # De-dup paths in case a ROM appears in both corpora (unlikely but
    # cheap to defend).
    for k in fams:
        seen: set[Path] = set()
        unique: list[Path] = []
        for p in fams[k]:
            r = p.resolve()
            if r not in seen:
                seen.add(r)
                unique.append(p)
        fams[k] = unique

    return fams


def diff_pair(a_bytes: bytes, b_bytes: bytes) -> list[int]:
    """Return the offsets where two ROMs differ. ROMs must be the
    same length (caller's responsibility)."""
    out: list[int] = []
    for i, (x, y) in enumerate(zip(a_bytes, b_bytes)):
        if x != y:
            out.append(i)
    return out


def score_runs(offsets: set[int], rom_size: int) -> list[tuple[int, int, int]]:
    """Group contiguous offsets into runs. Returns list of
    (run_start, run_length, distance_from_typical_checksum_zone)
    sorted ascending by length (short = checksum-like).

    Subaru convention: checksums tend to live near the calibration
    region boundary. For 2 MB FA-DIT ROMs, that's often around
    0x10000 or near 0xE0000. We don't strictly enforce this; just
    let the run-length filter do the work and let manual review
    pick from the top candidates.
    """
    if not offsets:
        return []
    sorted_offs = sorted(offsets)
    runs: list[tuple[int, int]] = []
    start = sorted_offs[0]
    prev = start
    for o in sorted_offs[1:]:
        if o == prev + 1:
            prev = o
        else:
            runs.append((start, prev - start + 1))
            start = o
            prev = o
    runs.append((start, prev - start + 1))

    # Add a distance-from-zone score: closer to 0x10000 OR
    # rom_size - 0x20000 = "checksum-like-zone" by Subaru habit
    def dist_score(start: int) -> int:
        target_a = 0x10000
        target_b = rom_size - 0x20000 if rom_size > 0x20000 else 0
        return min(abs(start - target_a), abs(start - target_b))

    return [(s, n, dist_score(s)) for s, n in runs]


def family_report(family: str, roms: list[Path], outpath: Path) -> dict:
    """Build the per-family TSV report. Returns summary stats."""
    # Group by ROM size — checksums are size-dependent, can't compare
    # cross-size.
    by_size: dict[int, list[Path]] = defaultdict(list)
    for r in roms:
        by_size[r.stat().st_size].append(r)

    report_lines: list[str] = []
    report_lines.append(
        "size_bytes\tn_roms\trun_start_hex\trun_length\tdist_score\tlikelihood"
    )

    total_groups = 0
    total_candidates = 0

    for size, group in sorted(by_size.items()):
        if len(group) < 2:
            continue
        total_groups += 1
        # Histogram of differing offsets across all pairs.
        diff_count: defaultdict[int, int] = defaultdict(int)
        n_pairs = 0
        for a, b in combinations(group, 2):
            a_bytes = a.read_bytes()
            b_bytes = b.read_bytes()
            if len(a_bytes) != len(b_bytes):
                continue
            for off in diff_pair(a_bytes, b_bytes):
                diff_count[off] += 1
            n_pairs += 1
        if n_pairs == 0:
            continue

        # Bytes that differ in EVERY pair are the strongest candidates.
        # Calibration cells often differ in only SOME pairs; checksum
        # bytes differ in nearly every pair (because almost any cal
        # change forces a checksum recompute).
        always_differing = {
            o for o, c in diff_count.items() if c == n_pairs
        }

        runs = score_runs(always_differing, size)
        # Sort by length ascending (shorter = more checksum-like),
        # then by dist_score ascending (closer to common checksum zones).
        runs.sort(key=lambda r: (r[1], r[2]))

        # Likelihood label: very short runs (2-8 bytes) get HIGH;
        # 9-32 LOW; longer NOISE
        for start, length, dist in runs[:20]:
            if 2 <= length <= 8:
                like = "HIGH"
            elif length <= 32:
                like = "LOW"
            else:
                like = "NOISE"
            report_lines.append(
                f"{size}\t{len(group)}\t0x{start:08X}\t{length}\t{dist}\t{like}"
            )
            total_candidates += 1

    if total_candidates > 0:
        outpath.write_text("\n".join(report_lines), encoding="utf-8")

    return {
        "family": family,
        "total_roms": len(roms),
        "size_groups": total_groups,
        "candidates": total_candidates,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--families", default="",
                   help="Comma-separated 4-char family prefixes "
                        "(default: every family with 2+ ROMs)")
    p.add_argument("--min-roms", type=int, default=2,
                   help="Skip families with fewer ROMs than this.")
    p.add_argument("--out-dir", type=Path, default=OUTDIR,
                   help="Where to write the per-family TSV reports.")
    args = p.parse_args()

    if not DECRYPTED.is_dir():
        print(f"checksum-discover: no ROM corpus at {DECRYPTED}",
              file=sys.stderr)
        return 1

    fams = load_families()
    if not fams:
        print(f"checksum-discover: no families found under {DECRYPTED}",
              file=sys.stderr)
        return 1

    chosen = (
        {s.strip().upper() for s in args.families.split(",") if s.strip()}
        if args.families else None
    )

    args.out_dir.mkdir(parents=True, exist_ok=True)

    summaries: list[dict] = []
    for family, roms in sorted(fams.items()):
        if chosen is not None and family not in chosen:
            continue
        if len(roms) < args.min_roms:
            continue
        outpath = args.out_dir / f"{family}_candidates.tsv"
        print(f"  scanning {family} ({len(roms)} ROMs) ...", flush=True)
        summary = family_report(family, roms, outpath)
        summaries.append(summary)

    print()
    print("=== Summary ===")
    print(f"{'family':<8} {'roms':>5} {'size_groups':>12} {'candidates':>11}")
    for s in summaries:
        print(f"{s['family']:<8} {s['total_roms']:>5} "
              f"{s['size_groups']:>12} {s['candidates']:>11}")
    print(f"\nPer-family reports written to {args.out_dir}/")
    print("Next step: manually review the HIGH-likelihood runs in each "
          ".tsv — those are the candidate checksum byte locations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
