#!/usr/bin/env python3
"""Audit a Ghidra function index against the exact ROM bytes it models."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

HEADER_RE = re.compile(
    r"^# program=(?P<program>\S+)\s+lang=(?P<language>\S+)\s+"
    r"base=(?P<base>[0-9a-fA-F]+)\s+size=(?P<size>\d+)$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_index(path: Path) -> tuple[dict, list[dict]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ValueError("empty function index")
    match = HEADER_RE.match(lines[0])
    if not match:
        raise ValueError("unrecognized function-index header")
    header = match.groupdict()
    functions = []
    for number, line in enumerate(lines[2:], start=3):
        if not line or line.startswith("#"):
            continue
        # Existing Ghidra exports use the two printable characters "\\t";
        # accept real tabs as well for hand-authored and future indexes.
        columns = line.split("\\t") if "\\t" in line else line.split("\t")
        if len(columns) < 5:
            raise ValueError(f"line {number}: expected five tab-separated columns")
        try:
            functions.append({
                "address": int(columns[0], 16),
                "size": int(columns[1]),
                "xrefs_to": int(columns[2]),
                "named": columns[3] != "-",
                "name": columns[4],
            })
        except ValueError as exc:
            raise ValueError(f"line {number}: invalid numeric field: {exc}") from exc
    header["base"] = int(header["base"], 16)
    header["modeled_memory_bytes"] = int(header.pop("size"))
    return header, functions


def audit(rom: Path, index: Path, image_base: int) -> dict:
    header, functions = parse_index(index)
    source_size = rom.stat().st_size
    rom_end = image_base + source_size
    in_rom = [
        function for function in functions
        if image_base <= function["address"]
        and function["address"] + function["size"] <= rom_end
    ]
    outside = [function for function in functions if function not in in_rom]
    named = [function for function in functions if function["named"]]
    return {
        "schema": "subuwutuner.rom-decompile-baseline.v1",
        "source": {
            "path": str(rom.resolve()),
            "size": source_size,
            "sha256": sha256(rom),
            "image_base": image_base,
            "image_end_exclusive": rom_end,
        },
        "index": {
            "path": str(index.resolve()),
            **header,
            "function_count": len(functions),
            "named_function_count": len(named),
            "unnamed_function_count": len(functions) - len(named),
            "functions_within_source_mapping": len(in_rom),
            "functions_outside_source_mapping": len(outside),
            "outside_source_addresses": [
                f"0x{function['address']:X}" for function in outside
            ],
        },
        "interpretation": {
            "modeled_memory_is_source_size": (
                header["modeled_memory_bytes"] == source_size
            ),
            "modeled_memory_note": (
                "Ghidra modeled-memory size includes added RAM/register blocks; "
                "source.size and source.sha256 identify the imported ROM bytes."
            ),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("index", type=Path)
    parser.add_argument("--image-base", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        result = audit(args.rom, args.index, args.image_base)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    text = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
