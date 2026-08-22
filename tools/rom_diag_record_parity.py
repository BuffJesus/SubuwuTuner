#!/usr/bin/env python3
"""Cross-check inferred diagnostic record identities through exact body mappings."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def compare_records(left: dict, right: dict, parity: dict) -> dict:
    left_by_address = {row["address"].lower(): row for row in left["candidates"]}
    right_by_address = {row["address"].lower(): row for row in right["candidates"]}
    stable, disagreements = [], []
    for mapping in parity["unique_identical_body_mappings"]:
        left_row = left_by_address.get(mapping["left_address"].lower())
        right_row = right_by_address.get(mapping["right_address"].lower())
        if not left_row or not right_row:
            continue
        row = {
            "left_address": mapping["left_address"],
            "right_address": mapping["right_address"],
            "body_sha256": mapping["sha256"],
            "size": mapping["size"],
            "left_proposed_name": left_row["proposed_name"],
            "right_proposed_name": right_row["proposed_name"],
        }
        if left_row["proposed_name"] == right_row["proposed_name"]:
            stable.append(row)
        else:
            disagreements.append(row)
    return {
        "schema": "subuwutuner.rom-diag-record-parity.v1",
        "left_cid": left["cid"],
        "right_cid": right["cid"],
        "stable_candidate_count": len(stable),
        "disagreement_count": len(disagreements),
        "stable_candidates": stable,
        "disagreements": disagreements,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("parity", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        document = compare_records(
            json.loads(args.left.read_text(encoding="utf-8")),
            json.loads(args.right.read_text(encoding="utf-8")),
            json.loads(args.parity.read_text(encoding="utf-8")),
        )
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(
        f"{document['left_cid']} -> {document['right_cid']}: "
        f"{document['stable_candidate_count']} stable, "
        f"{document['disagreement_count']} disagreements"
    )
    return 1 if document["disagreement_count"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
