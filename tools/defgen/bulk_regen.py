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


# Fields under [pack] where we prefer the existing manual value when the
# regenerated value is empty/default. Maps field name to the "empty"
# sentinel string defgen emits when the source XML lacks that tag.
#
# Why this exists: the 2026-05-20 audit (commits 0008695 + 8ea6d71) found
# that bulk_regen had silently clobbered rom_size_bytes = 1048576 manual
# patches back to 0 because the source XMLs lacked <filesize>. defgen now
# defaults rom_size_bytes to 1MB (8ea6d71), but the same regression class
# applies to transmission, years, etc. when XMLs omit those tags. This
# preservation pass is a second line of defense: even if defgen emits
# empty for a [pack] field, a non-empty existing value wins.
_PRESERVABLE_PACK_FIELDS: dict[str, str] = {
    "rom_size_bytes": "0",
    "transmission":   '""',
    "years":          "[]",
    # display_name is sometimes hand-edited; preserve when defgen would
    # blank it. defgen's actual default emits the XML's <xmlid>, so this
    # only triggers when an XML had no xmlid (effectively never).
    "display_name":   '""',
}


def _extract_pack_field(pack_text: str, key: str) -> tuple[str, str] | None:
    """Return (full_line, value_part) for `key` in the [pack] section.

    Bounded to the [pack] block so it doesn't pick up like-named keys in
    [[scaling]] / [[table]] blocks. Returns None if [pack] doesn't exist
    or doesn't contain `key`.
    """
    pack_match = re.search(r"^\[pack\]\s*$", pack_text, re.MULTILINE)
    if pack_match is None:
        return None
    body_start = pack_match.end()
    next_section = re.search(r"^\[", pack_text[body_start:], re.MULTILINE)
    body_end = body_start + (next_section.start() if next_section else
                              len(pack_text) - body_start)
    body = pack_text[body_start:body_end]
    line_match = re.search(
        rf"^(\s*{re.escape(key)}\s*=\s*([^\n#]+?))(\s*#.*)?$",
        body, re.MULTILINE)
    if line_match is None:
        return None
    full_line = line_match.group(1)
    value_part = line_match.group(2).strip()
    return (full_line, value_part)


def _replace_pack_field(pack_text: str, key: str, new_value_text: str) -> str:
    """Rewrite the `key = ...` line inside [pack] to use `new_value_text`.

    Preserves indentation and surrounding spacing. No-op if [pack] or
    the key isn't found.
    """
    pack_match = re.search(r"^\[pack\]\s*$", pack_text, re.MULTILINE)
    if pack_match is None:
        return pack_text
    body_start = pack_match.end()
    next_section = re.search(r"^\[", pack_text[body_start:], re.MULTILINE)
    body_end = body_start + (next_section.start() if next_section else
                              len(pack_text) - body_start)
    body = pack_text[body_start:body_end]
    new_body, n = re.subn(
        rf"^(\s*{re.escape(key)}\s*=\s*)[^\n#]+",
        lambda m: m.group(1) + new_value_text,
        body, count=1, flags=re.MULTILINE)
    if n == 0:
        return pack_text
    return pack_text[:body_start] + new_body + pack_text[body_end:]


def _apply_field_preservation(existing_text: str, new_text: str
                               ) -> tuple[str, list[tuple[str, str, str]]]:
    """Overlay existing-pack [pack] field values onto new_text where the
    new value is the defgen empty/default sentinel but the existing
    value is non-empty.

    Returns (rewritten_new_text, [(field, kept_value, dropped_value), ...]).
    The list is empty when no fields needed preservation.
    """
    overrides: list[tuple[str, str, str]] = []
    out_text = new_text
    for field, default_repr in _PRESERVABLE_PACK_FIELDS.items():
        existing = _extract_pack_field(existing_text, field)
        regenerated = _extract_pack_field(new_text, field)
        if existing is None or regenerated is None:
            continue
        existing_val = existing[1]
        new_val = regenerated[1]
        if new_val != default_repr:
            continue  # defgen produced a real value; trust it
        if existing_val == default_repr:
            continue  # nothing to preserve
        out_text = _replace_pack_field(out_text, field, existing_val)
        overrides.append((field, existing_val, new_val))
    return (out_text, overrides)


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

    # Preserve [pack] fields the existing pack manually patched (e.g.,
    # rom_size_bytes when the XML omitted <filesize>). See
    # _apply_field_preservation for the rule.
    new_text, preservations = _apply_field_preservation(existing_text, new_text)

    # No-op detection: skip writing if the regenerated text matches.
    if new_text == existing_text:
        return ("no-change", "already in sync")

    pack_path.write_text(new_text, encoding="utf-8")
    detail = f"wrote {len(new_text)} bytes from {xml_path.name}"
    if preservations:
        preserved_summary = ", ".join(
            f"{field}={kept}" for field, kept, _ in preservations)
        detail += f"; preserved {preserved_summary}"
    return ("regen", detail)


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
