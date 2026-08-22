#!/usr/bin/env python3
"""Compare a canonical ROM with same-CID siblings and emit evidence reports."""

from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
from typing import Any

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def changed_runs(left: bytes, right: bytes) -> list[tuple[int, int]]:
    runs, start = [], None
    for offset in range(max(len(left), len(right))):
        different = offset >= len(left) or offset >= len(right) or left[offset] != right[offset]
        if different and start is None: start = offset
        elif not different and start is not None:
            runs.append((start, offset)); start = None
    if start is not None: runs.append((start, max(len(left), len(right))))
    return runs

def compare(canonical: Path, siblings: list[Path]) -> dict[str, Any]:
    base, rows = canonical.read_bytes(), []
    for path in siblings:
        data = path.read_bytes(); runs = changed_runs(base, data)
        rows.append({"path": str(path.resolve()), "size": len(data), "sha256": digest(data),
                     "identical": data == base, "changed_bytes": sum(b-a for a,b in runs),
                     "changed_runs": len(runs), "first_change": runs[0][0] if runs else None,
                     "last_change_exclusive": runs[-1][1] if runs else None,
                     "runs": [{"start": a, "end": b, "length": b-a} for a,b in runs]})
    return {"schema": "subuwutuner.rom-sibling-fingerprint.v1",
            "canonical": {"path": str(canonical.resolve()), "size": len(base), "sha256": digest(base)},
            "siblings": rows}

def markdown(doc: dict[str, Any]) -> str:
    rows = doc["siblings"]
    unique = len({row["sha256"] for row in rows})
    lines = ["# ROM sibling fingerprint", "", "> Byte-level evidence only; similar spans do not authorize name transfer.", "",
             f"- Canonical: `{doc['canonical']['path']}`", f"- SHA-256: `{doc['canonical']['sha256']}`",
             f"- Siblings: **{len(rows)}**", f"- Byte-identical: **{sum(r['identical'] for r in rows)}**", "",
             f"- Unique non-canonical variants: **{unique}**", "",
             "| File | SHA-256 | Changed bytes | Runs | Changed span |", "|---|---|---:|---:|---|"]
    for row in rows:
        span = "identical" if row["identical"] else f"0x{row['first_change']:X}..0x{row['last_change_exclusive']:X}"
        lines.append(f"| `{Path(row['path']).name}` | `{row['sha256'][:16]}…` | {row['changed_bytes']} | {row['changed_runs']} | {span} |")
    lines += ["", "## Changed runs", ""]
    for row in rows:
        if row["identical"]: continue
        lines += [f"### {Path(row['path']).name}", ""]
        lines += [f"- `0x{run['start']:X}..0x{run['end']:X}` ({run['length']} bytes)" for run in row["runs"][:100]]
        if len(row["runs"]) > 100: lines.append(f"- … {len(row['runs'])-100} additional runs (see JSON)")
        lines.append("")
    return "\n".join(lines)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("canonical", type=Path); parser.add_argument("siblings", nargs="+", type=Path)
    parser.add_argument("--out", type=Path); parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(); doc = compare(args.canonical, args.siblings); text = markdown(doc)
    if args.out: args.out.write_text(text+"\n", encoding="utf-8")
    else: print(text)
    if args.json_out: args.json_out.write_text(json.dumps(doc, indent=2)+"\n", encoding="utf-8")
    return 0

if __name__ == "__main__": raise SystemExit(main())
