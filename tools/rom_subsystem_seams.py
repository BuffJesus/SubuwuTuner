#!/usr/bin/env python3
"""Find unnamed functions directly adjacent to named subsystem anchors."""
from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

HEADER_RE = re.compile(r"^====\s+(\S+)\s+@\s+([0-9a-fA-F]+)\s+size=(\d+)\s+xrefs=(\d+)\s+====$")
CALL_RE = re.compile(r"(\S+)@([0-9a-fA-F]+)")
ANCHORS = {
    "diagnostics": re.compile(
        r"diag|dtc|monitor|obd|uds|ssm|(?:^|_)fault(?:_|$)", re.I
    ),
    "security_authorization": re.compile(r"security|seed|key|auth|immobili", re.I),
    "boot_flash": re.compile(r"boot|flash|faci|reprog", re.I),
    "checksum_integrity": re.compile(r"checksum|crc|parity", re.I),
    "calibration_interpolation": re.compile(r"interp|axis|table|lookup|map_", re.I),
    "runtime_io": re.compile(r"sched|timer|interrupt|irq|sensor|signal|io_|can_", re.I),
}


def parse_index(path: Path) -> dict[int, dict]:
    functions = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line or line.startswith("#"):
            continue
        printable_tab = chr(92) + "t"
        columns = line.split(printable_tab) if printable_tab in line else line.split("\t")
        if len(columns) < 5:
            raise ValueError(f"{path}:{number}: malformed index row")
        address = int(columns[0], 16)
        functions[address] = {
            "address": address,
            "size": int(columns[1]),
            "xrefs_to": int(columns[2]),
            "named": columns[3] != "-",
            "name": columns[4],
        }
    return functions


def parse_decomp(paths: list[Path]) -> set[tuple[int, int]]:
    edges = set()
    caller = None
    for path in paths:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            header = HEADER_RE.match(line)
            if header:
                caller = int(header.group(2), 16)
                continue
            if caller is not None and line.startswith("calls:"):
                for match in CALL_RE.finditer(line[6:]):
                    edges.add((caller, int(match.group(2), 16)))
    return edges


def anchor_subsystems(name: str) -> list[str]:
    return [subsystem for subsystem, pattern in ANCHORS.items() if pattern.search(name)]


def inventory(index: Path, decomp_paths: list[Path], cid: str) -> dict:
    functions = parse_index(index)
    edges = parse_decomp(decomp_paths)
    evidence: dict[tuple[int, str], list[dict]] = defaultdict(list)
    for caller, callee in edges:
        caller_fn, callee_fn = functions.get(caller), functions.get(callee)
        if not caller_fn or not callee_fn:
            continue
        if not caller_fn["named"] and callee_fn["named"]:
            for subsystem in anchor_subsystems(callee_fn["name"]):
                evidence[(caller, subsystem)].append({
                    "direction": "calls_named_anchor",
                    "anchor_address": f"0x{callee:X}",
                    "anchor_name": callee_fn["name"],
                })
        if caller_fn["named"] and not callee_fn["named"]:
            for subsystem in anchor_subsystems(caller_fn["name"]):
                evidence[(callee, subsystem)].append({
                    "direction": "called_by_named_anchor",
                    "anchor_address": f"0x{caller:X}",
                    "anchor_name": caller_fn["name"],
                })
    candidates = []
    for (address, subsystem), anchors in sorted(evidence.items()):
        function = functions[address]
        unique_anchors = {
            (row["direction"], row["anchor_address"], row["anchor_name"])
            for row in anchors
        }
        rows = [
            {"direction": direction, "anchor_address": anchor_address, "anchor_name": name}
            for direction, anchor_address, name in sorted(unique_anchors)
        ]
        candidates.append({
            "address": f"0x{address:X}",
            "current_name": function["name"],
            "size": function["size"],
            "subsystem": subsystem,
            "confidence": "multi_anchor" if len(rows) >= 2 else "single_anchor",
            "evidence": rows,
        })
    counts = defaultdict(int)
    for row in candidates:
        counts[row["subsystem"]] += 1
    return {
        "schema": "subuwutuner.rom-subsystem-seams.v1",
        "cid": cid.upper(),
        "policy": {
            "evidence_only": True,
            "renaming_authorized": False,
            "direct_call_edges_only": True,
        },
        "function_count": len(functions),
        "call_edge_count": len(edges),
        "candidate_count": len(candidates),
        "candidate_counts_by_subsystem": dict(sorted(counts.items())),
        "candidates": candidates,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cid")
    parser.add_argument("index", type=Path)
    parser.add_argument("decompile_dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.decompile_dir.glob("decomp_*.txt"))
    if not paths:
        parser.error(f"no decomp_*.txt files in {args.decompile_dir}")
    try:
        document = inventory(args.index, paths, args.cid)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(
        f"{document['cid']}: {document['call_edge_count']} call edges, "
        f"{document['candidate_count']} unnamed seam candidates"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
