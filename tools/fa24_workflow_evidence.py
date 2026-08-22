#!/usr/bin/env python3
"""Audit VA FA24-swap workflow declarations against plaintext ROM bytes.

This is an offline candidate report. It validates the five table surfaces used
by the current modal using known stock byte patterns and bounded byte-shape
checks. It never edits definitions or authorizes a flash.
"""

from __future__ import annotations

import argparse
import json
import tomllib
from pathlib import Path
from typing import Any


WORKFLOW_TABLES = (
    "engine_displacement",
    "fuel_timing_hpfp_phase_transfer_curve",
    "avcs_intake_barometric_multiplier_low_intake_cam_target_tgv_closed",
    "avcs_intake_barometric_multiplier_high_intake_cam_target_tgv_closed",
    "fuel_injectors_pulse_injector_mult_table",
)

PHASE_STOCK = bytes.fromhex("4d4d4d4d4d4d3c3c372c2c2c21212121")
INJECTOR_STOCK = bytes.fromhex("010000c6008c00630051004600390032")


def load_pack(path: Path, definitions: dict[str, tuple[Path, dict[str, Any]]]) -> dict[str, Any]:
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    parent_id = data.get("pack", {}).get("extends")
    tables: dict[str, dict[str, Any]] = {}
    workflows: dict[str, dict[str, Any]] = {}
    inherited_from: str | None = None
    if parent_id and parent_id in definitions:
        parent_path, parent = definitions[parent_id]
        parent_effective = load_pack(parent_path, definitions)
        tables.update(parent_effective["tables"])
        workflows.update(parent_effective["workflows"])
        inherited_from = parent_id
    for table in data.get("table", []):
        if table.get("id"):
            tables[table["id"]] = table
    for workflow in data.get("workflow", []):
        if workflow.get("id"):
            workflows[workflow["id"]] = workflow
    return {
        "data": data,
        "tables": tables,
        "workflows": workflows,
        "inherited_from": inherited_from,
    }


def find_rom(rom_root: Path, cid: str) -> Path | None:
    matches = sorted(rom_root.glob(f"*_{cid.upper()}/rom.bin"))
    return matches[0] if matches else None


def validate_table(table: dict[str, Any], rom: bytes | None) -> dict[str, Any]:
    address = table.get("address")
    result: dict[str, Any] = {
        "address": address,
        "dimensions": table.get("dimensions"),
        "data_type": table.get("data_type"),
        "raw_offset": address - 0x6000 if isinstance(address, int) else None,
        "check": "missing_rom",
    }
    if rom is None or not isinstance(address, int):
        return result
    offset = address - 0x6000
    result["raw_offset"] = offset
    if offset < 0 or offset >= len(rom):
        result["check"] = "out_of_range"
        return result

    if table["id"] == "engine_displacement":
        result["observed_hex"] = rom[offset : offset + 2].hex()
        result["check"] = "stock_2_0L" if rom[offset : offset + 2] == bytes.fromhex("3e80") else "different"
    elif table["id"] == "fuel_timing_hpfp_base_offset":
        result["observed_hex"] = rom[offset : offset + 2].hex()
        result["check"] = "stock_80deg" if rom[offset : offset + 2] == bytes.fromhex("0320") else "different"
    elif table["id"] == "fuel_timing_hpfp_phase_transfer_curve":
        observed = rom[offset : offset + len(PHASE_STOCK)]
        result["observed_hex"] = observed.hex()
        result["check"] = "stock_pattern" if observed == PHASE_STOCK else "different"
    elif table["id"] == "fuel_injectors_pulse_injector_mult_table":
        observed = rom[offset : offset + len(INJECTOR_STOCK)]
        result["observed_hex"] = observed.hex()
        result["check"] = "stock_pattern" if observed == INJECTOR_STOCK else "different"
    else:
        length = 320 if table.get("dimensions") == 2 else 2
        observed = rom[offset : offset + length]
        result["observed_bytes"] = len(observed)
        result["non_ff_bytes"] = sum(byte != 0xFF for byte in observed)
        result["check"] = "bounded_nonblank" if len(observed) == length and any(observed) else "blank_or_short"
    return result


def build_result(path: Path, definitions: dict[str, tuple[Path, dict[str, Any]]], rom_root: Path) -> dict[str, Any]:
    cid = path.stem
    effective = load_pack(path, definitions)
    workflow = effective["workflows"].get("fa24_swap")
    rom_path = find_rom(rom_root, cid)
    rom = rom_path.read_bytes() if rom_path else None
    tables = []
    for table_id in WORKFLOW_TABLES:
        table = effective["tables"].get(table_id)
        validation = validate_table({**table, "id": table_id}, rom) if table else {"check": "missing_table"}
        tables.append({"id": table_id, "present": table is not None, **validation})
    declared = list(workflow.get("required_tables", [])) if workflow else []
    declared_set = set(declared)
    implementation_set = set(WORKFLOW_TABLES)
    all_present = all(table["present"] for table in tables)
    checks = {table["id"]: table["check"] for table in tables}
    byte_ready = bool(rom_path and all(value in {"stock_2_0L", "stock_80deg", "stock_pattern", "bounded_nonblank"} for value in checks.values()))
    if workflow and declared_set != implementation_set:
        status = "declared_set_mismatch"
    elif not all_present:
        status = "missing_workflow_table"
    elif not rom_path:
        status = "no_plaintext_fixture"
    elif not byte_ready:
        status = "byte_check_review"
    elif workflow:
        status = "workflow_declared_and_byte_checked"
    else:
        status = "candidate_byte_checked"
    return {
        "cid": cid,
        "definition": str(path),
        "inherited_from": effective["inherited_from"],
        "workflow_declared": workflow is not None,
        "declared_required_tables": declared,
        "implementation_required_tables": list(WORKFLOW_TABLES),
        "rom": str(rom_path) if rom_path else None,
        "status": status,
        "tables": tables,
    }


def render(results: list[dict[str, Any]], definitions_root: Path, rom_root: Path) -> str:
    lines = [
        "# VA FA24 workflow evidence report",
        "",
        "Generated by `tools/fa24_workflow_evidence.py`.",
        "",
        "> Offline evidence only. This report does not edit definitions, prove",
        "> calibration semantics, validate checksums, or authorize ECU flashing.",
        "",
        f"- Definitions: `{definitions_root}`",
        f"- Plaintext corpus: `{rom_root}`",
        f"- Packs scanned: **{len(results)}**",
        "",
        "## Summary",
        "",
        "| CID | Workflow | Plaintext ROM | Required tables | Byte evidence | Status |",
        "|---|---|---|---|---|---|",
    ]
    for result in results:
        checks = {table["id"]: table["check"] for table in result["tables"]}
        evidence = sum(value in {"stock_2_0L", "stock_80deg", "stock_pattern", "bounded_nonblank"} for value in checks.values())
        present = sum(table["present"] for table in result["tables"])
        lines.append(
            f"| `{result['cid']}` | {'yes' if result['workflow_declared'] else 'no'} | "
            f"{'yes' if result['rom'] else 'no'} | {present}/5 | {evidence}/5 | `{result['status']}` |"
        )
    lines.extend(
        [
            "",
            "## Per-table evidence",
            "",
            "The plaintext images are ECU-NOR images with the table definitions'",
            "canonical addresses translated to `raw_offset = address - 0x6000`.",
            "Scalar and 1-D checks compare known stock bytes; AVCS checks only",
            "bounded, nonblank 10×16×uint16 regions.",
            "",
            "| CID | Table | Canonical address | Raw offset | Check |",
            "|---|---|---:|---:|---|",
        ]
    )
    for result in results:
        for table in result["tables"]:
            address = f"`0x{table['address']:X}`" if isinstance(table.get("address"), int) else "—"
            offset = f"`0x{table['raw_offset']:X}`" if isinstance(table.get("raw_offset"), int) else "—"
            lines.append(f"| `{result['cid']}` | `{table['id']}` | {address} | {offset} | `{table['check']}` |")
    lines.extend(
        [
            "",
            "## Promotion guidance",
            "",
            "A `candidate_byte_checked` pack has all five current modal table IDs",
            "and passes the limited offline byte/shape checks. It still needs",
            "human review of the basemap semantics and pack declaration before",
            "workflow opt-in. `declared_set_mismatch` means the TOML workflow's",
            "required-table list disagrees with the actual UI implementation and",
            "must be corrected before relying on the pack gate.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    code_root = Path(__file__).resolve().parents[1]
    workspace_root = code_root.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--definitions", type=Path, default=code_root / "definitions" / "impreza")
    parser.add_argument("--rom-root", type=Path, default=workspace_root / "findings" / "engine-ecu-rom-plaintext")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    paths = sorted(p for p in args.definitions.glob("lf*.toml") if "_cobb_" not in p.stem)
    definitions = {p.stem: (p, tomllib.loads(p.read_text(encoding="utf-8"))) for p in paths}
    results = [build_result(path, definitions, args.rom_root) for path in paths]
    markdown = render(results, args.definitions, args.rom_root)
    document = {"tool": "fa24_workflow_evidence", "results": results}
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(markdown, encoding="utf-8")
    else:
        print(markdown, end="")
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
