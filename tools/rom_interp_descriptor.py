#!/usr/bin/env python3
"""Decode an LF9 big-endian 2D interpolation descriptor from a ROM image."""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def decode(blob: bytes, address: int, image_base: int = 0,
           x_type: str = "u32") -> dict:
    offset = address - image_base
    if offset < 0 or offset + 14 > len(blob):
        raise ValueError(f"descriptor outside ROM mapping at 0x{address:X}")
    data_address, x_address, y_address = struct.unpack_from(">III", blob, offset)
    x_count, y_count = struct.unpack_from(">BB", blob, offset + 12)
    if not x_count or not y_count:
        raise ValueError("descriptor axis counts must be nonzero")

    def unpack_at(fmt: str, target: int, count: int) -> list[int]:
        size = struct.calcsize(fmt) * count
        target_offset = target - image_base
        if target_offset < 0 or target_offset + size > len(blob):
            raise ValueError(f"descriptor target outside ROM mapping at 0x{target:X}")
        return list(struct.unpack_from(">" + fmt * count, blob, target_offset))

    if x_type not in {"u16", "u32"}:
        raise ValueError(f"unsupported X axis type: {x_type}")
    x_axis = unpack_at("I" if x_type == "u32" else "H", x_address, x_count)
    y_axis = unpack_at("H", y_address, y_count)
    flat = unpack_at("h", data_address, x_count * y_count)
    grid = [flat[row:row + y_count] for row in range(0, len(flat), y_count)]
    return {
        "descriptor_address": f"0x{address:X}",
        "data_address": f"0x{data_address:X}",
        "x_address": f"0x{x_address:X}",
        "y_address": f"0x{y_address:X}",
        "x_count": x_count,
        "y_count": y_count,
        f"x_axis_{x_type}": x_axis,
        "y_axis_u16": y_axis,
        "grid_s16": grid,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("address", type=lambda value: int(value, 0))
    parser.add_argument("--image-base", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--x-type", choices=("u16", "u32"), default="u32")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        result = decode(args.rom.read_bytes(), args.address, args.image_base, args.x_type)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
