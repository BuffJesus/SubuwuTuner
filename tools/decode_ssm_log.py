#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Decode an `ssm-a8-poll` log against a hypothesis TSV so you can
eyeball physical values without running the full correlator.

The intended at-the-car use: after a smoke-test poll, you want to
know "is RPM ~800 at idle?" without doing big-endian arithmetic in
your head from the hex bytes. This script reads the log's address
list out of its header, looks each address up in the hypothesis
TSV (which maps cobb_column -> ssm_addr_24b + width + scaling),
and prints decoded values either per-row or as a summary.

The hypothesis TSV format is the one produced by the analyst-side
`ssm_a8_hypothesis.py` (one row per CSV column, with optional
ssm_addr_24b / width / scaling_expr / confidence). Rows without an
addr are skipped silently — they're the UNKNOWN columns awaiting
brute discovery.

Usage:
    python decode_ssm_log.py --self-test
    python decode_ssm_log.py --log <FILE.log> --hypothesis <FILE.tsv>
        [--mode rows|summary|tail] [--n 10] [--col NAME[,NAME...]]

Modes:
    rows     one line per poll: "<elapsed_ms>  col1=val  col2=val  ..."
    summary  one line per column: "<col>  min=X  mean=Y  max=Z  n=N"
    tail     same as rows but only the last --n entries (default 10).
             Useful at the car: skim the last few seconds of a live
             capture without scrolling thousands of rows.

The scaling expression is evaluated with restricted eval (no builtins,
only `x` in scope). Same approach as the analyst's correlate_ssm_to_cobb.

Companion: tools/cross_ref_ssm_a8.py (the full Pearson correlator),
docs/29-ssm-a8-poll-workflow.md (the workflow it slots into).
"""
from __future__ import annotations

import argparse
import csv
import math
import re
import struct
import sys
from collections import namedtuple
from pathlib import Path

SsmA8Row = namedtuple("SsmA8Row", "t_ms values")  # values: list[int|None]
Mapping = namedtuple("Mapping", "col_name unit addr width scaling_expr confidence")


def parse_ssm_a8_log(path: Path) -> tuple[list[int], list[SsmA8Row]]:
    """Parse a `subuwutuner-cli ssm-a8-poll` log.

    Returns (addresses, rows). Same format as cross_ref_ssm_a8.py's
    parse_ssm_a8_log; duplicated here so this script stays standalone.
    """
    addresses: list[int] = []
    rows: list[SsmA8Row] = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line.startswith("# addresses(BE24):"):
                tail = line.split(":", 1)[1].strip()
                addresses = [int(tok, 16) for tok in tail.split()]
            continue
        parts = line.split("\t", 1)
        if len(parts) != 2:
            continue
        try:
            t_ms = int(parts[0])
        except ValueError:
            continue
        payload = parts[1].strip()
        if payload.startswith("!"):
            rows.append(SsmA8Row(t_ms=t_ms, values=[None] * len(addresses)))
            continue
        try:
            bs = [int(tok, 16) for tok in payload.split()]
        except ValueError:
            continue
        if len(bs) != len(addresses):
            continue
        rows.append(SsmA8Row(t_ms=t_ms, values=bs))
    return addresses, rows


def parse_hypothesis(path: Path) -> list[Mapping]:
    """Parse the analyst hypothesis TSV. Rows with empty ssm_addr_24b
    are skipped (they're the UNKNOWN columns awaiting brute discovery).
    Expected columns: cobb_column, unit, ssm_addr_24b, width,
    scaling_expr, catalog_name, confidence."""
    out: list[Mapping] = []
    with path.open(encoding="utf-8") as f:
        rdr = csv.DictReader(f, delimiter="\t")
        for r in rdr:
            addr_str = (r.get("ssm_addr_24b") or "").strip()
            if not addr_str:
                continue
            try:
                addr = int(addr_str, 16)
                width = int((r.get("width") or "0").strip())
            except ValueError:
                continue
            if width not in (1, 2, 4):
                continue
            scaling = (r.get("scaling_expr") or "x").strip() or "x"
            out.append(Mapping(
                col_name=(r.get("cobb_column") or "").strip(),
                unit=(r.get("unit") or "").strip(),
                addr=addr,
                width=width,
                scaling_expr=scaling,
                confidence=(r.get("confidence") or "").strip(),
            ))
    return out


def be_combine(bs: list[int | None], offset: int, width: int) -> int | None:
    """Combine `width` consecutive bytes from `bs` starting at `offset`
    into a big-endian unsigned integer. Returns None on any None byte
    (error mid-stream) or out-of-range offset."""
    if offset + width > len(bs):
        return None
    v = 0
    for k in range(width):
        b = bs[offset + k]
        if b is None:
            return None
        v = (v << 8) | (b & 0xFF)
    return v


def apply_expr(raw: int, expr: str) -> float:
    """Evaluate `expr` with `x = raw`. Restricted to arithmetic + bitops;
    `__builtins__` is stripped so attribute access has no surface.

    TRUST ASSUMPTION: `expr` comes from a hypothesis TSV the operator
    wrote (or pulled from a trusted analyst pipeline). The empty-
    builtins eval defense neutralizes the common attacks but is
    escapable via type-subclass introspection — do NOT feed this an
    untrusted TSV from the internet.
    """
    if expr.strip() == "x":
        return float(raw)
    return float(eval(expr, {"__builtins__": {}}, {"x": raw}))


def build_column_index(addresses: list[int],
                       mappings: list[Mapping]) -> list[tuple[Mapping, int]]:
    """For each mapping whose `addr..addr+width-1` lies contiguously in
    `addresses`, emit (mapping, offset_in_row). Mappings whose addresses
    aren't all in the poll are skipped (couldn't be decoded anyway)."""
    addr_to_off = {a: i for i, a in enumerate(addresses)}
    out: list[tuple[Mapping, int]] = []
    for m in mappings:
        slots = [addr_to_off.get(m.addr + k) for k in range(m.width)]
        if any(s is None for s in slots):
            continue
        # Require contiguity (offsets monotonically increasing by 1)
        # — anything else means the poll request order doesn't match
        # the assumed byte layout for this column.
        for k in range(1, m.width):
            if slots[k] != slots[0] + k:
                slots = None
                break
        if slots is None:
            continue
        out.append((m, slots[0]))
    return out


def decode_row(row: SsmA8Row,
               cols: list[tuple[Mapping, int]],
               col_filter: set[str] | None) -> list[tuple[Mapping, float | None]]:
    """Decode every column in `cols` for one row. Returns
    (mapping, physical_value or None on failure)."""
    out: list[tuple[Mapping, float | None]] = []
    for m, off in cols:
        if col_filter is not None and m.col_name not in col_filter:
            continue
        raw = be_combine(row.values, off, m.width)
        if raw is None:
            out.append((m, None))
            continue
        try:
            phys = apply_expr(raw, m.scaling_expr)
        except Exception:
            phys = None
        out.append((m, phys))
    return out


def format_value(v: float | None, unit: str) -> str:
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "?"
    # 2-3 sig figs for small, integer for large (RPM, etc).
    if abs(v) >= 100:
        s = f"{v:.0f}"
    elif abs(v) >= 1:
        s = f"{v:.2f}"
    else:
        s = f"{v:.3f}"
    return s + unit if unit else s


def print_rows(rows: list[SsmA8Row],
               cols: list[tuple[Mapping, int]],
               col_filter: set[str] | None) -> None:
    for r in rows:
        parts = [f"t={r.t_ms}ms"]
        for m, v in decode_row(r, cols, col_filter):
            parts.append(f"{m.col_name}={format_value(v, m.unit)}")
        print("  ".join(parts))


def print_summary(rows: list[SsmA8Row],
                  cols: list[tuple[Mapping, int]],
                  col_filter: set[str] | None) -> None:
    per_col: dict[str, tuple[Mapping, list[float]]] = {}
    for r in rows:
        for m, v in decode_row(r, cols, col_filter):
            entry = per_col.get(m.col_name)
            if entry is None:
                per_col[m.col_name] = (m, [])
            if v is not None and not (isinstance(v, float) and math.isnan(v)):
                per_col[m.col_name][1].append(v)
    if not per_col:
        print("(no decodable columns — check that the hypothesis addresses appear in the log)")
        return
    name_w = max(len(name) for name in per_col)
    for name in sorted(per_col):
        m, vals = per_col[name]
        if not vals:
            print(f"{name:<{name_w}}  no-data")
            continue
        lo, hi = min(vals), max(vals)
        mean = sum(vals) / len(vals)
        unit = m.unit
        print(
            f"{name:<{name_w}}  "
            f"min={format_value(lo, unit)}  "
            f"mean={format_value(mean, unit)}  "
            f"max={format_value(hi, unit)}  "
            f"n={len(vals)}"
        )


# -------------------- self-test --------------------

def _self_test() -> int:
    """Build a synthetic ssm-a8-poll log + hypothesis pair and confirm
    the decoder recovers expected RPM/coolant values. Exit 0 on
    success."""
    import tempfile
    addrs = [0x00000E, 0x00000F, 0x000008]  # RPM hi/lo, coolant
    # Fake 5 polls: RPM = 800, 805, 810, 815, 820 (raw = rpm * 4).
    # Coolant raw = 130 (90°C as x - 40, but our scaling is x/2 - 40
    # for synthetic just to vary; we'll set it as the canonical x - 40).
    rows_raw = []
    for i, rpm in enumerate([800, 805, 810, 815, 820]):
        raw = rpm * 4
        hi = (raw >> 8) & 0xFF
        lo = raw & 0xFF
        coolant_raw = 130  # x - 40 = 90°C
        rows_raw.append((i * 100, [hi, lo, coolant_raw]))

    with tempfile.TemporaryDirectory() as td:
        log_path = Path(td) / "ssm.log"
        hyp_path = Path(td) / "hyp.tsv"
        with log_path.open("w", encoding="utf-8") as f:
            f.write("# subuwutuner ssm-a8-poll log v1\n")
            f.write("# transport: mock\n")
            f.write("# interval_ms: 100\n")
            f.write("# sa: none\n")
            f.write(
                "# addresses(BE24): " + " ".join(f"0x{a:06X}" for a in addrs) + "\n"
            )
            f.write(
                "# format: <elapsed_ms>\\t<HH HH ...>  (one byte per address, request order)\n"
            )
            for t_ms, bs in rows_raw:
                f.write(f"{t_ms}\t" + " ".join(f"{b:02X}" for b in bs) + "\n")

        with hyp_path.open("w", encoding="utf-8", newline="") as f:
            w = csv.writer(f, delimiter="\t")
            w.writerow([
                "cobb_column", "unit", "ssm_addr_24b", "width",
                "scaling_expr", "catalog_name", "confidence",
            ])
            w.writerow(["RPM", "RPM", "0x00000E", "2", "x/4", "Engine - RPM", "MANUAL"])
            w.writerow(["Coolant", "C", "0x000008", "1", "x-40", "Coolant", "MANUAL"])
            w.writerow(["NotInLog", "", "0xF8AAAA", "1", "x", "missing", "MANUAL"])

        addresses, rows = parse_ssm_a8_log(log_path)
        mappings = parse_hypothesis(hyp_path)
        cols = build_column_index(addresses, mappings)

        # Should have indexed RPM + Coolant; NotInLog should have been skipped.
        names = sorted(m.col_name for m, _ in cols)
        if names != ["Coolant", "RPM"]:
            print(f"  FAIL: indexed columns {names} != ['Coolant', 'RPM']",
                  file=sys.stderr)
            return 1

        # Decode first row, expect RPM=800, Coolant=90.
        decoded = decode_row(rows[0], cols, None)
        d = {m.col_name: v for m, v in decoded}
        if abs(d.get("RPM", 0) - 800.0) > 0.5:
            print(f"  FAIL: row 0 RPM = {d.get('RPM')} (expected 800)",
                  file=sys.stderr)
            return 1
        if abs(d.get("Coolant", 0) - 90.0) > 0.5:
            print(f"  FAIL: row 0 Coolant = {d.get('Coolant')} (expected 90)",
                  file=sys.stderr)
            return 1
        print(f"  row 0: RPM={d['RPM']:.1f}  Coolant={d['Coolant']:.1f}")
        print("  OK")
        return 0


# -------------------- CLI --------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Decode an ssm-a8-poll log via a hypothesis TSV")
    ap.add_argument("--self-test", action="store_true",
                    help="run synthetic-data decode + exit 0 on success")
    ap.add_argument("--log", type=Path, default=None,
                    help="path to a subuwutuner-cli ssm-a8-poll log")
    ap.add_argument("--hypothesis", type=Path, default=None,
                    help="path to the hypothesis TSV (cobb_column, "
                         "ssm_addr_24b, width, scaling_expr, ...)")
    ap.add_argument("--mode", choices=["rows", "summary", "tail"],
                    default="summary",
                    help="rows: per-poll decoded line; summary: min/mean/max "
                         "per column (default); tail: last --n rows")
    ap.add_argument("--n", type=int, default=10,
                    help="row count for --mode tail (default 10)")
    ap.add_argument("--col", type=str, default=None,
                    help="comma-separated column names to filter to")
    args = ap.parse_args()

    if args.self_test:
        print("self-test: synthetic decode")
        return _self_test()

    if args.log is None or args.hypothesis is None:
        print("error: --log and --hypothesis are both required "
              "(or use --self-test)", file=sys.stderr)
        return 2

    addresses, rows = parse_ssm_a8_log(args.log)
    if not addresses:
        print(f"error: no '# addresses(BE24):' header found in {args.log}",
              file=sys.stderr)
        return 1
    if not rows:
        print(f"error: no data rows in {args.log}", file=sys.stderr)
        return 1

    mappings = parse_hypothesis(args.hypothesis)
    cols = build_column_index(addresses, mappings)
    if not cols:
        print(
            f"warning: 0 hypothesis columns map cleanly into the poll's "
            f"{len(addresses)} addresses — nothing to decode. "
            f"Check that the poll's --addr list matches the hypothesis "
            f"TSV ssm_addr_24b values.",
            file=sys.stderr,
        )
        return 1

    col_filter: set[str] | None = None
    if args.col:
        col_filter = {c.strip() for c in args.col.split(",") if c.strip()}

    print(f"# log: {args.log.name}  rows: {len(rows)}  "
          f"decoded columns: {len(cols)}")
    if args.mode == "rows":
        print_rows(rows, cols, col_filter)
    elif args.mode == "tail":
        print_rows(rows[-args.n:], cols, col_filter)
    else:  # summary
        print_summary(rows, cols, col_filter)
    return 0


if __name__ == "__main__":
    sys.exit(main())
