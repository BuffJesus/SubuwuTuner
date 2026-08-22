#!/usr/bin/env python3
"""Verify semantic function candidates against exact ROM body hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def parse_rom(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--rom must be CID=PATH")
    cid, path = value.split("=", 1)
    if not cid or not path:
        raise argparse.ArgumentTypeError("--rom must be CID=PATH")
    return cid.upper(), Path(path)


def verify(document: dict, roms: dict[str, Path], image_base: int) -> list[str]:
    errors = []
    if document.get("schema") != "subuwutuner.rom-semantic-evidence.v1":
        return [f"unsupported schema: {document.get('schema')!r}"]
    blobs = {}
    for cid, path in roms.items():
        try:
            blobs[cid] = path.read_bytes()
        except OSError as exc:
            errors.append(f"{cid}: cannot read ROM: {exc}")
    for candidate in document.get("candidates", []):
        name = candidate.get("proposed_name", "<unnamed>")
        try:
            size = int(candidate["size"])
            expected = candidate["body_sha256"]
            for location in candidate["locations"]:
                cid = str(location["cid"]).upper()
                if cid not in blobs:
                    errors.append(f"{name}: missing --rom for {cid}")
                    continue
                address = int(str(location["address"]), 0)
                offset = address - image_base
                body = blobs[cid][offset:offset + size] if offset >= 0 else b""
                if len(body) != size:
                    errors.append(f"{name}/{cid}: body outside ROM mapping at 0x{address:X}")
                    continue
                actual = hashlib.sha256(body).hexdigest()
                if actual != expected:
                    errors.append(
                        f"{name}/{cid}: body hash mismatch: expected {expected}, got {actual}"
                    )
        except (KeyError, TypeError, ValueError) as exc:
            errors.append(f"{name}: invalid candidate: {exc}")
    return errors


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
    roms = dict(args.rom)
    errors = verify(document, roms, args.image_base)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"verified {len(document.get('candidates', []))} semantic candidates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
