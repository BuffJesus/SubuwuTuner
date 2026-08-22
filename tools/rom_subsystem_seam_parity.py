#!/usr/bin/env python3
"""Cross-check subsystem seam evidence through exact function-body mappings."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def compare_seams(left: dict, right: dict, parity: dict) -> dict:
    left_rows = {
        (row["address"].lower(), row["subsystem"]): row
        for row in left["candidates"]
    }
    right_rows = {
        (row["address"].lower(), row["subsystem"]): row
        for row in right["candidates"]
    }
    stable, subsystem_disagreements = [], []
    for mapping in parity["unique_identical_body_mappings"]:
        left_address = mapping["left_address"].lower()
        right_address = mapping["right_address"].lower()
        left_subsystems = {subsystem for address, subsystem in left_rows if address == left_address}
        right_subsystems = {subsystem for address, subsystem in right_rows if address == right_address}
        for subsystem in sorted(left_subsystems & right_subsystems):
            stable.append({
                "left_address": mapping["left_address"],
                "right_address": mapping["right_address"],
                "size": mapping["size"],
                "body_sha256": mapping["sha256"],
                "subsystem": subsystem,
                "left_confidence": left_rows[(left_address, subsystem)]["confidence"],
                "right_confidence": right_rows[(right_address, subsystem)]["confidence"],
            })
        if left_subsystems and right_subsystems and not (left_subsystems & right_subsystems):
            subsystem_disagreements.append({
                "left_address": mapping["left_address"],
                "right_address": mapping["right_address"],
                "body_sha256": mapping["sha256"],
                "left_subsystems": sorted(left_subsystems),
                "right_subsystems": sorted(right_subsystems),
            })
    by_subsystem = {}
    for row in stable:
        by_subsystem[row["subsystem"]] = by_subsystem.get(row["subsystem"], 0) + 1
    return {
        "schema": "subuwutuner.rom-subsystem-seam-parity.v1",
        "left_cid": left["cid"],
        "right_cid": right["cid"],
        "policy": {
            "evidence_only": True,
            "renaming_authorized": False,
            "requires_unique_full_body_match": True,
        },
        "stable_association_count": len(stable),
        "stable_counts_by_subsystem": dict(sorted(by_subsystem.items())),
        "subsystem_disagreement_count": len(subsystem_disagreements),
        "stable_associations": stable,
        "subsystem_disagreements": subsystem_disagreements,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left_seams", type=Path)
    parser.add_argument("right_seams", type=Path)
    parser.add_argument("parity", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        left = json.loads(args.left_seams.read_text(encoding="utf-8"))
        right = json.loads(args.right_seams.read_text(encoding="utf-8"))
        parity = json.loads(args.parity.read_text(encoding="utf-8"))
        document = compare_seams(left, right, parity)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(
        f"{document['left_cid']} -> {document['right_cid']}: "
        f"{document['stable_association_count']} stable seams, "
        f"{document['subsystem_disagreement_count']} disagreements"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
