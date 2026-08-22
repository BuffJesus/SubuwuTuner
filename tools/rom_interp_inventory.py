#!/usr/bin/env python3
"""Inventory LF9 U32/U16-axis 2D interpolation descriptors in a ROM."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

try:
    from rom_interp_descriptor import decode
except ModuleNotFoundError:  # Imported as tools.rom_interp_inventory in tests.
    from tools.rom_interp_descriptor import decode


def _strictly_increasing(values: list[int]) -> bool:
    return all(left < right for left, right in zip(values, values[1:]))


def scan(blob: bytes, image_base: int = 0, max_axis: int = 64,
         x_type: str = "u32") -> list[dict]:
    if x_type not in {"u16", "u32"}:
        raise ValueError(f"unsupported X axis type: {x_type}")
    results = []
    end_address = image_base + len(blob)
    for offset in range(0, len(blob) - 13, 4):
        data_address, x_address, y_address = struct.unpack_from(">III", blob, offset)
        x_count, y_count = struct.unpack_from(">BB", blob, offset + 12)
        if not (2 <= x_count <= max_axis and 2 <= y_count <= max_axis):
            continue
        if not all(image_base <= value < end_address for value in
                   (data_address, x_address, y_address)):
            continue
        x_alignment_mask = 3 if x_type == "u32" else 1
        if data_address & 1 or x_address & x_alignment_mask or y_address & 1:
            continue
        x_width = 4 if x_type == "u32" else 2
        ranges = sorted((
            (data_address, data_address + x_count * y_count * 2),
            (x_address, x_address + x_count * x_width),
            (y_address, y_address + y_count * 2),
        ))
        if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
            continue
        try:
            item = decode(blob, image_base + offset, image_base, x_type)
        except ValueError:
            continue
        x_key = f"x_axis_{x_type}"
        if not (_strictly_increasing(item[x_key]) and
                _strictly_increasing(item["y_axis_u16"])):
            continue
        canonical = json.dumps({
            x_key: item[x_key],
            "y_axis_u16": item["y_axis_u16"],
            "grid_s16": item["grid_s16"],
        }, sort_keys=True, separators=(",", ":")).encode()
        item["content_sha256"] = hashlib.sha256(canonical).hexdigest()
        flat = [value for row in item["grid_s16"] for value in row]
        item["grid_min"] = min(flat)
        item["grid_max"] = max(flat)
        item["grid_unique_count"] = len(set(flat))
        needle = struct.pack(">I", image_base + offset)
        item["pointer_references"] = [
            f"0x{image_base + position:X}"
            for position in range(0, len(blob) - 3)
            if blob[position:position + 4] == needle
        ]
        results.append(item)
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--image-base", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--max-axis", type=int, default=64)
    parser.add_argument("--x-type", choices=("u16", "u32"), default="u32")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        descriptors = scan(args.rom.read_bytes(), args.image_base, args.max_axis,
                           args.x_type)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    document = {"schema": "subuwutuner.rom-interp-inventory.v1",
                "x_axis_type": args.x_type,
                "descriptor_count": len(descriptors), "descriptors": descriptors}
    rendered = json.dumps(document, indent=2) + "\n"
    if args.out:
        args.out.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
