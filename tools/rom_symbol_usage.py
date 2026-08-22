#!/usr/bin/env python3
"""Index decompiler function usage of selected RAM/data symbols."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

HEADER_RE = re.compile(r"^==== (\S+) @ ([0-9A-Fa-f]+) size=(\d+) xrefs=(\d+) ====$")


def index_usage(paths: list[Path], symbols: list[str]) -> dict:
    wanted = {symbol.lower(): symbol for symbol in symbols}
    combined = re.compile(
        r"(?<![A-Za-z0-9_])(?:" + "|".join(
            sorted((re.escape(symbol) for symbol in wanted), key=len, reverse=True)
        ) + r")(?![A-Za-z0-9_])"
    ) if wanted else None
    usage = {symbol: [] for symbol in symbols}
    for path in sorted(paths):
        current = None
        hits: dict[str, list[int]] = {}
        for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            header = HEADER_RE.match(line)
            if header:
                if current:
                    for symbol, lines in hits.items():
                        usage[wanted[symbol]].append({**current, "reference_count": len(lines),
                                                       "lines": lines})
                current = {"function": header.group(1),
                           "address": f"0x{int(header.group(2), 16):X}",
                           "file": path.name}
                hits = {}
                continue
            if not current:
                continue
            lowered = line.lower()
            for symbol in {match.group(0) for match in combined.finditer(lowered)}:
                hits.setdefault(symbol, []).append(line_number)
        if current:
            for symbol, lines in hits.items():
                usage[wanted[symbol]].append({**current, "reference_count": len(lines),
                                               "lines": lines})
    return {"schema": "subuwutuner.rom-symbol-usage.v1", "symbols": usage}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("decompile_dir", type=Path)
    parser.add_argument("symbols", nargs="+")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    paths = list(args.decompile_dir.glob("decomp_*.txt"))
    if not paths:
        parser.error(f"no decomp_*.txt files in {args.decompile_dir}")
    result = index_usage(paths, args.symbols)
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
