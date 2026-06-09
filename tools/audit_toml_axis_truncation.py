#!/usr/bin/env python3
"""
Audit subuwutuner def-pack TOML files for likely axis-truncation bugs.

Background: 2026-06-09 we discovered that lf79103p's wastegate-duty
tables had axes declared as length 7×8 but were actually 14×17 in the
ROM. Same pattern caught HPFP cluster (declared as 4-byte uint32, was
16-element uint8). Likely there are more.

This scanner flags candidate tables where the declared byte-extent
(cols × rows × sizeof(data_type)) is suspiciously small compared to
the address gap to the next declared table.

Methodology:
  For each [[table]] in each pack:
    - Compute declared_extent = cols × rows × dtype_bytes.
    - Find the smallest gap to any other table in the same pack
      whose address > this table's address.
    - If gap / declared_extent > threshold (default 2.0), flag.
    - If gap is exactly an integer multiple of one row's bytes
      (cols × dtype_bytes), the axis_y is likely truncated by that
      ratio. Flag with a strength rating.

Output: a markdown report grouped by pack + by suspect axis pair,
ranked by confidence. Confidence is "high" when gap is a clean
multiple of row-bytes (strong signal of axis_y truncation).

Run from D:/Subuwu/code/:
    python tools/audit_toml_axis_truncation.py [--pack lf79103p]
                                               [--threshold 2.0]
                                               [--out report.md]

Default: scans all definitions/impreza/*.toml, writes to stdout.
"""
from __future__ import annotations

import argparse
import sys
import tomllib
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

DTYPE_BYTES = {
    "uint8": 1, "int8": 1,
    "uint16_be": 2, "int16_be": 2,
    "uint32_be": 4, "int32_be": 4,
    "float32_be": 4,
}


@dataclass
class AxisDecl:
    id: str
    length: int
    address: int | None = None


@dataclass
class TableDecl:
    id: str
    name: str
    address: int
    dimensions: int
    data_type: str
    axis_x: str | None
    axis_y: str | None
    cols: int
    rows: int

    @property
    def cell_bytes(self) -> int:
        return DTYPE_BYTES.get(self.data_type, 1)

    @property
    def declared_extent(self) -> int:
        return self.cols * self.rows * self.cell_bytes

    @property
    def row_bytes(self) -> int:
        return self.cols * self.cell_bytes


def load_pack(path: Path) -> tuple[dict[str, AxisDecl], list[TableDecl]]:
    with path.open("rb") as fp:
        doc = tomllib.load(fp)
    axes: dict[str, AxisDecl] = {}
    for a in doc.get("axis", []):
        axes[a["id"]] = AxisDecl(
            id=a["id"],
            length=int(a.get("length", 1)),
            address=int(a["address"]) if "address" in a else None,
        )
    tables = []
    for t in doc.get("table", []):
        dim = int(t.get("dimensions", 0))
        cols, rows = 1, 1
        if dim == 1:
            ax = axes.get(t.get("axis_x"))
            cols = ax.length if ax else 1
        elif dim == 2:
            axx = axes.get(t.get("axis_x"))
            axy = axes.get(t.get("axis_y"))
            cols = axx.length if axx else 1
            rows = axy.length if axy else 1
        tables.append(TableDecl(
            id=t["id"],
            name=t.get("name", t["id"]),
            address=int(t["address"]),
            dimensions=dim,
            data_type=t.get("data_type", "uint8"),
            axis_x=t.get("axis_x"),
            axis_y=t.get("axis_y"),
            cols=cols,
            rows=rows,
        ))
    tables.sort(key=lambda t: t.address)
    return axes, tables


def analyse_pack(path: Path, threshold: float) -> list[dict]:
    axes, tables = load_pack(path)
    findings = []
    for i, t in enumerate(tables):
        if t.dimensions == 0:
            continue  # scalars can't be truncated by axis
        # Find next-table gap, skipping zero-extent / scalar entries.
        gap = None
        for j in range(i + 1, len(tables)):
            if tables[j].address > t.address:
                gap = tables[j].address - t.address
                break
        if gap is None or t.declared_extent == 0:
            continue
        ratio = gap / t.declared_extent
        if ratio < threshold:
            continue
        # Is the gap a clean multiple of row_bytes? Strong axis_y truncation signal.
        clean_row_multiple = (
            t.row_bytes > 0
            and gap % t.row_bytes == 0
            and gap // t.row_bytes != t.rows
        )
        # Is the gap a clean multiple of cell_bytes (axis_x truncation)?
        clean_col_multiple = (
            t.cell_bytes > 0
            and gap % t.cell_bytes == 0
            and not clean_row_multiple
        )
        confidence = "high" if clean_row_multiple else (
            "medium" if clean_col_multiple else "low"
        )
        possible_rows = gap // t.row_bytes if t.row_bytes else 0
        findings.append({
            "table_id": t.id,
            "table_name": t.name,
            "address": t.address,
            "dimensions": t.dimensions,
            "data_type": t.data_type,
            "axis_x": t.axis_x,
            "axis_y": t.axis_y,
            "declared_cols": t.cols,
            "declared_rows": t.rows,
            "declared_extent_bytes": t.declared_extent,
            "gap_to_next_bytes": gap,
            "ratio": ratio,
            "confidence": confidence,
            "possible_rows": possible_rows if clean_row_multiple else None,
        })
    return findings


def render_markdown(by_pack: dict[str, list[dict]], threshold: float) -> str:
    out = []
    out.append("# TOML axis-truncation audit\n")
    out.append(f"- Threshold: declared_extent × {threshold} ≤ gap_to_next\n")
    out.append("- Confidence:\n")
    out.append("  - **high**: gap is exact integer multiple of row_bytes → axis_y likely truncated\n")
    out.append("  - **medium**: gap is multiple of cell_bytes but not row_bytes → axis_x may be truncated, or dtype wrong\n")
    out.append("  - **low**: gap is not a clean multiple of either → could be unused padding\n")
    out.append("\n")

    # Group findings across packs by (axis_x, axis_y) pair to surface shared-axis
    # truncation patterns (the wastegate bug pattern: many tables share short axes).
    axis_pair_freq = defaultdict(list)
    for pack, findings in by_pack.items():
        for f in findings:
            if f["confidence"] == "high":
                axis_pair_freq[(f["axis_x"], f["axis_y"])].append((pack, f))

    if axis_pair_freq:
        out.append("## Shared-axis hot-spots (high-confidence findings grouped by axis pair)\n\n")
        out.append("| (axis_x, axis_y) | Packs | Distinct tables | Sample |\n")
        out.append("|---|---|---|---|\n")
        ranked = sorted(axis_pair_freq.items(), key=lambda kv: -len(kv[1]))
        for (ax, ay), instances in ranked[:25]:
            packs = sorted({p for p, _ in instances})
            tids = sorted({f["table_id"] for _, f in instances})
            sample = instances[0][1]
            out.append(
                f"| ({ax!r}, {ay!r}) | {len(packs)} | {len(tids)} | "
                f"`{sample['table_id']}` declared {sample['declared_cols']}×{sample['declared_rows']}, "
                f"gap suggests {sample['possible_rows']} rows |\n"
            )
        out.append("\n")

    # Per-pack detail
    for pack in sorted(by_pack):
        findings = by_pack[pack]
        if not findings:
            continue
        out.append(f"## {pack}\n\n")
        high = [f for f in findings if f["confidence"] == "high"]
        med = [f for f in findings if f["confidence"] == "medium"]
        low = [f for f in findings if f["confidence"] == "low"]
        out.append(f"- {len(findings)} total findings ({len(high)} high, {len(med)} medium, {len(low)} low)\n\n")

        if high:
            out.append("### High confidence (gap is clean row-stride multiple)\n\n")
            out.append("| Table | Address | Declared | Gap | Possible rows | Axis pair |\n")
            out.append("|---|---|---|---|---|---|\n")
            for f in sorted(high, key=lambda x: -x["gap_to_next_bytes"])[:30]:
                out.append(
                    f"| `{f['table_id']}` | 0x{f['address']:05x} | "
                    f"{f['declared_cols']}×{f['declared_rows']} ({f['declared_extent_bytes']} B) | "
                    f"{f['gap_to_next_bytes']} B | **{f['possible_rows']}** | "
                    f"({f['axis_x']!r}, {f['axis_y']!r}) |\n"
                )
            out.append("\n")

        if med:
            out.append("### Medium confidence (clean cell-stride multiple but not row-stride)\n\n")
            out.append("| Table | Address | Declared | Gap | Axis pair |\n")
            out.append("|---|---|---|---|---|\n")
            for f in sorted(med, key=lambda x: -x["gap_to_next_bytes"])[:15]:
                out.append(
                    f"| `{f['table_id']}` | 0x{f['address']:05x} | "
                    f"{f['declared_cols']}×{f['declared_rows']} | "
                    f"{f['gap_to_next_bytes']} B | "
                    f"({f['axis_x']!r}, {f['axis_y']!r}) |\n"
                )
            out.append("\n")

    return "".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", action="append", default=None,
                    help="Specific pack(s) to scan (default: all definitions/impreza/*.toml).")
    ap.add_argument("--threshold", type=float, default=2.0,
                    help="Flag if gap >= threshold × declared_extent (default 2.0).")
    ap.add_argument("--out", type=Path, default=None,
                    help="Write markdown report to file (default: stdout).")
    args = ap.parse_args(argv)

    defs_dir = Path("definitions/impreza")
    if args.pack:
        paths = [defs_dir / f"{p}.toml" for p in args.pack]
    else:
        # Skip the _cobb_f3xx_v2 overlays — they don't declare tables.
        paths = sorted(p for p in defs_dir.glob("*.toml")
                       if "_cobb_f3xx" not in p.name)

    by_pack = {}
    for path in paths:
        if not path.exists():
            print(f"audit: {path} not found, skipping", file=sys.stderr)
            continue
        try:
            findings = analyse_pack(path, args.threshold)
        except Exception as e:
            print(f"audit: {path.name}: {e}", file=sys.stderr)
            continue
        by_pack[path.stem] = findings
        n_high = sum(1 for f in findings if f["confidence"] == "high")
        n_total = len(findings)
        print(f"  {path.stem}: {n_total} findings ({n_high} high)", file=sys.stderr)

    md = render_markdown(by_pack, args.threshold)
    if args.out:
        args.out.write_text(md, encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(md)


if __name__ == "__main__":
    main()
