#!/usr/bin/env python3
"""Verify big-endian pointer-slot evidence against exact ROM images."""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def verify(document: dict, roms: dict[str, Path], image_base: int) -> list[str]:
    if document.get("schema") != "subuwutuner.rom-pointer-evidence.v1":
        return [f"unsupported schema: {document.get('schema')!r}"]
    errors = []
    blobs = {}
    for cid, path in roms.items():
        try:
            blobs[cid.upper()] = path.read_bytes()
        except OSError as exc:
            errors.append(f"{cid}: cannot read ROM: {exc}")
    for record in document.get("records", []):
        cid = str(record.get("cid", "")).upper()
        label = record.get("label", "<unlabeled>")
        if cid not in blobs:
            errors.append(f"{label}: missing --rom for {cid}")
            continue
        blob = blobs[cid]
        for pointer in record.get("pointers", []):
            try:
                address = int(str(pointer["literal_address"]), 0)
                expected = int(str(pointer["target"]), 0)
                offset = address - image_base
                if offset < 0 or offset + 4 > len(blob):
                    errors.append(f"{label}/{cid}: pointer outside ROM at 0x{address:X}")
                    continue
                actual = struct.unpack_from(">I", blob, offset)[0]
                if actual != expected:
                    errors.append(f"{label}/{cid}: pointer 0x{address:X} expected "
                                  f"0x{expected:X}, got 0x{actual:X}")
            except (KeyError, TypeError, ValueError) as exc:
                errors.append(f"{label}/{cid}: invalid pointer: {exc}")
    return errors


def parse_rom(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--rom must be CID=PATH")
    cid, path = value.split("=", 1)
    return cid.upper(), Path(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--rom", action="append", type=parse_rom, default=[])
    parser.add_argument("--image-base", type=lambda value: int(value, 0), default=0)
    args = parser.parse_args()
    try:
        document = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    errors = verify(document, dict(args.rom), args.image_base)
    if errors:
        print("\n".join(errors))
        return 1
    print(f"verified {len(document.get('records', []))} pointer records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
