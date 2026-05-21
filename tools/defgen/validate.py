#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
validate — run descriptor predicates against a (pack, ROM) pair.

Consumer-side bridge between descriptors.py and the in-tree pack corpus.
For each table/axis in the pack, look up applicable descriptors by id,
decode the ROM bytes at the declared address, and report whether the
bytes actually look like what the descriptor expects.

Three terminal verdicts per entry:

  PASS           — at least one descriptor matched the entry's bytes.
  FAIL           — one or more descriptors matched by id but the bytes
                   failed the predicate (range, monotonicity, shape).
                   The reasons string explains why.
  NO_DESCRIPTOR  — no descriptor's id_patterns match this entry's id.
                   Informational, not an error: the descriptor library
                   is hand-curated and coverage grows over time.

Usage:
    python validate.py --pack definitions/legacy/a2tb002c.toml \\
                       --rom  fixtures/.../A2TB002C.bin \\
                       [--tsv] [--kind table|axis] [--only ID]

Exit codes:
    0  every entry that had a matching descriptor PASSed
    1  one or more FAIL verdicts (cited descriptor rejected the bytes)
    2  argument / file errors
"""

from __future__ import annotations

import argparse
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import descriptors


# ---------------------------------------------------------------------------
# Pack loading
# ---------------------------------------------------------------------------

@dataclass
class PackEntry:
    """A pack [[table]] or [[axis]] record reduced to what validate needs."""
    kind: str          # "table" or "axis"
    id: str
    address: int
    data_type: str
    dimensions: int    # 0/1/2 for tables; axes get dims=1 here
    rows: int = 0
    cols: int = 0
    length: int = 0    # axes
    axis_x: str = ""
    axis_y: str = ""


def load_pack_entries(pack_path: Path) -> tuple[list[PackEntry], dict[str, int]]:
    """Read a pack TOML and return (entries, axis_lengths_by_id).

    Resolves the axis-length lookup that 2D tables need: a [[table]]
    block carries axis_x/axis_y by id, and we want their `length` to
    populate DecodeHint(rows=..., cols=...). The dict is keyed by axis
    id and returns length.
    """
    raw = tomllib.loads(pack_path.read_text(encoding="utf-8"))
    axis_length: dict[str, int] = {}
    entries: list[PackEntry] = []

    for a in raw.get("axis", []) or []:
        aid = str(a.get("id", "")).strip()
        if not aid:
            continue
        length = int(a.get("length", 0) or 0)
        axis_length[aid] = length
        entries.append(PackEntry(
            kind="axis",
            id=aid,
            address=int(a.get("address", 0) or 0),
            data_type=str(a.get("data_type", "uint16_be") or "uint16_be"),
            dimensions=1,
            length=length,
        ))

    for t in raw.get("table", []) or []:
        tid = str(t.get("id", "")).strip()
        if not tid:
            continue
        dims = int(t.get("dimensions", 0) or 0)
        ax_x = str(t.get("axis_x", "") or "")
        ax_y = str(t.get("axis_y", "") or "")
        # For 1D: cols = axis_x.length, rows = 1.
        # For 2D: cols = axis_x.length, rows = axis_y.length.
        cols = axis_length.get(ax_x, 0) if ax_x else 0
        rows = axis_length.get(ax_y, 0) if ax_y else 0
        entries.append(PackEntry(
            kind="table",
            id=tid,
            address=int(t.get("address", 0) or 0),
            data_type=str(t.get("data_type", "uint16_be") or "uint16_be"),
            dimensions=dims,
            rows=rows if dims == 2 else 0,
            cols=cols if dims >= 1 else 0,
            length=cols if dims == 1 else 0,
            axis_x=ax_x,
            axis_y=ax_y,
        ))
    return entries, axis_length


def hint_for(entry: PackEntry) -> descriptors.DecodeHint:
    """Build a DecodeHint from a pack entry. Caller decides what to skip."""
    return descriptors.DecodeHint(
        dtype=entry.data_type,
        dims=entry.dimensions,
        length=entry.length,
        rows=entry.rows,
        cols=entry.cols,
    )


# ---------------------------------------------------------------------------
# Verdict aggregation
# ---------------------------------------------------------------------------

@dataclass
class EntryReport:
    entry: PackEntry
    status: str  # "PASS", "FAIL", "NO_DESCRIPTOR", "SKIPPED"
    matched: list[tuple[str, descriptors.Verdict]]  # (descriptor_id, verdict)
    skip_reason: str = ""

    def best_match_label(self) -> str:
        if not self.matched:
            return ""
        # Sort: passing verdicts first, then by score desc.
        sorted_m = sorted(self.matched,
                          key=lambda mv: (mv[1].matches, mv[1].score),
                          reverse=True)
        did, v = sorted_m[0]
        if v.reasons:
            return f"{did}: {v.reasons[0]}"
        return did


def validate_entry(entry: PackEntry, rom: bytes) -> EntryReport:
    """Run all applicable descriptors against a single entry's ROM slice."""
    hint = hint_for(entry)
    cells = hint.total_cells()
    if cells <= 0 or hint.dims == 0:
        return EntryReport(entry, "SKIPPED", [],
                           skip_reason=f"dims={hint.dims} unhandled")
    try:
        width = hint.byte_width()
    except KeyError:
        return EntryReport(entry, "SKIPPED", [],
                           skip_reason=f"unknown dtype {entry.data_type}")
    n_bytes = cells * width
    if entry.address + n_bytes > len(rom):
        return EntryReport(entry, "SKIPPED", [],
                           skip_reason=f"address 0x{entry.address:x}+"
                                       f"{n_bytes}B exceeds ROM size "
                                       f"{len(rom)}")
    buf = rom[entry.address:entry.address + n_bytes]
    candidates = descriptors.candidates_for(entry.id, kind=entry.kind,
                                            dims=entry.dimensions)
    if not candidates:
        return EntryReport(entry, "NO_DESCRIPTOR", [])
    matched: list[tuple[str, descriptors.Verdict]] = []
    for d in candidates:
        v = d.predicate(buf, hint)
        matched.append((d.id, v))
    status = "PASS" if any(v.matches for _, v in matched) else "FAIL"
    return EntryReport(entry, status, matched)


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def print_human(reports: list[EntryReport], file=sys.stdout) -> None:
    counts = {"PASS": 0, "FAIL": 0, "NO_DESCRIPTOR": 0, "SKIPPED": 0}
    for r in reports:
        counts[r.status] = counts.get(r.status, 0) + 1

    total = sum(counts.values())
    covered = counts["PASS"] + counts["FAIL"]
    print(f"{'='*60}", file=file)
    print(f"validate.py — {total} entries", file=file)
    print(f"{'='*60}", file=file)
    print(f"  PASS              {counts['PASS']:5d}  "
          f"({counts['PASS']/total:6.1%})", file=file)
    print(f"  FAIL              {counts['FAIL']:5d}  "
          f"({counts['FAIL']/total:6.1%})", file=file)
    print(f"  NO_DESCRIPTOR     {counts['NO_DESCRIPTOR']:5d}  "
          f"({counts['NO_DESCRIPTOR']/total:6.1%})", file=file)
    print(f"  SKIPPED           {counts['SKIPPED']:5d}  "
          f"({counts['SKIPPED']/total:6.1%})", file=file)
    if covered:
        print(f"  Coverage          {covered/total:6.1%}  "
              f"(PASS/(PASS+FAIL+NO_DESCRIPTOR) excluding SKIPPED)",
              file=file)
    print(file=file)

    # Detail: list FAILs (most actionable), then a few PASSes for context.
    fails = [r for r in reports if r.status == "FAIL"]
    if fails:
        print(f"--- FAIL ({len(fails)}) ---", file=file)
        for r in fails:
            print(f"  {r.entry.kind:5s} {r.entry.id:50s} "
                  f"@0x{r.entry.address:08x}", file=file)
            for did, v in r.matched:
                tag = "fail" if not v.matches else "pass"
                reasons = "; ".join(v.reasons) if v.reasons else "(no detail)"
                print(f"        {tag} via {did}: {reasons}", file=file)
        print(file=file)

    passes = [r for r in reports if r.status == "PASS"]
    if passes:
        print(f"--- PASS ({len(passes)}) ---", file=file)
        for r in passes[:20]:
            label = r.best_match_label()
            print(f"  {r.entry.kind:5s} {r.entry.id:50s} "
                  f"@0x{r.entry.address:08x}  {label}", file=file)
        if len(passes) > 20:
            print(f"  ... ({len(passes) - 20} more)", file=file)


def print_tsv(reports: list[EntryReport], file=sys.stdout) -> None:
    print("kind\tid\taddress\tstatus\tdescriptor\tmatched\treasons", file=file)
    for r in reports:
        addr_hex = f"0x{r.entry.address:08x}"
        if r.status in ("NO_DESCRIPTOR", "SKIPPED"):
            reasons = r.skip_reason if r.status == "SKIPPED" else ""
            print(f"{r.entry.kind}\t{r.entry.id}\t{addr_hex}\t"
                  f"{r.status}\t\t\t{reasons}", file=file)
            continue
        for did, v in r.matched:
            reasons = "; ".join(v.reasons)
            print(f"{r.entry.kind}\t{r.entry.id}\t{addr_hex}\t"
                  f"{r.status}\t{did}\t{v.matches}\t{reasons}",
                  file=file)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="validate",
        description="Run descriptor predicates against a (pack, ROM) pair.")
    p.add_argument("--pack", required=True, type=Path,
                   help="Pack TOML to validate.")
    p.add_argument("--rom", required=True, type=Path,
                   help="ROM binary to read declared addresses from.")
    p.add_argument("--tsv", action="store_true",
                   help="Emit TSV instead of the human summary.")
    p.add_argument("--kind", choices=("table", "axis"), default="",
                   help="Restrict to table or axis entries (default: both).")
    p.add_argument("--only", default="",
                   help="Only process the entry whose id matches exactly.")
    return p


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    if not args.pack.is_file():
        print(f"validate: pack not found: {args.pack}", file=sys.stderr)
        return 2
    if not args.rom.is_file():
        print(f"validate: ROM not found: {args.rom}", file=sys.stderr)
        return 2

    entries, _axes = load_pack_entries(args.pack)
    if args.kind:
        entries = [e for e in entries if e.kind == args.kind]
    if args.only:
        entries = [e for e in entries if e.id == args.only]
    if not entries:
        print("validate: no entries matched the filter.", file=sys.stderr)
        return 2

    rom = args.rom.read_bytes()
    reports = [validate_entry(e, rom) for e in entries]
    if args.tsv:
        print_tsv(reports)
    else:
        print_human(reports)
    return 1 if any(r.status == "FAIL" for r in reports) else 0


if __name__ == "__main__":
    sys.exit(main())
