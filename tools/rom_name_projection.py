#!/usr/bin/env python3
"""Emit review-only function-name projections from exact unique body matches."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

try:
    from tools.rom_function_parity import compare
except ModuleNotFoundError:  # Direct execution: python tools/rom_name_projection.py
    from rom_function_parity import compare


def canonical_address_name(name: str, address: str) -> str:
    suffix = f"_{int(address, 0):x}"
    return name[:-len(suffix)] if name.lower().endswith(suffix) else name


def build_projection(parity: dict, source_cid: str, target_cid: str) -> dict:
    rows = []
    counts = {
        "eligible": 0, "already_named": 0, "address_variant": 0,
        "conflict": 0, "source_unnamed": 0,
    }
    for mapping in parity["unique_identical_body_mappings"]:
        if not mapping["left_named"]:
            status = "source_unnamed"
        elif not mapping["right_named"]:
            status = "eligible"
        elif mapping["left_name"] == mapping["right_name"]:
            status = "already_named"
        elif (
            canonical_address_name(mapping["left_name"], mapping["left_address"])
            == canonical_address_name(mapping["right_name"], mapping["right_address"])
        ):
            status = "address_variant"
        else:
            status = "conflict"
        counts[status] += 1
        rows.append({
            "source_cid": source_cid.upper(),
            "source_address": mapping["left_address"],
            "target_cid": target_cid.upper(),
            "target_address": mapping["right_address"],
            "source_name": mapping["left_name"],
            "target_name": mapping["right_name"],
            "canonical_name": canonical_address_name(
                mapping["left_name"], mapping["left_address"]
            ),
            "size": mapping["size"],
            "body_sha256": mapping["sha256"],
            "status": status,
            "match_method": "unique_full_body_sha256",
        })
    return {
        "schema": "subuwutuner.rom-name-projection.v1",
        "policy": {
            "review_only": True,
            "automatic_application_authorized": False,
            "ambiguous_body_hashes_excluded": True,
        },
        "source_cid": source_cid.upper(),
        "target_cid": target_cid.upper(),
        "counts": counts,
        "projections": rows,
    }


def write_tsv(path: Path, document: dict) -> None:
    fields = [
        "source_cid", "source_address", "target_cid", "target_address",
        "source_name", "target_name", "canonical_name", "size", "body_sha256",
        "status", "match_method",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(document["projections"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_rom", type=Path)
    parser.add_argument("source_index", type=Path)
    parser.add_argument("source_cid")
    parser.add_argument("target_rom", type=Path)
    parser.add_argument("target_index", type=Path)
    parser.add_argument("target_cid")
    parser.add_argument("--source-base", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--target-base", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tsv-out", type=Path)
    args = parser.parse_args()
    parity = compare(
        args.source_rom, args.source_index, args.target_rom, args.target_index,
        args.source_base, args.target_base,
    )
    document = build_projection(parity, args.source_cid, args.target_cid)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    if args.tsv_out:
        write_tsv(args.tsv_out, document)
    print(
        f"{args.source_cid.upper()} -> {args.target_cid.upper()}: "
        f"{document['counts']['eligible']} eligible, "
        f"{document['counts']['conflict']} conflicts"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
