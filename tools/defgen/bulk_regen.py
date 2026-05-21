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

# Canonical sources. Lookup order (highest priority first):
#   1. PER_CID_OVERRIDES — hand-curated single-CID XMLs.
#   2. MASTER_BUNDLES    — multi-CID XML bundles (Merp's canonical
#                          ecu_defs.xml for EJ-era, plus forum-sourced
#                          VA/VB WRX bundles).
# CIDs that appear in multiple sources resolve to the highest-priority one.
MASTER_XML = (REPO_ROOT / "fixtures" / "private" / "roms_extracted" /
              "romraider-xml" / "A2TB000L" / "aid40221_ecu_defs_copy.xml")

# Bundles that may declare many CIDs each. Each entry is (xml_path, default_subdir).
# The default_subdir is the relative dir under definitions/ where any newly-
# created pack for a CID found only in this bundle will land.
MASTER_BUNDLES: list[tuple[Path, str]] = [
    (MASTER_XML, ""),  # default_subdir="" → use defgen's emitted platform
    (REPO_ROOT / "fixtures" / "private" / "romraider_defs" /
     "va" / "va_wrx_bundle.xml", "impreza"),
    (REPO_ROOT / "fixtures" / "private" / "romraider_defs" /
     "vb" / "vb_wrx_bundle.xml", "impreza"),
]

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

# When a bundle is the SOLE source for a CID and we're creating a new pack,
# coerce the platform field to this fixed value (regardless of what the
# bundle XML's <model> says). Avoids the WRX-line getting split between
# `subaru.wrx` (new bundle CIDs) and `subaru.impreza` (existing impreza/
# packs). The default subdir per bundle is impreza/ for now; future cleanup
# could migrate both to a new wrx/ subdir + subaru.wrx platform together.
NEW_PACK_PLATFORM_OVERRIDE = "subaru.impreza"

# Default `includes` line for newly-created packs in each subdir.
NEW_PACK_INCLUDES: dict[str, str] = {
    "impreza": 'includes       = ["../pids.toml"]',
}


def _bundle_cids(xml_path: Path) -> list[str]:
    """Return <xmlid> values declared in an XML bundle, original case preserved.

    Subaru's VB-era CIDs mix upper and lower case in deliberate positions
    (e.g. `LHBHE00Bx0G`), and defgen's --rom-id filter is case-sensitive,
    so we must hand defgen the exact case used in the XML.
    """
    text = xml_path.read_text(encoding="utf-8", errors="replace")
    return [m.strip() for m in re.findall(r"<xmlid>([^<]+)</xmlid>", text)]


def _cid_source_map() -> dict[str, tuple[Path, str, str]]:
    """Build {UPPER_CID: (xml_path, default_subdir, original_case_cid)} across
    all configured sources.

    Keying by uppercase lets us look up CIDs from lowercase pack filenames
    while preserving the original mixed-case spelling for defgen's filter.
    Lookup priority: per-CID overrides win, then bundles in declaration order.
    """
    out: dict[str, tuple[Path, str, str]] = {}
    for bundle_path, subdir in MASTER_BUNDLES:
        if not bundle_path.is_file():
            continue
        for cid in _bundle_cids(bundle_path):
            key = cid.upper()
            out.setdefault(key, (bundle_path, subdir, cid))
    for cid, override_path in PER_CID_OVERRIDES.items():
        out[cid.upper()] = (override_path, "", cid)
    return out


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
    # platform: defgen emits `subaru.<model>` based on the XML's <model>
    # tag, but the VA/VB WRX bundles say <model>WRX</model> which produces
    # `subaru` (the lowercased tail is stripped to one segment). Existing
    # packs in definitions/impreza/ use `subaru.impreza` as a stable
    # platform key; preserve that when defgen falls back to the bare
    # `subaru` sentinel.
    "platform":       '"subaru"',
}


# [[identification]] block fields where existing hand-edits should win.
# defgen builds this block from the XML's <romid> children plus
# <internalidaddress>, so manually-tuned values (explicit cid_address
# inferred from sibling-family pattern, the cid_scan safety net flag,
# the ecu_part string) get clobbered without a preservation pass.
# Maps field name to the "empty/default" sentinel string defgen emits.
# A value of None on the new side (line absent altogether) is treated
# the same as the sentinel for fields like cid_scan that defgen never
# emits at all.
_PRESERVABLE_IDENT_FIELDS: dict[str, str | None] = {
    "cid_address": "0x00000000",
    "ecu_part":    '""',
    "cid_scan":    None,  # defgen never emits; preserve if existing has it
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


def _extract_ident_field(pack_text: str, key: str) -> tuple[str, str] | None:
    """Return (full_line, value_part) for `key` in the first [[identification]]
    block. Bounded to that block so it doesn't match like-named keys in
    other sections. Returns None when the block or field is absent."""
    ident_match = re.search(r"^\[\[identification\]\]\s*$", pack_text, re.MULTILINE)
    if ident_match is None:
        return None
    body_start = ident_match.end()
    next_section = re.search(r"^\[", pack_text[body_start:], re.MULTILINE)
    body_end = body_start + (next_section.start() if next_section else
                              len(pack_text) - body_start)
    body = pack_text[body_start:body_end]
    line_match = re.search(
        rf"^(\s*{re.escape(key)}\s*=\s*([^\n#]+?))(\s*#.*)?$",
        body, re.MULTILINE)
    if line_match is None:
        return None
    return (line_match.group(1), line_match.group(2).strip())


def _replace_ident_field(pack_text: str, key: str, new_value_text: str) -> str:
    """Rewrite `key = ...` inside the first [[identification]] block."""
    ident_match = re.search(r"^\[\[identification\]\]\s*$", pack_text, re.MULTILINE)
    if ident_match is None:
        return pack_text
    body_start = ident_match.end()
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


def _insert_ident_field(pack_text: str, key: str, value_text: str) -> str:
    """Insert `key = value_text` into the first [[identification]] block
    when the key wasn't already present (e.g. defgen never emits cid_scan).
    Inserts immediately after the cid_match line for visibility; falls back
    to before the next section boundary."""
    ident_match = re.search(r"^\[\[identification\]\]\s*$", pack_text, re.MULTILINE)
    if ident_match is None:
        return pack_text
    # Try to insert after cid_match.
    after_cid_match = re.search(
        r"(^cid_match\s*=\s*[^\n]+\n)",
        pack_text[ident_match.end():], re.MULTILINE)
    if after_cid_match is not None:
        insert_pos = ident_match.end() + after_cid_match.end()
        line = f"{key:<11} = {value_text}\n"
        return pack_text[:insert_pos] + line + pack_text[insert_pos:]
    return pack_text  # degraded: structure unrecognized, leave alone


def _apply_ident_preservation(existing_text: str, new_text: str
                               ) -> tuple[str, list[tuple[str, str, str]]]:
    """Overlay [[identification]] hand-tunes from existing onto new_text.

    Preserves:
      - cid_address: when defgen emits 0x00000000 and existing is non-zero.
      - ecu_part:    when defgen emits empty string and existing is non-empty.
      - cid_scan:    when defgen omits it but existing has it (defgen never
                     emits cid_scan; we add the line back).

    Returns (rewritten_new_text, [(field, kept, dropped)]).
    """
    overrides: list[tuple[str, str, str]] = []
    out_text = new_text
    for field, default_repr in _PRESERVABLE_IDENT_FIELDS.items():
        existing = _extract_ident_field(existing_text, field)
        if existing is None:
            continue  # not in existing, nothing to preserve
        existing_val = existing[1]
        regenerated = _extract_ident_field(out_text, field)
        if regenerated is None:
            # defgen didn't emit this field at all (e.g. cid_scan). Insert it.
            if default_repr is None or existing_val != default_repr:
                out_text = _insert_ident_field(out_text, field, existing_val)
                overrides.append((field, existing_val, "<absent>"))
            continue
        new_val = regenerated[1]
        if default_repr is None:
            continue  # not a "preserve over sentinel" field
        if new_val != default_repr:
            continue  # defgen emitted a real value; trust it
        if existing_val == default_repr:
            continue  # nothing meaningful to preserve
        out_text = _replace_ident_field(out_text, field, existing_val)
        overrides.append((field, existing_val, new_val))
    return (out_text, overrides)


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


def _regen_pack(pack_path: Path,
                source_map: dict[str, tuple[Path, str, str]],
                only: set[str] | None, *, dry_run: bool = False
                ) -> tuple[str, str]:
    """Regenerate one pack. Returns (status, detail).

    status is one of "regen", "skip", "error", "no-change". Detail is a
    short human-readable explanation for logs.

    `dry_run=True` runs the full pipeline (XML parse, defgen re-emit,
    includes re-insertion, [pack] field preservation, no-op detection)
    but skips the final write. The returned (status, detail) is
    otherwise identical, so dry-run output accurately previews what a
    live run would do — including preservation reports.
    """
    cid = pack_path.stem.upper()
    if only is not None and cid not in only:
        return ("skip", "filtered out by --only")

    # Pick the source XML from the unified map.
    entry = source_map.get(cid)
    if entry is None:
        return ("skip", f"CID {cid} not in any configured XML source — "
                        f"manual regen required")
    xml_path, _subdir, xml_cid = entry

    if not xml_path.is_file():
        return ("error", f"source XML missing: {xml_path}")

    # Capture the existing includes line.
    existing_text = pack_path.read_text(encoding="utf-8")
    includes_line = _extract_includes_line(existing_text)

    # Re-emit via defgen. Pass the mixed-case CID from the XML so the
    # filter matches Subaru's case-sensitive VB-era xmlid spellings.
    try:
        xml_text = xml_path.read_text(encoding="utf-8", errors="replace")
        packs = defgen.parse_rom_xml(xml_text, rom_id_filter=xml_cid)
    except Exception as e:
        return ("error", f"defgen.parse_rom_xml failed: {e}")
    if not packs:
        return ("error", f"no pack for {xml_cid} in {xml_path.name}")
    if len(packs) > 1:
        return ("error", f"unexpected — XML returned {len(packs)} packs "
                         f"for romid {xml_cid}")
    new_text = defgen.pack_to_toml(packs[0])

    # Re-insert the includes line if the original had one.
    if includes_line:
        new_text = _insert_includes_line(new_text, includes_line)

    # Preserve [pack] fields the existing pack manually patched (e.g.,
    # rom_size_bytes when the XML omitted <filesize>). See
    # _apply_field_preservation for the rule.
    new_text, preservations = _apply_field_preservation(existing_text, new_text)
    # Same for [[identification]] hand-tunes (cid_address sibling-pattern,
    # cid_scan safety net, ecu_part).
    new_text, ident_preservations = _apply_ident_preservation(existing_text, new_text)
    preservations = preservations + ident_preservations

    # No-op detection: skip writing if the regenerated text matches.
    if new_text == existing_text:
        return ("no-change", "already in sync")

    if not dry_run:
        pack_path.write_text(new_text, encoding="utf-8")
        verb = "wrote"
    else:
        verb = "would write"
    detail = f"{verb} {len(new_text)} bytes from {xml_path.name}"
    if preservations:
        preserved_summary = ", ".join(
            f"{field}={kept}" for field, kept, _ in preservations)
        detail += f"; preserved {preserved_summary}"
    return ("regen", detail)


def _create_pack(xml_cid: str, xml_path: Path, default_subdir: str,
                  definitions_dir: Path, *, dry_run: bool = False
                  ) -> tuple[str, str]:
    """Create a brand-new pack file for a CID present in a bundle but not
    yet in definitions/. Returns (status, detail) using the same vocabulary
    as _regen_pack. `xml_cid` is the original-case CID from the XML."""
    if not default_subdir:
        return ("skip", "no default_subdir configured for this bundle")
    target = definitions_dir / default_subdir / f"{xml_cid.lower()}.toml"
    if target.exists():
        return ("skip", "already exists (handled by regen pass)")
    try:
        xml_text = xml_path.read_text(encoding="utf-8", errors="replace")
        packs = defgen.parse_rom_xml(xml_text, rom_id_filter=xml_cid)
    except Exception as e:
        return ("error", f"defgen.parse_rom_xml failed: {e}")
    if not packs:
        return ("error", f"no pack for {xml_cid} in {xml_path.name}")
    new_text = defgen.pack_to_toml(packs[0])

    # Force platform consistency for new bundle-sourced packs.
    if NEW_PACK_PLATFORM_OVERRIDE:
        new_text = _replace_pack_field(new_text, "platform",
                                       f'"{NEW_PACK_PLATFORM_OVERRIDE}"')
    # Inject the standard includes line for this subdir if configured.
    includes_line = NEW_PACK_INCLUDES.get(default_subdir)
    if includes_line:
        new_text = _insert_includes_line(new_text, includes_line)

    if not dry_run:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(new_text, encoding="utf-8")
        verb = "created"
    else:
        verb = "would create"
    return ("create", f"{verb} {target.relative_to(REPO_ROOT)} "
                       f"({len(new_text)} bytes from {xml_path.name})")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="bulk_regen")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would change without writing.")
    ap.add_argument("--only", nargs="*", default=None,
                    help="Restrict to specific CIDs (case-insensitive).")
    ap.add_argument("--create-missing", action="store_true",
                    help="After the regen pass, also create new packs for "
                         "CIDs that appear in a bundle XML but have no "
                         "existing pack under definitions/. Each new pack "
                         "lands in the bundle's configured default subdir.")
    args = ap.parse_args(argv)

    only = {c.upper() for c in args.only} if args.only is not None else None
    source_map = _cid_source_map()

    counts = {"regen": 0, "skip": 0, "error": 0, "no-change": 0, "create": 0}
    errors: list[tuple[Path, str]] = []

    definitions_dir = REPO_ROOT / "definitions"
    existing_cids: set[str] = set()
    for pack in sorted(definitions_dir.rglob("*.toml")):
        rel = pack.relative_to(REPO_ROOT)
        # Skip ecuparams and any non-pack root-level files.
        if any(part in SKIP_DIRS for part in rel.parts) or pack.parent == definitions_dir:
            continue
        existing_cids.add(pack.stem.upper())

        status, detail = _regen_pack(pack, source_map, only,
                                     dry_run=args.dry_run)
        # --only filtering happens inside _regen_pack and returns "skip";
        # don't count those as we'd never have considered them anyway.
        if status == "skip" and only is not None and detail == "filtered out by --only":
            continue
        counts[status] += 1

        if status == "error":
            errors.append((rel, detail))
            print(f"  [ERR]  {rel}: {detail}", file=sys.stderr)
        elif status == "regen":
            tag = "[dry]" if args.dry_run else "[ok] "
            print(f"  {tag}  {rel}: {detail}")
        elif status == "skip":
            print(f"  [skip] {rel}: {detail}")

    # Optional: create new packs for CIDs in bundles that have no existing pack.
    if args.create_missing:
        for upper_cid, (xml_path, default_subdir, xml_cid) in sorted(source_map.items()):
            if upper_cid in existing_cids:
                continue
            if only is not None and upper_cid not in only:
                continue
            if not default_subdir:
                # Master XML has no subdir mapping; skip — we don't auto-place
                # packs from the canonical EJ XML.
                continue
            status, detail = _create_pack(xml_cid, xml_path, default_subdir,
                                          definitions_dir,
                                          dry_run=args.dry_run)
            counts[status] = counts.get(status, 0) + 1
            if status == "error":
                errors.append((Path(f"<new {xml_cid}>"), detail))
                print(f"  [ERR]  {xml_cid}: {detail}", file=sys.stderr)
            elif status == "create":
                tag = "[dry]" if args.dry_run else "[new]"
                print(f"  {tag}  {detail}")

    print()
    summary = (f"summary: regen={counts['regen']} "
               f"no-change={counts['no-change']} "
               f"skip={counts['skip']} error={counts['error']}")
    if counts["create"]:
        summary += f" create={counts['create']}"
    summary += " (dry-run, no files written)" if args.dry_run else ""
    print(summary)
    if errors:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
