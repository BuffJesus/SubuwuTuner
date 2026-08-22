#!/usr/bin/env python3
"""Map RAM targets to ROM pointer literals and decompiler function references."""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

try:
    from rom_symbol_usage import index_usage
except ModuleNotFoundError:  # Imported as tools.rom_pointer_aliases in tests.
    from tools.rom_symbol_usage import index_usage


def audit(blob: bytes, paths: list[Path], targets: list[int], image_base: int = 0) -> dict:
    target_locations = {}
    symbol_locations = {}
    for target in targets:
        needle = struct.pack(">I", target)
        locations = [image_base + offset for offset in range(len(blob) - 3)
                     if blob[offset:offset + 4] == needle]
        target_locations[target] = locations
        for location in locations:
            suffix = f"{location:08x}"
            symbol_locations[location] = [f"PTR_DAT_{suffix}", f"PTR_PTR_{suffix}",
                                          f"DAT_{suffix}"]
    symbols = [symbol for group in symbol_locations.values() for symbol in group]
    usage = index_usage(paths, symbols)["symbols"] if symbols else {}
    output = {}
    for target in targets:
        locations = target_locations[target]
        aliases = []
        for location in locations:
            references = []
            for symbol in symbol_locations[location]:
                for row in usage[symbol]:
                    references.append({"alias": symbol, **row})
            aliases.append({"literal_address": f"0x{location:X}",
                            "references": references})
        output[f"0x{target:X}"] = {"literal_count": len(locations), "aliases": aliases}
    return {"schema": "subuwutuner.rom-pointer-aliases.v1", "targets": output}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("decompile_dir", type=Path)
    parser.add_argument("targets", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--image-base", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    paths = list(args.decompile_dir.glob("decomp_*.txt"))
    if not paths:
        parser.error(f"no decomp_*.txt files in {args.decompile_dir}")
    try:
        result = audit(args.rom.read_bytes(), paths, args.targets, args.image_base)
    except OSError as exc:
        parser.error(str(exc))
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
