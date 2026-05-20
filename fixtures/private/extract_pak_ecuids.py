"""
extract_pak_ecuids.py — decode every pak's header.csv and build a
{CID: ecuid} map.

For each .pak in encrypted_paks/:
  1. unpak.py extracts header.csv + body file
  2. rc2decode.exe decrypts header.csv (CsvKey)
  3. Parse the decoded CSV:
       FileName column -> CID (strip .bid/.sob)
       NewRomid column -> 10-hex SSM ecuid

Output:
  fixtures/private/pak_cid_to_ecuid.tsv   (CID<TAB>ecuid<TAB>pak_filename)

This map is then consumed by patch_ecu_part.py to populate ecu_part in
the 25 new FA-DIT VA/VB WRX packs in definitions/impreza/.
"""
from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAKS = ROOT / "fixtures" / "private" / "roms_extracted" / "encrypted_paks"
TOOLS = ROOT / "build" / "scratch" / "pak-tools"
UNPAK = TOOLS / "unpak.py"
RC2 = TOOLS / "rc2decode.exe"
OUT = ROOT / "fixtures" / "private" / "pak_cid_to_ecuid.tsv"


def decode_pak_header(pak: Path, work: Path) -> dict | None:
    work.mkdir(parents=True, exist_ok=True)
    # 1. unpak
    r = subprocess.run(
        ["py", str(UNPAK), "-i", str(pak), "-u", "-d", str(work)],
        capture_output=True, text=True)
    if r.returncode != 0:
        return None
    hdr = work / "header.csv"
    if not hdr.exists():
        return None
    # 2. rc2decode header.csv
    dec = work / "header_decoded.csv"
    r = subprocess.run(
        [str(RC2), str(hdr), "CsvKey", str(dec)],
        capture_output=True, text=True)
    if r.returncode != 0 or not dec.exists():
        return None
    return parse_decoded_csv(dec, pak.name)


def parse_decoded_csv(path: Path, pak_name: str) -> dict | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    # CSV has a header row "[FileData]" with column names, then data rows.
    # FileName col maps to "<CID>.bid" / "<CID>.sob". NewRomid is the ecuid
    # for the file getting flashed in.
    rdr = csv.reader(text.splitlines())
    rows = list(rdr)
    if not rows:
        return None
    # Find column indexes
    header = rows[0]
    try:
        fn_col = header.index("FileName")
        nr_col = header.index("NewRomid")
        np_col = header.index("NewPartsNo")
    except ValueError:
        return None
    out = []
    for row in rows[1:]:
        if len(row) <= max(fn_col, nr_col, np_col):
            continue
        fn = row[fn_col].strip()
        ecuid = row[nr_col].strip()
        partno = row[np_col].strip()
        if not fn or not ecuid:
            continue
        # FileName like "LF9L000E.bid" or "LF9L000E.sob"
        cid = fn.rsplit(".", 1)[0]
        if len(ecuid) == 10 and all(c in "0123456789ABCDEFabcdef" for c in ecuid):
            out.append((cid, ecuid.upper(), partno, pak_name))
    return out


def main():
    if not PAKS.is_dir():
        print(f"PAKS dir not found: {PAKS}", file=sys.stderr)
        return 1
    paks = sorted(PAKS.glob("*.pak"))
    print(f"Scanning {len(paks)} paks...\n")

    all_rows = []
    fail = 0
    for i, pak in enumerate(paks, 1):
        with tempfile.TemporaryDirectory() as tmp:
            rows = decode_pak_header(pak, Path(tmp))
        if rows is None:
            fail += 1
            continue
        all_rows.extend(rows)
        if i % 10 == 0:
            print(f"  {i}/{len(paks)} paks scanned")

    # De-dup by (CID, ecuid)
    seen = set()
    final = []
    for cid, eid, partno, pak in all_rows:
        key = (cid.upper(), eid.upper())
        if key in seen:
            continue
        seen.add(key)
        final.append((cid, eid, partno, pak))
    final.sort()

    with OUT.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t")
        w.writerow(["CID", "ecuid", "Part_Number", "pak_filename"])
        for row in final:
            w.writerow(row)

    print()
    print(f"Wrote {len(final)} unique CID/ecuid rows to {OUT.name}")
    print(f"Failed paks: {fail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
