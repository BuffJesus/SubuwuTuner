#!/usr/bin/env python3
"""Recover diagnostic maturation-record indices from decompiled structure offsets."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

HEADER_RE = re.compile(r"^====\s+(\S+)\s+@\s+([0-9a-fA-F]+)\s+size=(\d+)\s+xrefs=(\d+)\s+====$")
FLAG_RE = re.compile(r"\[(0x[0-9a-fA-F]+|\d+)\]\s*&\s*1\)\s*!=\s*0")
KNOWN_RE = re.compile(r"^diag_monitor_mature_rec([0-9a-fA-F]{2})$")
BASE_RE = re.compile(r"puVar1\s*=\s*(PTR_[A-Za-z0-9_]+)")


def parse_functions(paths: list[Path]) -> list[dict]:
    functions, current = [], None
    for path in paths:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = HEADER_RE.match(line)
            if match:
                if current:
                    functions.append(current)
                current = {
                    "name": match.group(1),
                    "address": int(match.group(2), 16),
                    "size": int(match.group(3)),
                    "lines": [],
                }
            elif current:
                current["lines"].append(line)
        if current:
            functions.append(current)
            current = None
    return functions


def recover(paths: list[Path], cid: str) -> dict:
    candidates, known, inconsistencies, recovered = [], [], [], []
    for function in parse_functions(paths):
        body = "\n".join(function["lines"])
        offsets = [int(match.group(1), 0) for match in FLAG_RE.finditer(body)]
        if not offsets:
            continue
        # Maturation records are 12 bytes and use the status flag at +2.
        record_offsets = {offset - 2 for offset in offsets if offset >= 2 and (offset - 2) % 12 == 0}
        if len(record_offsets) != 1 or "= puVar1" not in body or "| 4" not in body:
            continue
        record_index = record_offsets.pop() // 12
        base_match = BASE_RE.search(body)
        if not base_match:
            continue
        recovered.append((function, record_index, base_match.group(1)))
    bank_order = {
        base: index
        for index, base in enumerate(dict.fromkeys(
            base for _, _, base in sorted(recovered, key=lambda row: row[0]["address"])
        ))
    }
    for function, record_index, base in recovered:
        bank = bank_order[base]
        proposed = (
            f"diag_monitor_mature_rec{record_index:02X}"
            if bank == 0
            else f"diag_monitor_mature_bank{bank:02X}_rec{record_index:02X}"
        )
        existing = KNOWN_RE.match(function["name"])
        row = {
            "address": f"0x{function['address']:X}",
            "size": function["size"],
            "record_index": record_index,
            "record_offset": record_index * 12,
            "record_bank": bank,
            "record_bank_pointer": base,
            "current_name": function["name"],
            "proposed_name": proposed,
            "match_method": "status_offset_plus_2_stride_12_and_maturation_bit_4",
        }
        if existing:
            row["status"] = "known_consistent" if int(existing.group(1), 16) == record_index else "known_conflict"
            known.append(row)
            if row["status"] == "known_conflict":
                inconsistencies.append(row)
        elif function["name"].startswith("FUN_"):
            row["status"] = "review_candidate"
            candidates.append(row)
    return {
        "schema": "subuwutuner.rom-diag-record-candidates.v1",
        "cid": cid.upper(),
        "policy": {"review_only": True, "automatic_application_authorized": False},
        "known_validation_count": len(known),
        "candidate_count": len(candidates),
        "inconsistency_count": len(inconsistencies),
        "known": known,
        "candidates": candidates,
        "inconsistencies": inconsistencies,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cid")
    parser.add_argument("decompile_dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.decompile_dir.glob("decomp_*.txt"))
    if not paths:
        parser.error(f"no decomp_*.txt files in {args.decompile_dir}")
    document = recover(paths, args.cid)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(
        f"{document['cid']}: {document['known_validation_count']} known validated, "
        f"{document['candidate_count']} candidates, "
        f"{document['inconsistency_count']} inconsistencies"
    )
    return 1 if document["inconsistency_count"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
