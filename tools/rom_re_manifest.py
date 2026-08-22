#!/usr/bin/env python3
"""Create and verify provenance manifests for offline ROM analysis bundles.

The manifest is deliberately independent of Ghidra.  An analyst records the
inputs and outputs of a run, then ``verify`` detects source substitution,
changed label inputs, or artifact drift before names are reused elsewhere.
It never modifies a ROM or a public definition.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "subuwutuner.rom-re-evidence.v1"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def entropy_file(path: Path) -> float:
    counts: Counter[int] = Counter()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            counts.update(chunk)
            size += len(chunk)
    if not size:
        return 0.0
    return -sum((count / size) * math.log2(count / size)
                for count in counts.values())


def file_record(path: Path, base: Path) -> dict[str, Any]:
    resolved = path.resolve()
    try:
        display = str(resolved.relative_to(base.resolve()))
    except ValueError:
        display = str(resolved)
    return {
        "path": display,
        "size": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def parse_anchor(value: str) -> dict[str, Any]:
    parts = value.split(":", 2)
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("anchor must be NAME:ADDRESS:HEXBYTES")
    name, address_text, bytes_text = parts
    try:
        address = int(address_text, 0)
        bytes.fromhex(bytes_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid anchor {value!r}: {exc}") from exc
    return {"name": name, "address": address, "bytes_hex": bytes_text.lower()}


def create_manifest(args: argparse.Namespace) -> dict[str, Any]:
    base = args.base.resolve()
    source = file_record(args.rom, base)
    source["entropy_bits_per_byte"] = round(entropy_file(args.rom), 6)
    source["plaintext_status"] = args.plaintext_status
    return {
        "schema": SCHEMA,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "path_base": str(base),
        "cid": args.cid.upper(),
        "family": (args.family or args.cid[:4]).upper(),
        "source": source,
        "architecture": {
            "processor": args.processor,
            "endianness": args.endianness,
            "image_base": args.image_base,
        },
        "boot_anchors": args.anchor,
        "public_label_inputs": [file_record(path, base) for path in args.labels],
        "siblings": [
            {**file_record(path, base), "compatibility": "unreviewed"}
            for path in args.sibling
        ],
        "artifacts": [file_record(path, base) for path in args.artifact],
        "policy": {
            "evidence_only": True,
            "definition_edits_authorized": False,
            "name_transfer_requires_byte_or_shape_match": True,
        },
    }


def resolve_record(record: dict[str, Any], base: Path) -> Path:
    path = Path(record["path"])
    return path if path.is_absolute() else base / path


def verify_manifest(document: dict[str, Any], base: Path) -> list[str]:
    errors: list[str] = []
    if document.get("schema") != SCHEMA:
        return [f"unsupported schema: {document.get('schema')!r}"]
    groups = [("source", [document.get("source", {})])]
    groups.extend((name, document.get(name, [])) for name in
                  ("public_label_inputs", "siblings", "artifacts"))
    for group, records in groups:
        for index, record in enumerate(records):
            label = f"{group}[{index}]"
            try:
                path = resolve_record(record, base)
                if not path.is_file():
                    errors.append(f"{label}: missing file: {path}")
                    continue
                actual_size = path.stat().st_size
                if actual_size != record.get("size"):
                    errors.append(
                        f"{label}: size drift: expected {record.get('size')}, got {actual_size}"
                    )
                actual_hash = sha256_file(path)
                if actual_hash != record.get("sha256"):
                    errors.append(
                        f"{label}: sha256 drift: expected {record.get('sha256')}, got {actual_hash}"
                    )
            except (KeyError, OSError, TypeError) as exc:
                errors.append(f"{label}: invalid record: {exc}")
    source = document.get("source", {})
    architecture = document.get("architecture", {})
    try:
        source_path = resolve_record(source, base)
        image_base = int(architecture.get("image_base", 0))
        if source_path.is_file():
            image = source_path.read_bytes()
            for index, anchor in enumerate(document.get("boot_anchors", [])):
                expected = bytes.fromhex(anchor["bytes_hex"])
                offset = int(anchor["address"]) - image_base
                actual = image[offset:offset + len(expected)] if offset >= 0 else b""
                if actual != expected:
                    errors.append(
                        f"boot_anchors[{index}]: {anchor.get('name', 'unnamed')} "
                        f"mismatch at 0x{int(anchor['address']):X}: expected "
                        f"{expected.hex()}, got {actual.hex() or '<out-of-range>'}"
                    )
    except (KeyError, OSError, TypeError, ValueError) as exc:
        errors.append(f"boot anchors: invalid record: {exc}")
    return errors


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="write a new evidence manifest")
    create.add_argument("--rom", type=Path, required=True)
    create.add_argument("--cid", required=True)
    create.add_argument("--family")
    create.add_argument("--processor", required=True)
    create.add_argument("--endianness", choices=("big", "little"), required=True)
    create.add_argument("--image-base", type=lambda value: int(value, 0), default=0)
    create.add_argument("--plaintext-status",
                        choices=("verified", "suspected", "quarantined"),
                        default="suspected")
    create.add_argument("--anchor", action="append", type=parse_anchor, default=[])
    create.add_argument("--labels", action="append", type=Path, default=[])
    create.add_argument("--sibling", action="append", type=Path, default=[])
    create.add_argument("--artifact", action="append", type=Path, default=[])
    create.add_argument("--base", type=Path, default=Path.cwd())
    create.add_argument("--out", type=Path, required=True)
    verify = sub.add_parser("verify", help="verify every pinned file")
    verify.add_argument("manifest", type=Path)
    verify.add_argument("--base", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "create":
        document = create_manifest(args)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.out} ({document['source']['sha256']})")
        return 0
    document = json.loads(args.manifest.read_text(encoding="utf-8"))
    recorded_base = document.get("path_base")
    base = (args.base or (Path(recorded_base) if recorded_base else args.manifest.parent)).resolve()
    errors = verify_manifest(document, base)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"verified {args.manifest}: all pinned files unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
