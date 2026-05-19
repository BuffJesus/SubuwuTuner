"""
dump_xml_corpus.py
==================

Sweep all cached forum-attachment directories for .xml files that look
like ECU definitions (RomRaider or ECUFlash format). Copy them into a
clean corpus directory organized by ECU ID where derivable.

Source dirs (cached extracted archives):
  roms_extracted/forumdownloads/batch/ext_raw_*/
  roms_extracted/forumdownloads/validation/aid_*/extracted/
  roms_extracted/forumdownloads/investigate/ext_raw_*/

Output:
  roms_extracted/romraider-xml/
    <ECU_ID>/<aid>_<filename>.xml          per-ECU definitions
    _other/<aid>_<filename>.xml            unknown / multi-ECU defs
    INDEX.md                               summary
"""

from __future__ import annotations

import hashlib
import re
import shutil
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = [
    HERE / "roms_extracted" / "forumdownloads" / "batch",
    HERE / "roms_extracted" / "forumdownloads" / "validation",
    HERE / "roms_extracted" / "forumdownloads" / "investigate",
    HERE / "roms_extracted" / "forumdownloads" / "data",
    HERE / "roms_extracted" / "forumdownloads" / "documents",
]
OUT = HERE / "roms_extracted" / "romraider-xml"

ECU_ID_RE = re.compile(r"[A-Z]\w{6,9}[A-Z0-9_]", re.I)
DEF_ROOT_TAGS = ("rom", "roms", "romraider", "ecu", "ecutags", "ecuflash")


def classify_xml(p: Path) -> tuple[bool, list[str], str]:
    """Return (is_definition, ecu_ids_referenced, kind)."""
    try:
        head = p.read_text(encoding="utf-8", errors="ignore")[:8192]
    except Exception:
        return False, [], "unreadable"
    if "<" not in head:
        return False, [], "not_xml"
    low = head.lower()
    is_def = any(f"<{t}" in low for t in DEF_ROOT_TAGS)
    if not is_def:
        # Try larger sample
        try:
            full = p.read_text(encoding="utf-8", errors="ignore")
            is_def = any(f"<{t}" in full.lower() for t in DEF_ROOT_TAGS)
        except Exception:
            pass
    if not is_def:
        return False, [], "non_definition_xml"
    # RomRaider XML structure:
    #   <xmlid>DB4G301D</xmlid>   ← human-readable ECU ID (what we use)
    #   <ecuid>5B42564107</ecuid> ← internal cal-hash (NOT what we want)
    # ECUFlash often uses xmlid the same way.
    text = p.read_text(encoding="utf-8", errors="ignore")
    ids: set[str] = set()
    # Primary: <xmlid> tag value
    for m in re.finditer(r"<xmlid>\s*([A-Z0-9_]+)\s*</xmlid>", text, re.I):
        v = m.group(1).strip().upper().rstrip("_")
        if 6 <= len(v) <= 12 and any(c.isalpha() for c in v):
            ids.add(v)
    # Fallback: <calid> tag (rare but seen)
    if not ids:
        for m in re.finditer(r"<calid>\s*([A-Z0-9_]+)\s*</calid>", text, re.I):
            v = m.group(1).strip().upper().rstrip("_")
            if 6 <= len(v) <= 12 and any(c.isalpha() for c in v):
                ids.add(v)
    # Fallback: pattern-match in filename
    if not ids:
        for m in re.finditer(
            r'\b(EH3J|EH3N|EH3R|XH3J|XH3N|XH3M|XS1[A-Z]|ZA1J|DB4G|DB4I|EA1M|EA1U|'
            r'XE1[A-Z]|XHV[A-Z]|ZF2[A-Z]|LF9[A-Z]|LG7[A-Z]|LT8[A-Z]|LHB[A-Z]|'
            r'LV9N|AV9D|LF7[5-9]|LF6A|AF5[A-Z]|AF56|AE8M|EP5[A-Z]|EE5[A-Z]|'
            r'EZ1[A-Z]|DE5[A-Z]|DZ1E)[A-Z0-9_]{2,6}\b',
            p.name, re.I,
        ):
            v = m.group(0).upper().rstrip("_")
            if 6 <= len(v) <= 12: ids.add(v)
    low = text.lower()
    kind = ("romraider" if "<romid>" in low or "<romraider" in low
            else "ecuflash" if "<ecuflash" in low
            else "definition")
    return True, sorted(ids), kind


def extract_aid(path: Path) -> str | None:
    """Get attachment id from container directory like ext_raw_42758."""
    for part in path.parts:
        m = re.match(r"(?:ext_raw_|aid_|raw_)(\d+)", part)
        if m: return m.group(1)
    return None


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    seen_hashes: set[str] = set()
    by_ecu: dict[str, list[Path]] = defaultdict(list)
    non_def: list[Path] = []
    total_xml = 0

    for src in SOURCES:
        if not src.exists(): continue
        for xml in src.rglob("*.xml"):
            total_xml += 1
            data = xml.read_bytes()
            h = hashlib.sha256(data).hexdigest()
            if h in seen_hashes: continue
            seen_hashes.add(h)

            is_def, ids, kind = classify_xml(xml)
            if not is_def:
                non_def.append(xml); continue
            aid = extract_aid(xml) or "x"
            safe_name = re.sub(r"[^\w.-]", "_", xml.name)[:80]
            if ids:
                for eid in ids:
                    target_dir = OUT / eid
                    target_dir.mkdir(parents=True, exist_ok=True)
                    dst = target_dir / f"aid{aid}_{safe_name}"
                    if not dst.exists():
                        shutil.copy(xml, dst)
                    by_ecu[eid].append(dst)
            else:
                other = OUT / "_other"
                other.mkdir(parents=True, exist_ok=True)
                dst = other / f"aid{aid}_{safe_name}"
                if not dst.exists():
                    shutil.copy(xml, dst)
                by_ecu["_other"].append(dst)

    # Index
    lines: list[str] = ["# RomRaider / ECUFlash XML corpus\n"]
    lines.append(f"- Source XMLs scanned: {total_xml}")
    lines.append(f"- Unique XMLs (by sha256): {len(seen_hashes)}")
    lines.append(f"- Cataloged as definitions: {sum(len(v) for k, v in by_ecu.items() if k != '_other')}")
    lines.append(f"- Multi-ECU / no-ID definitions: {len(by_ecu.get('_other', []))}")
    lines.append(f"- Non-definition XMLs skipped: {len(non_def)}\n")

    # Per-ECU table
    lines.append("## Per-ECU coverage\n")
    lines.append("| ECU ID | # XML files | Have decrypted ROM? |")
    lines.append("|---|---|---|")
    PLAIN = HERE / "plaintext_corpus" / "keystream-recovered"
    have_rom: set[str] = set()
    if PLAIN.exists():
        for p in PLAIN.rglob("*.bin"):
            have_rom.add(p.stem.upper())
    other = by_ecu.pop("_other", [])
    for eid in sorted(by_ecu):
        rom_mark = "✓" if eid in have_rom else "—"
        lines.append(f"| `{eid}` | {len(by_ecu[eid])} | {rom_mark} |")
    if other:
        lines.append(f"| `_other` | {len(other)} | — |")
        by_ecu["_other"] = other

    (OUT / "INDEX.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"→ {OUT}")
    print(f"  ECU IDs cataloged: {sum(1 for k in by_ecu if k != '_other')}")
    print(f"  Have both XML and decrypted ROM: {sum(1 for k in by_ecu if k != '_other' and k in have_rom)}")
    print(f"  XML-only (ROM still locked or absent): {sum(1 for k in by_ecu if k != '_other' and k not in have_rom)}")
    print(f"  Other / multi-ECU XMLs: {len(by_ecu.get('_other', []))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
