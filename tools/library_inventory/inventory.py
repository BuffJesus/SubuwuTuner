#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Local tune-library inventory.

Walks one or more directories for `.ptm` tune files and `.stune` project
directories. Hashes every `.ptm` (MD5 — matches the AP's `/backupcksum`
format), parses filenames for vendor/variant hints, dedups by hash, and
emits a unified library index.

The output index is the data structure the eventual in-tool "currently-
flashed identification" feature will consume: when an AP is connected,
SubuwuTuner pulls `/backupcksum` (a 32-char MD5) and looks it up in the
index to surface "Currently flashed: <name> (in your library)".

This script is intentionally standalone — no SubuwuTuner C++ build is
required, no Python dependencies beyond the standard library. It works
on any machine with Python 3.10+.

Run:
    python tools/library_inventory/inventory.py                # default paths
    python tools/library_inventory/inventory.py --scan D:/maps --scan ~/tunes
    python tools/library_inventory/inventory.py --json         # JSON to stdout
    python tools/library_inventory/inventory.py --out-dir ./my-library-index

See tools/library_inventory/README.md for output format + caller flow.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Iterable, Optional


# Canonical default scan paths. The user can override with --scan.
# Order matters only for tie-breaking when the same hash appears in
# multiple places — earlier paths "win" the canonical_path field.
DEFAULT_SCAN_PATHS = [
    Path("D:/Subuwu/ap-maps"),
    Path("D:/Documents/Tuner"),
    Path("D:/Documents/Tuner/Projects"),
    Path.home() / "Documents" / "SubuwuTuner",
    Path.home() / "Documents" / "Tuner",
]


@dataclasses.dataclass
class PtmEntry:
    """One row in the library index for a `.ptm` tune file."""

    path: str
    size: int
    mtime: int
    md5: str
    # Heuristic-parsed from filename. Empty when we can't tell.
    vendor: str = ""
    variant: str = ""  # e.g. "WRK3", "v401", "BigSF"
    fuel_grade: str = ""  # e.g. "91", "93", "E85"
    stage: str = ""  # e.g. "Stage0", "Stage1", "Stage2"
    # If a sibling _UNLOCKED file shares the same content hash, the
    # parent file's `unlocked_sibling` becomes the sibling's path. One
    # of them is the canonical entry; the other gets dropped during
    # dedup (we keep the shorter filename / the non-_UNLOCKED variant).
    unlocked_sibling: str = ""


@dataclasses.dataclass
class StuneEntry:
    """One row in the library index for a `.stune` project."""

    path: str
    project_name: str = ""
    # [ptm_metadata] block if present — indicates the project was
    # produced by `ptm import` and round-trips back to a .ptm.
    ptm_vendor_id: str = ""
    ptm_vehicle_id: str = ""
    ptm_rom_sum: str = ""
    # Path to the project's source.bin (the base ROM patches were
    # authored against). Useful for the library cross-reference.
    source_bin: str = ""
    source_bin_md5: str = ""


def md5_of_file(p: Path) -> str:
    """MD5 hex digest of the file at `p`. Returns empty on read failure."""
    h = hashlib.md5()
    try:
        with p.open("rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return ""


# Filename heuristics. The patterns below are observed conventions across
# the corpora the project has analyzed; each one is a "weak signal" —
# matching just means "consistent with this label," not a guarantee.
# When a pattern doesn't match, the corresponding field stays empty.
_VENDOR_PATTERNS = [
    (re.compile(r"^(Stage\d)", re.I), "COBB"),
    (re.compile(r"COBB\b", re.I), "COBB"),
    (re.compile(r"NexGen\b", re.I), "NexGen"),
    (re.compile(r"\bFehr_?", re.I), "Fehr"),
    (re.compile(r"\bDMann\b", re.I), "Fehr"),  # DMann is the tuner
    (re.compile(r"\bNTM\b", re.I), "NTM"),
    (re.compile(r"\bFelix\b", re.I), "Felix"),
    (re.compile(r"\bEpifan\b", re.I), "EpifanSoft"),
]

_STAGE_PATTERN = re.compile(r"\b(Stage[0-9])\b", re.I)
_FUEL_PATTERN = re.compile(r"\b(91|93|94|E85|E50|E30|Race)\b", re.I)
_VARIANT_PATTERNS = [
    re.compile(r"\b(WRK[0-9]+)\b", re.I),
    re.compile(r"\b(v\d{2,3})\b", re.I),
    re.compile(r"\b(Redline|SF|BigSF|Economy|Valet|HWG|LWG)\b", re.I),
]


def parse_filename_hints(name: str) -> dict:
    """Best-effort vendor / stage / fuel / variant from a `.ptm` filename."""
    out: dict[str, str] = {}
    for pat, vendor in _VENDOR_PATTERNS:
        if pat.search(name):
            out["vendor"] = vendor
            break
    if m := _STAGE_PATTERN.search(name):
        out["stage"] = m.group(1)
    if m := _FUEL_PATTERN.search(name):
        out["fuel_grade"] = m.group(1)
    variants: list[str] = []
    for pat in _VARIANT_PATTERNS:
        for m in pat.finditer(name):
            variants.append(m.group(1))
    if variants:
        out["variant"] = " / ".join(variants)
    return out


def parse_stune_metadata(project_dir: Path) -> StuneEntry:
    """Pull [ptm_metadata] + source.bin hash from a project.toml."""
    entry = StuneEntry(path=str(project_dir))
    manifest = project_dir / "project.toml"
    if not manifest.is_file():
        return entry
    try:
        # tomllib is stdlib since Python 3.11. Fall back to a minimal
        # line-scanner for 3.10 if tomllib isn't there.
        try:
            import tomllib  # type: ignore[import-not-found]

            with manifest.open("rb") as f:
                doc = tomllib.load(f)
        except ImportError:
            doc = _toml_minimal_parse(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return entry

    project = doc.get("project", {}) if isinstance(doc, dict) else {}
    if isinstance(project, dict):
        entry.project_name = str(project.get("display_name", "") or "")
        source_rom = project.get("source_rom", {})
        if isinstance(source_rom, dict):
            rel = source_rom.get("path", "")
            if isinstance(rel, str) and rel:
                entry.source_bin = str(project_dir / rel)
                if Path(entry.source_bin).is_file():
                    entry.source_bin_md5 = md5_of_file(Path(entry.source_bin))

    meta = doc.get("ptm_metadata", {}) if isinstance(doc, dict) else {}
    if isinstance(meta, dict):
        entry.ptm_vendor_id = str(meta.get("vendor_id", "") or "")
        entry.ptm_vehicle_id = str(meta.get("vehicle_id", "") or "")
        entry.ptm_rom_sum = str(meta.get("rom_sum", "") or "")
    return entry


def _toml_minimal_parse(text: str) -> dict:
    """Tiny TOML scanner — enough to pull `[project]` + `[ptm_metadata]`.

    Used as a fallback when tomllib isn't available (Python 3.10). Only
    handles the shape `project.toml` actually emits: top-level tables,
    string keys, nested-table dotted paths. Not a real TOML parser.
    """
    out: dict[str, dict] = {}
    section: Optional[dict] = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            keys = line[1:-1].strip().split(".")
            cur = out
            for k in keys:
                cur = cur.setdefault(k, {})
            section = cur
            continue
        if "=" not in line or section is None:
            continue
        k, _, v = line.partition("=")
        v = v.strip()
        if v.startswith('"') and v.endswith('"'):
            v = v[1:-1]
        section[k.strip()] = v
    return out


def scan_path(root: Path) -> tuple[list[PtmEntry], list[StuneEntry]]:
    """Walk one root path, return (ptm_entries, stune_entries)."""
    ptms: list[PtmEntry] = []
    stunes: list[StuneEntry] = []
    if not root.is_dir():
        return ptms, stunes
    for entry in root.rglob("*"):
        # .stune projects — treat the dir as the entry, then skip its
        # interior so we don't scan it as loose .ptm hosts.
        if entry.is_dir() and entry.suffix.lower() == ".stune":
            stunes.append(parse_stune_metadata(entry))
            continue
        if entry.is_file() and entry.suffix.lower() == ".ptm":
            md5 = md5_of_file(entry)
            if not md5:
                continue
            try:
                stat = entry.stat()
            except OSError:
                continue
            hints = parse_filename_hints(entry.name)
            ptms.append(
                PtmEntry(
                    path=str(entry),
                    size=stat.st_size,
                    mtime=int(stat.st_mtime),
                    md5=md5,
                    vendor=hints.get("vendor", ""),
                    variant=hints.get("variant", ""),
                    fuel_grade=hints.get("fuel_grade", ""),
                    stage=hints.get("stage", ""),
                )
            )
    return ptms, stunes


def dedupe_ptms(entries: list[PtmEntry]) -> list[PtmEntry]:
    """Group by md5; keep one canonical entry per group.

    Tie-break: prefer the entry whose filename does NOT end in
    `_UNLOCKED`, and otherwise the shortest path. The dropped sibling's
    path goes into the canonical's `unlocked_sibling` field so the user
    can still find it.
    """
    by_hash: dict[str, list[PtmEntry]] = {}
    for e in entries:
        by_hash.setdefault(e.md5, []).append(e)
    out: list[PtmEntry] = []
    for md5, group in by_hash.items():
        if len(group) == 1:
            out.append(group[0])
            continue

        def rank(e: PtmEntry) -> tuple[int, int, str]:
            # Lower rank = canonical. Non-_UNLOCKED beats _UNLOCKED;
            # shorter path beats longer.
            unlocked = 1 if "_UNLOCKED" in e.path else 0
            return (unlocked, len(e.path), e.path)

        group.sort(key=rank)
        canonical = group[0]
        # Best-effort: surface the first sibling as the .unlocked_sibling
        # pointer. Multi-way dupes (>2) get just the first.
        if len(group) > 1:
            canonical.unlocked_sibling = group[1].path
        out.append(canonical)
    return out


def render_index_toml(ptms: list[PtmEntry], stunes: list[StuneEntry]) -> str:
    """Emit the library index as a single TOML document."""
    lines: list[str] = []
    lines.append("# SubuwuTuner library index — generated by")
    lines.append("# tools/library_inventory/inventory.py")
    lines.append("# Hash is MD5 to match the AP's /backupcksum format.")
    lines.append("")
    lines.append("schema = \"subuwutuner.library-index.v1\"")
    lines.append(f"generator = \"library_inventory/inventory.py\"")
    lines.append(f"ptm_count = {len(ptms)}")
    lines.append(f"stune_count = {len(stunes)}")
    lines.append("")
    for e in sorted(ptms, key=lambda x: (x.vendor, x.stage, x.variant, x.path)):
        lines.append("[[ptm]]")
        lines.append(f'  path        = {json.dumps(e.path)}')
        lines.append(f"  size        = {e.size}")
        lines.append(f"  mtime       = {e.mtime}")
        lines.append(f'  md5         = "{e.md5}"')
        if e.vendor:
            lines.append(f'  vendor      = "{e.vendor}"')
        if e.stage:
            lines.append(f'  stage       = "{e.stage}"')
        if e.fuel_grade:
            lines.append(f'  fuel_grade  = "{e.fuel_grade}"')
        if e.variant:
            lines.append(f'  variant     = "{e.variant}"')
        if e.unlocked_sibling:
            lines.append(f'  unlocked_sibling = {json.dumps(e.unlocked_sibling)}')
        lines.append("")
    for e in sorted(stunes, key=lambda x: x.path):
        lines.append("[[stune]]")
        lines.append(f'  path             = {json.dumps(e.path)}')
        if e.project_name:
            lines.append(f'  project_name     = {json.dumps(e.project_name)}')
        if e.ptm_vendor_id:
            lines.append(f'  ptm_vendor_id    = {json.dumps(e.ptm_vendor_id)}')
        if e.ptm_vehicle_id:
            lines.append(f'  ptm_vehicle_id   = {json.dumps(e.ptm_vehicle_id)}')
        if e.ptm_rom_sum:
            lines.append(f'  ptm_rom_sum      = {json.dumps(e.ptm_rom_sum)}')
        if e.source_bin:
            lines.append(f'  source_bin       = {json.dumps(e.source_bin)}')
        if e.source_bin_md5:
            lines.append(f'  source_bin_md5   = "{e.source_bin_md5}"')
        lines.append("")
    return "\n".join(lines)


def render_summary_md(ptms: list[PtmEntry], stunes: list[StuneEntry], scanned: Iterable[Path]) -> str:
    """Human-readable summary report (Markdown)."""
    lines: list[str] = []
    lines.append("# Tune library inventory")
    lines.append("")
    lines.append("Scanned paths:")
    for p in scanned:
        lines.append(f"- `{p}`")
    lines.append("")
    lines.append(f"**Found:** {len(ptms)} unique .ptm files, {len(stunes)} .stune projects.")
    lines.append("")

    # Group by vendor for a quick visual.
    by_vendor: dict[str, list[PtmEntry]] = {}
    for e in ptms:
        by_vendor.setdefault(e.vendor or "(unknown)", []).append(e)
    lines.append("## .ptm files by vendor")
    lines.append("")
    for vendor in sorted(by_vendor.keys()):
        entries = by_vendor[vendor]
        lines.append(f"### {vendor} ({len(entries)})")
        lines.append("")
        for e in sorted(entries, key=lambda x: (x.stage, x.variant, x.path)):
            tags: list[str] = []
            if e.stage:
                tags.append(e.stage)
            if e.fuel_grade:
                tags.append(f"{e.fuel_grade} oct")
            if e.variant:
                tags.append(e.variant)
            tag_str = f" — {', '.join(tags)}" if tags else ""
            size_kb = e.size / 1024.0
            name = Path(e.path).name
            lines.append(f"- `{name}` ({size_kb:.1f} KB, MD5 `{e.md5}`){tag_str}")
        lines.append("")

    if stunes:
        lines.append("## .stune projects")
        lines.append("")
        for e in sorted(stunes, key=lambda x: x.path):
            line = f"- `{Path(e.path).name}`"
            if e.ptm_vendor_id or e.ptm_vehicle_id:
                line += f" — imported from {e.ptm_vendor_id or '?'} / {e.ptm_vehicle_id or '?'}"
            if e.project_name:
                line += f" ({e.project_name})"
            lines.append(line)
        lines.append("")

    lines.append("## Cross-reference hint")
    lines.append("")
    lines.append("If you have an AP plugged in, `subuwutuner-cli ets pull /backupcksum`")
    lines.append("returns the MD5 of the currently-flashed ROM. Grep this report for")
    lines.append("that MD5 to identify which tune is currently on the ECU.")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--scan",
        action="append",
        default=None,
        help="Directory to scan. Repeatable. Defaults to several known paths.",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path("."),
        help="Where to write library_index.toml + library_summary.md (default: cwd).",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="Emit a JSON document on stdout instead of writing files.",
    )
    ap.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress progress output.",
    )
    ap.add_argument(
        "--query",
        metavar="MD5",
        default=None,
        help="Look up a single 32-char MD5 hash. Prints the matching "
             "library entry as a single line (path / vendor / variant) "
             "and exits 0 on match, 1 on miss. The hash is what you'd "
             "get from `subuwutuner-cli ets pull /backupcksum`. Pipeline-"
             "friendly: skips the file output entirely.",
    )
    args = ap.parse_args(argv)

    # --query mode short-circuits the scan-write flow. It STILL scans
    # (no cache yet — that's a Phase 2 follow-up), but emits only the
    # match and skips the index/summary file writes.
    if args.query is not None:
        target = args.query.strip().lower()
        # Tolerate "MD5 (hash)" or "0x..." prefixes the user might
        # paste from somewhere. Strip non-hex; keep the trailing 32.
        target = "".join(c for c in target if c in "0123456789abcdef")
        if len(target) != 32:
            print(f"--query: expected a 32-char hex MD5, got {len(target)} hex chars after "
                  f"normalization (input: {args.query!r})", file=sys.stderr)
            return 2

    scan_paths = [Path(p) for p in (args.scan or [str(p) for p in DEFAULT_SCAN_PATHS])]
    all_ptms: list[PtmEntry] = []
    all_stunes: list[StuneEntry] = []
    actually_scanned: list[Path] = []
    for root in scan_paths:
        if not root.is_dir():
            if not args.quiet and args.query is None:
                print(f"  skip (not a directory): {root}", file=sys.stderr)
            continue
        actually_scanned.append(root)
        if not args.quiet and args.query is None:
            print(f"  scanning: {root}", file=sys.stderr)
        ptms, stunes = scan_path(root)
        all_ptms.extend(ptms)
        all_stunes.extend(stunes)

    deduped = dedupe_ptms(all_ptms)

    # --query: short-circuit. Find the hit, print it, exit.
    if args.query is not None:
        for e in deduped:
            if e.md5 == target:
                # Pipeline-friendly single-line output: tab-separated
                # fields. Stable column order so awk + cut work cleanly.
                parts = [
                    e.md5,
                    e.vendor or "-",
                    e.stage or "-",
                    e.variant or "-",
                    e.fuel_grade or "-",
                    e.path,
                ]
                print("\t".join(parts))
                return 0
        if not args.quiet:
            print(f"--query: no library entry matches MD5 {target}",
                  file=sys.stderr)
        return 1

    if not args.quiet:
        dup_count = len(all_ptms) - len(deduped)
        msg = f"  found {len(deduped)} unique .ptm files"
        if dup_count:
            msg += f" ({dup_count} content-dups collapsed)"
        msg += f", {len(all_stunes)} .stune projects"
        print(msg, file=sys.stderr)

    if args.json:
        # Single JSON dump, no files written.
        doc = {
            "schema": "subuwutuner.library-index.v1",
            "ptm_count": len(deduped),
            "stune_count": len(all_stunes),
            "scanned": [str(p) for p in actually_scanned],
            "ptm": [dataclasses.asdict(e) for e in deduped],
            "stune": [dataclasses.asdict(e) for e in all_stunes],
        }
        json.dump(doc, sys.stdout, indent=2, sort_keys=False)
        sys.stdout.write("\n")
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    index_path = args.out_dir / "library_index.toml"
    summary_path = args.out_dir / "library_summary.md"
    index_path.write_text(render_index_toml(deduped, all_stunes), encoding="utf-8")
    summary_path.write_text(render_summary_md(deduped, all_stunes, actually_scanned), encoding="utf-8")
    if not args.quiet:
        print(f"  wrote {index_path}", file=sys.stderr)
        print(f"  wrote {summary_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
