#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
bulk_regen — one-shot helper to regenerate every existing pack TOML from
its original XML source, preserving the pack's `includes` line.

Driving need: when a defgen bug fix changes how generated TOMLs look
(e.g. the 2026-05-19 float-axis storagetype fix), the in-tree packs need
to be regenerated to pick up the corrected output. Hand-running defgen
per pack would drop each pack's `includes` line (defgen never emits it),
and we don't want a 333-file commit that silently strips includes.

This script:
  1. Walks `definitions/<model>/*.toml`, skipping `ecuparams/` and root.
  2. For each pack, reads its pack.id (the lowercased CID).
  3. Looks up the source XML for that CID:
       - Master ecu_defs.xml at fixtures/private/roms_extracted/
         romraider-xml/A2TB000L/aid40221_ecu_defs_copy.xml for CIDs
         present there (the bulk of EJ-era coverage).
       - Single-CID XML at fixtures/private/roms_extracted/romraider-xml/
         <CID>/aidx_*.xml for CIDs not in master (per-XML overrides).
  4. Captures the existing pack's `includes` line.
  5. Runs `defgen.parse_rom_xml` + emits a fresh TOML for that CID.
  6. Re-inserts the `includes` line into the regenerated text at the
     same position the original had it (right after `license =`).
  7. Writes the result back to the pack's path.

Idempotent: re-running produces no further changes once everything is
in sync with the current defgen behaviour.

Usage:
    python tools/defgen/bulk_regen.py [--dry-run] [--only <cid> ...]

--dry-run prints which packs would be touched without modifying files.
--only restricts to specific CIDs (case-insensitive); useful for
spot-checking the regen on a single pack before committing the bulk.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "defgen"))

import defgen  # noqa: E402

# Canonical sources. Order matters: try master first; CIDs found there
# use master. CIDs not in master fall through to per-CID single-XML files.
MASTER_XML = (REPO_ROOT / "fixtures" / "private" / "roms_extracted" /
              "romraider-xml" / "A2TB000L" / "aid40221_ecu_defs_copy.xml")

# CIDs whose XML lives outside the master (added in earlier commits).
# Each maps to the absolute path of the XML defgen should read.
PER_CID_OVERRIDES: dict[str, Path] = {
    "EZ1GC00C": REPO_ROOT / "fixtures" / "private" / "roms_extracted" /
                "romraider-xml" / "EZ1GC00C" / "aidx_aid43797_EZ1GC00C.xml",
    "EP5G600A": REPO_ROOT / "fixtures" / "private" / "roms_extracted" /
                "romraider-xml" / "EP5G600A" / "aidx_aid43764_EP5G600A.xml",
    "LV9N001D": REPO_ROOT / "fixtures" / "private" / "roms_extracted" /
                "romraider-xml" / "LV9N001D" / "aid24036_RR_LV9N001D.xml",
}

# Packs under definitions/ that we skip — they're not defgen-derived.
SKIP_DIRS = {"ecuparams"}

# Pre-load master XML CID set to avoid re-parsing for every pack.
def _master_cids() -> set[str]:
    text = MASTER_XML.read_text(encoding="utf-8", errors="replace")
    return set(m.upper().strip()
               for m in re.findall(r"<xmlid>([^<]+)</xmlid>", text))


def _extract_includes_line(pack_text: str) -> str | None:
    """Return the `includes = [...]` line verbatim, or None if absent."""
    m = re.search(r"^(includes\s*=\s*\[.*?\])\s*$", pack_text, re.MULTILINE)
    return m.group(1) if m else None


def _insert_includes_line(pack_text: str, includes_line: str) -> str:
    """Insert `includes_line` immediately after the `license =` line.

    Match the license line strictly — pattern includes the closing `"`
    then `$` in MULTILINE mode, NOT `\\s*$`. The looser form was greedy
    across newlines (`\\s` matches `\\n`), eating the blank lines between
    license and [[identification]] into the capture group and stranding
    the insertion immediately above [[identification]] instead of below
    license. Strict line-end anchoring avoids that.
    """
    pattern = r'(^license\s*=\s*"[^"]*")$'
    if not re.search(pattern, pack_text, re.MULTILINE):
        # Should never happen for defgen-emitted packs, but degrade
        # gracefully: prepend before the first [[identification]] block.
        ident = re.search(r"^\[\[identification\]\]", pack_text, re.MULTILINE)
        if ident is None:
            return pack_text  # can't place it sensibly
        return (pack_text[:ident.start()] +
                includes_line + "\n\n\n" +
                pack_text[ident.start():])
    return re.sub(pattern, r"\1\n" + includes_line, pack_text,
                  count=1, flags=re.MULTILINE)


def _regen_pack(pack_path: Path, master_cids: set[str],
                only: set[str] | None) -> tuple[str, str]:
    """Regenerate one pack. Returns (status, detail).

    status is one of "regen", "skip", "error", "no-change". Detail is a
    short human-readable explanation for logs.
    """
    cid = pack_path.stem.upper()
    if only is not None and cid not in only:
        return ("skip", "filtered out by --only")

    # Pick the source XML.
    if cid in PER_CID_OVERRIDES:
        xml_path = PER_CID_OVERRIDES[cid]
    elif cid in master_cids:
        xml_path = MASTER_XML
    else:
        return ("skip", f"CID {cid} not in master XML and no per-CID "
                        f"override — manual regen required")

    if not xml_path.is_file():
        return ("error", f"source XML missing: {xml_path}")

    # Capture the existing includes line.
    existing_text = pack_path.read_text(encoding="utf-8")
    includes_line = _extract_includes_line(existing_text)

    # Re-emit via defgen.
    try:
        xml_text = xml_path.read_text(encoding="utf-8", errors="replace")
        packs = defgen.parse_rom_xml(xml_text, rom_id_filter=cid)
    except Exception as e:
        return ("error", f"defgen.parse_rom_xml failed: {e}")
    if not packs:
        return ("error", f"no pack for {cid} in {xml_path.name}")
    if len(packs) > 1:
        return ("error", f"unexpected — XML returned {len(packs)} packs "
                         f"for romid {cid}")
    new_text = defgen.pack_to_toml(packs[0])

    # Re-insert the includes line if the original had one.
    if includes_line:
        new_text = _insert_includes_line(new_text, includes_line)

    # No-op detection: skip writing if the regenerated text matches.
    if new_text == existing_text:
        return ("no-change", "already in sync")

    pack_path.write_text(new_text, encoding="utf-8")
    return ("regen", f"wrote {len(new_text)} bytes from {xml_path.name}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="bulk_regen")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would change without writing.")
    ap.add_argument("--only", nargs="*", default=None,
                    help="Restrict to specific CIDs (case-insensitive).")
    args = ap.parse_args(argv)

    only = {c.upper() for c in args.only} if args.only is not None else None
    master_cids = _master_cids()

    counts = {"regen": 0, "skip": 0, "error": 0, "no-change": 0}
    errors: list[tuple[Path, str]] = []

    definitions_dir = REPO_ROOT / "definitions"
    for pack in sorted(definitions_dir.rglob("*.toml")):
        rel = pack.relative_to(REPO_ROOT)
        # Skip ecuparams and any non-pack root-level files.
        if any(part in SKIP_DIRS for part in rel.parts) or pack.parent == definitions_dir:
            continue

        if args.dry_run:
            cid = pack.stem.upper()
            target = (PER_CID_OVERRIDES[cid] if cid in PER_CID_OVERRIDES
                      else MASTER_XML if cid in master_cids
                      else None)
            if target is None:
                if only and cid not in only:
                    continue
                print(f"  [skip] {rel}: no source for {cid}")
                counts["skip"] += 1
                continue
            if only and cid not in only:
                continue
            print(f"  [dry]  {rel} <- {target.name}")
            counts["regen"] += 1
            continue

        status, detail = _regen_pack(pack, master_cids, only)
        counts[status] += 1
        if status == "error":
            errors.append((rel, detail))
            print(f"  [ERR]  {rel}: {detail}", file=sys.stderr)
        elif status == "regen":
            print(f"  [ok]   {rel}: {detail}")

    print()
    print(f"summary: regen={counts['regen']} no-change={counts['no-change']} "
          f"skip={counts['skip']} error={counts['error']}")
    if errors:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
