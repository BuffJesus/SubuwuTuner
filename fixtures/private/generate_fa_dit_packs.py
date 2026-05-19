"""
generate_fa_dit_packs.py — bulk-generate FA-DIT WRX packs from the
forum-sourced per-CID XMLs at fixtures/private/roms_extracted/
romraider-xml-per-cid/.

For each LF/LV/LHB XML there:
  1. defgen with the XML as input
  2. Patch [pack].platform to "subaru.impreza" (per Path B convention
     — all WRX variants live in definitions/impreza/)
  3. Patch [pack].years from the romid submodel (e.g. "VA 2021" -> 2021)
  4. Patch [pack].rom_size_bytes based on family
       LF75/LF78/LF79/LF9B/LF9C/LV9N (2 MB) -> 2_097_152
       LF9D/LF9G/LF9L                 (2.5 MB) -> 2_621_440
       LHB*                             (4 MB)  -> 4_194_304
  5. Add `cid_scan = true` to the [[identification]] block (FA-DIT
     CIDs live at variable per-firmware offsets — already documented
     in docs/11)
  6. Add `includes = ["../pids.toml"]` so the pack gets shared SSM
     datalogger PIDs out of the box
  7. Write to definitions/impreza/<cid>.toml

Per the user's 2026-05-19 correction on provenance (memory entry
project_intree_va_vb_xml_provenance.md): the in-tree romraider_*_wrx.xml
files (Layer-1 source for the per-CID XMLs via make_per_cid_xmls.py)
are forum-sourced, NOT Atlas-derived. The resulting per-CID XMLs are
§1201-clean fact sources eligible for public-repo packs per docs/17 §4
acceptance criteria. They're produced by defgen from clean XML;
they don't contain ROM bytes.
"""

from __future__ import annotations

import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PER_CID = ROOT / "fixtures" / "private" / "roms_extracted" / "romraider-xml-per-cid"
DEFGEN = ROOT / "tools" / "defgen" / "defgen.py"
OUT_DIR = ROOT / "definitions" / "impreza"

# ROM size by family prefix (first 4 chars of CID, uppercased).
ROM_SIZE = {
    # FA-DIT VA WRX (2015-2018) — 2 MB
    "LF75": 2_097_152, "LF78": 2_097_152, "LF79": 2_097_152,
    "LF9B": 2_097_152, "LF9C": 2_097_152, "LV9N": 2_097_152,
    # FA-DIT VA WRX 2019+ — 2.5 MB
    "LF9D": 2_621_440, "LF9G": 2_621_440, "LF9L": 2_621_440,
    # FA-DIT VB WRX 2022+ — 4 MB
    "LHBH": 4_194_304, "LHBK": 4_194_304, "LHBP": 4_194_304, "LHBT": 4_194_304,
}


def submodel_year(submodel: str) -> int | None:
    """Parse 'VA 2021' / 'VB 2022' etc. -> 2021 / 2022."""
    m = re.search(r"\b(\d{4})\b", submodel or "")
    return int(m.group(1)) if m else None


def patch_pack(text: str, cid: str, xml_path: Path) -> str:
    """Patch the defgen output: platform, years, rom_size_bytes,
    cid_scan, includes."""
    # Read xml for romid metadata
    tree = ET.parse(xml_path)
    submodel = ""
    for rom in tree.getroot().findall("rom"):
        rid = rom.find("romid/xmlid")
        if rid is None or rid.text != cid:
            continue
        sub_el = rom.find("romid/submodel")
        submodel = (sub_el.text or "").strip() if sub_el is not None else ""
        break
    year = submodel_year(submodel)

    fam = cid[:4].upper()
    rom_size = ROM_SIZE.get(fam, 0)

    # 1. platform = subaru.impreza
    text = re.sub(
        r'^(platform\s*=\s*)"subaru"',
        r'\1"subaru.impreza"',
        text, count=1, flags=re.MULTILINE)

    # 2. years = [yr] if year known
    if year:
        text = re.sub(
            r'^(years\s*=\s*)\[\]',
            rf'\1[{year}]',
            text, count=1, flags=re.MULTILINE)

    # 3. rom_size_bytes
    if rom_size > 0:
        text = re.sub(
            r'^(rom_size_bytes\s*=\s*)0$',
            rf'\g<1>{rom_size}',
            text, count=1, flags=re.MULTILINE)

    # 4. Insert `includes = ["../pids.toml"]` after license = "..."
    text = re.sub(
        r'^(license\s*=\s*"[^"]*")$',
        r'\1\nincludes       = ["../pids.toml"]',
        text, count=1, flags=re.MULTILINE)

    # 5. Add `cid_scan = true` inside [[identification]] block,
    #    right after the cid_match line.
    text = re.sub(
        r'^(cid_match\s*=\s*"[^"]*")$',
        r'\1\ncid_scan    = true',
        text, count=1, flags=re.MULTILINE)

    return text


def main():
    if not PER_CID.is_dir():
        print(f"per-CID XML dir not found: {PER_CID}", file=sys.stderr)
        return 1

    families = ("LF", "LV", "LH")
    xml_files = sorted(p for p in PER_CID.glob("*.xml")
                       if p.stem.upper().startswith(families))

    print(f"Found {len(xml_files)} candidate XML files\n")
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    n_new = 0
    n_skip = 0
    n_fail = 0
    for xml in xml_files:
        cid = xml.stem.upper()
        # Strip any non-alphanumeric (some have suffix like _x0G — keep
        # original casing per XML)
        cid_norm = re.sub(r"[^A-Za-z0-9]", "", xml.stem)
        out_name = cid_norm.lower() + ".toml"
        out_path = OUT_DIR / out_name
        if out_path.exists():
            print(f"  SKIP {cid} (exists at {out_path.name})")
            n_skip += 1
            continue

        # Run defgen
        r = subprocess.run(
            ["py", str(DEFGEN), str(xml), "--rom-id", cid_norm],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  FAIL {cid}: defgen rc={r.returncode}: {r.stderr.strip()[:150]}", file=sys.stderr)
            n_fail += 1
            continue
        text = r.stdout
        if not text or "[pack]" not in text:
            # Try without --rom-id (XML may have a single rom)
            r = subprocess.run(
                ["py", str(DEFGEN), str(xml)],
                capture_output=True, text=True)
            text = r.stdout

        if not text or "[pack]" not in text:
            print(f"  FAIL {cid}: no [pack] block in output", file=sys.stderr)
            n_fail += 1
            continue

        # Patch + write
        patched = patch_pack(text, cid_norm, xml)
        out_path.write_text(patched, encoding="utf-8")
        n_new += 1
        # Quick stats: how many tables?
        n_tables = patched.count("[[table]]")
        n_axes = patched.count("[[axis]]")
        print(f"  OK   {cid_norm} -> {out_path.name} ({n_tables} tables, {n_axes} axes)")

    print()
    print(f"Generated: {n_new}  Skipped: {n_skip}  Failed: {n_fail}")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
