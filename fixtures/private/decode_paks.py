"""
decode_paks.py — batch-decode Subaru FlashWrite .pak/.pk2 files to raw .bin.

Three-stage pipeline (all stages need the C++ tools from aalesv/pak-tools
plus the unpak.py from the same repo):

  Stage 1: unpak.py        — split .pak into header.csv + body.bid
  Stage 2: rc2decode.exe   — RC2 decrypt body.bid with per-pak keyword
                             (looked up from subaru_pak_metadata.tsv)
  Stage 3: crypt.exe       — swap-bit decode with per-ECU 4-word vendor key
                             (auto-tried: Denso CAN -> Hitachi -> Denso K-line)

Vendor-key auto-detection: tries Denso CAN first (most VA WRX 2015-2018),
then Hitachi (VA WRX 2019+ and VB WRX), then Denso K-line (older). Accepts
the first attempt whose output contains the expected CID string verbatim.

Outputs: roms_extracted/decoded_paks/<CID>.bin

Usage:
    python decode_paks.py                       # process all staged paks
    python decode_paks.py --only LF9L000E       # process just one
"""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAK_DIR = HERE / "roms_extracted" / "encrypted_paks"
OUT_DIR = HERE / "roms_extracted" / "decoded_paks"
META_TSV = HERE / "subaru_pak_metadata.tsv"
REPO_ROOT = HERE.parent.parent
TOOLS_DIR = REPO_ROOT / "build" / "scratch" / "pak-tools"

UNPAK_PY = TOOLS_DIR / "unpak.py"
RC2_EXE = TOOLS_DIR / "rc2decode.exe"
CRYPT_EXE = TOOLS_DIR / "crypt.exe"
SREC2BIN_EXE = TOOLS_DIR / "srec2bin.exe"

# Vendor keys in try-order. Most common first.
VENDOR_KEYS = [
    # (label, key_words)
    ("Denso_CAN",
     ("0x92A0", "0xE282", "0x32C0", "0xC85B")),
    ("Hitachi",
     ("0x5FB1", "0xA7CA", "0x42DA", "0xB740")),
    ("Denso_Kline",
     ("0x6E86", "0xF513", "0xCE22", "0x7856")),
]


def load_metadata() -> dict[str, tuple[str, str, str]]:
    """Map pak_number -> (cid, keyword, vehicle)."""
    out: dict[str, tuple[str, str, str]] = {}
    with META_TSV.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for r in reader:
            pak = r.get("Pack_Number", "").strip()
            cid = r.get("CID", "").strip()
            kw = r.get("Keyword", "").strip()
            veh = r.get("Vehicle", "").strip()
            if pak and cid and kw:
                # Same pak can appear in multiple rows (different
                # AppricableCID source CIDs all updating to the same
                # new CID). First entry wins.
                out.setdefault(pak, (cid, kw, veh))
    return out


def decode_one(pak_path: Path, cid: str, keyword: str,
               work_dir: Path) -> Path | None:
    """Run unpak -> rc2decode -> crypt for one pak. Returns output bin
    path on success, None on failure.

    Tries each vendor key in turn; accepts the first that produces a
    plaintext containing the CID byte string.
    """
    work_dir.mkdir(parents=True, exist_ok=True)

    # Stage 1: unpak
    r = subprocess.run(
        ["py", str(UNPAK_PY), "-i", str(pak_path), "-u", "-d", str(work_dir)],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  unpak FAILED: {r.stderr.strip()[:200]}", file=sys.stderr)
        return None, None

    # Stage 1 should have produced .bid (newer Hitachi paks, BIN data
    # format) OR .sob (older Denso paks, SREC data format). The .sob
    # paks typically have TWO files — a bootloader (egisblc...) and
    # the real ECU code (named after the part number). We want the
    # bigger of the two for ECU paks.
    bid_files = list(work_dir.glob("*.bid"))
    sob_files = [s for s in work_dir.glob("*.sob")
                 if not s.name.startswith("egisblc")]
    if bid_files:
        payload_path = bid_files[0]
        is_srec = False
    elif sob_files:
        payload_path = max(sob_files, key=lambda p: p.stat().st_size)
        is_srec = True
    else:
        print(f"  no .bid/.sob produced", file=sys.stderr)
        return None, None

    # Stage 2: RC2 decrypt
    post_rc2 = work_dir / "post_rc2.dat"
    r = subprocess.run(
        [str(RC2_EXE), str(payload_path), keyword, str(post_rc2)],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  rc2decode FAILED: {r.stderr.strip()[:200]}", file=sys.stderr)
        return None, None

    # Stage 2b (S-Record paks only): srec -> bin
    if is_srec:
        srec_bin = work_dir / "srec_to_bin.bin"
        # srec2bin needs -M <megabytes>. Pak source files are typically
        # 1 MB or 2 MB ROMs; try 2 by default (handles both).
        r = subprocess.run(
            [str(SREC2BIN_EXE), str(srec_bin), "-M", "2", "-s", str(post_rc2)],
            capture_output=True, text=True)
        # srec2bin always exits 1 but produces the output anyway —
        # don't fail on returncode; just check the file exists.
        if not srec_bin.exists() or srec_bin.stat().st_size == 0:
            print(f"  srec2bin FAILED", file=sys.stderr)
            return None, None
        post_rc2 = srec_bin  # feed this to crypt stage

    # Stage 3: try each vendor key. Accept the first one where the
    # output contains the CID byte string in any of three case forms:
    # all-upper, all-lower, or mixed (last char swapped). Some Subaru
    # CIDs are stored mixed-case (e.g. LF9D040h).
    cid_variants = {
        cid.upper().encode("ascii"),
        cid.lower().encode("ascii"),
        (cid[:-1].upper() + cid[-1].lower()).encode("ascii"),
        (cid[:-1].lower() + cid[-1].upper()).encode("ascii"),
    }

    for label, key_words in VENDOR_KEYS:
        candidate = work_dir / f"candidate_{label}.bin"
        r = subprocess.run(
            [str(CRYPT_EXE), str(post_rc2), *key_words, str(candidate)],
            capture_output=True, text=True)
        if r.returncode != 0:
            continue
        data = candidate.read_bytes()
        if any(v in data for v in cid_variants):
            return (candidate, label)
    return (None, None)


def main():
    ap = argparse.ArgumentParser(prog="decode_paks")
    ap.add_argument("--only", action="append", default=None,
                    help="Process only the named CID(s).")
    ap.add_argument("--keep-work-dir", action="store_true",
                    help="Don't delete intermediate work dirs.")
    args = ap.parse_args()

    if not all(p.exists() for p in (UNPAK_PY, RC2_EXE, CRYPT_EXE)):
        print(f"Missing tools. Expected at {TOOLS_DIR}", file=sys.stderr)
        return 1

    meta = load_metadata()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    paks = sorted(PAK_DIR.glob("22*.pak")) + sorted(PAK_DIR.glob("22*.pk2"))
    if args.only:
        wanted = {c.upper() for c in args.only}
    else:
        wanted = None

    results = {"OK": 0, "FAIL": 0, "SKIP": 0}
    by_label: dict[str, int] = {}

    for pak in paks:
        pak_num = pak.stem
        if pak_num not in meta:
            results["SKIP"] += 1
            continue
        cid, keyword, vehicle = meta[pak_num]
        if wanted and cid.upper() not in wanted:
            continue
        out_path = OUT_DIR / f"{cid}.bin"
        if out_path.exists():
            print(f"  {cid}: already decoded ({out_path.name})")
            results["OK"] += 1
            continue

        with tempfile.TemporaryDirectory() as td:
            work = Path(td) if not args.keep_work_dir else (PAK_DIR.parent / "work" / pak_num)
            decoded, label = decode_one(pak, cid, keyword, work)
            if decoded is not None:
                shutil.move(str(decoded), str(out_path))
                results["OK"] += 1
                by_label[label] = by_label.get(label, 0) + 1
                print(f"  {cid}: OK via {pak.name} ({label})")
            else:
                results["FAIL"] += 1
                print(f"  {cid}: FAILED via {pak.name}", file=sys.stderr)

    print()
    print(f"Summary: OK={results['OK']}  FAIL={results['FAIL']}  SKIP={results['SKIP']}")
    print(f"By vendor key: {by_label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
