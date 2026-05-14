#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
loggergen — convert a RomRaider logger XML into a SubuwuTuner pack TOML of
SSM datalogger PIDs.

The output is a self-contained pack (`[pack] schema_version = 1` plus
`[[scaling]]` and `[[pid]]` arrays) intended to be loaded alongside an
ECU pack at runtime. RomRaider's logger.xml carries three kinds of entry:

  - <parameter>  — standard SSM PIDs at protocol-defined addresses
                   (same for every SSM-capable Subaru ECU). Emitted as
                   one [[pid]] per parameter.
  - <switch>     — single-bit status flags. Not yet modelled in our schema;
                   skipped with a warning.
  - <ecuparam>   — extended parameters whose RAM address varies per ROM CID.
                   Skipped in this pass; future work to merge into per-CID
                   packs.

Each <parameter> declares one or more <conversion>s (units + linear
expression). We emit the FIRST conversion as the canonical scaling for
that PID; alternative-unit conversions are dropped (the user can carry
their preferred units by hand if it matters). Conversion expressions go
through `defgen.parse_toexpr`, which handles linear compositions and
RomRaider's `/*RSHIFT(N)*/`/`/*LSHIFT(N)*/` fixed-point hints; non-linear
forms (`(x-128)*100/x`, INVERSE_DIVIDE, AND-masks) flatten to identity
with a warning.

Usage:
    python loggergen.py path/to/logger.xml -o definitions/pids.toml
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

# Reuse defgen utilities.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import defgen  # noqa: E402


def _parse_address(text: str | None) -> int:
    """RomRaider logger addresses are bare hex with `0x` prefix."""
    if text is None:
        return 0
    text = text.strip()
    if text.lower().startswith("0x"):
        return int(text, 16)
    return int(text, 16)


def _length_to_data_type(length: str | None) -> tuple[int, str]:
    """`<address length="N">` -> (byte_length, our DataType string).

    Logger addresses default to length=1 (single byte). Multi-byte reads
    use big-endian on Subaru."""
    n = int(length) if length and length.isdigit() else 1
    if n == 1:
        return (1, "uint8")
    if n == 2:
        return (2, "uint16_be")
    if n == 4:
        return (4, "uint32_be")
    raise ValueError(f"unsupported PID byte length: {n}")


def _scaling_for_conversion(conv: ET.Element, data_type: str,
                            warnings: list[tuple[str, str, str]],
                            pid_id: str) -> defgen.ScalingRecord | None:
    units = (conv.get("units") or "").strip()
    expr  = (conv.get("expr") or "x").strip()
    fmt   = conv.get("format")

    parsed = defgen.parse_toexpr(expr)
    if parsed is None:
        factor, offset = (1.0, 0.0)
        warnings.append(("pid", pid_id,
                         f"non-linear conversion flattened to identity: {expr!r}"))
    else:
        factor, offset = parsed

    # Synthesise a stable scaling id from (units, expression). Identical
    # scalings across PIDs (e.g. the % `(x-128)*100/128` correction shape
    # used by every A/F-correction PID) collapse to one [[scaling]] record.
    slug = defgen._slugify(f"{units} {expr}".strip())
    if not slug:
        slug = "identity"

    return defgen.ScalingRecord(
        id=slug,
        factor=factor,
        offset=offset,
        unit=units,
        minimum=float(conv.get("gauge_min") or 0.0),
        maximum=float(conv.get("gauge_max") or 0.0),
        precision=defgen._format_to_precision(fmt),
        data_type=data_type,
    )


def parse_logger_xml(text: str) -> tuple[list[defgen.ScalingRecord],
                                          list[dict[str, Any]],
                                          list[tuple[str, str, str]]]:
    """Return (scalings, pid_dicts, warnings).

    `pid_dicts` carry the [[pid]] field values as plain dicts so we can
    emit them through defgen's TOML writer without inventing a new
    dataclass.
    """
    root = ET.fromstring(text)
    proto = root.find('protocols/protocol[@id="SSM"]')
    if proto is None:
        raise ValueError("no SSM protocol in logger XML")

    scalings: list[defgen.ScalingRecord] = []
    seen_scaling_ids: set[str] = set()
    pids: list[dict[str, Any]] = []
    warnings: list[tuple[str, str, str]] = []

    params_el = proto.find("parameters")
    if params_el is None:
        return (scalings, pids, warnings)

    for p in params_el.findall("parameter"):
        raw_id = (p.get("id") or "").strip()
        if not raw_id:
            continue
        pid_id = defgen._slugify(raw_id)
        name   = (p.get("name") or "").strip()
        addr_el = p.find("address")
        if addr_el is None:
            warnings.append(("pid", pid_id, "no <address> element; skipped"))
            continue
        try:
            length, data_type = _length_to_data_type(addr_el.get("length"))
        except ValueError as e:
            warnings.append(("pid", pid_id, f"{e}; skipped"))
            continue
        address = _parse_address(addr_el.text)

        conv_el = p.find("conversions/conversion")
        if conv_el is None:
            warnings.append(("pid", pid_id, "no <conversion>; skipped"))
            continue
        scaling = _scaling_for_conversion(conv_el, data_type, warnings, pid_id)
        if scaling is not None and scaling.id not in seen_scaling_ids:
            scalings.append(scaling)
            seen_scaling_ids.add(scaling.id)

        pids.append({
            "id":          pid_id,
            "name":        name,
            "ssm_address": defgen._hex(address),
            "length":      length,
            "data_type":   data_type,
            "scaling":     scaling.id if scaling else "",
            "unit":        scaling.unit if scaling else "",
            "default_log": False,
        })

    return (scalings, pids, warnings)


def emit_pack_toml(scalings: list[defgen.ScalingRecord],
                   pids: list[dict[str, Any]]) -> str:
    """Build the standalone PID-pack TOML."""
    out: list[str] = []
    out.append(
        "# Generated by tools/defgen/loggergen.py from a public RomRaider\n"
        "# logger XML. SSM datalogger PIDs (protocol-level addresses, shared\n"
        "# across all SSM-capable Subaru ECUs). See definitions/README.md.\n\n"
    )
    out.append(defgen._emit_table("[pack]", {
        "schema_version": 1,
        "id":             "subaru-ssm-pids",
        "display_name":   "Subaru SSM datalogger PIDs",
        "platform":       "subaru.ssm",
        "transmission":   "",
        "years":          [],
        "endianness":     "big",
        # SSM PID addresses are RAM offsets, not ROM offsets; no ROM bounds
        # to check, so leave at 0 (the validator treats 0 as "unspecified").
        "rom_size_bytes": 0,
        "license":        "Apache-2.0",
    }))
    out.append("")
    for s in scalings:
        out.append(s.to_toml())
        out.append("")
    for p in pids:
        out.append(defgen._emit_table("[[pid]]", p))
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("xml", type=Path, help="Path to a RomRaider logger XML")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output TOML path (default: stdout)")
    args = parser.parse_args(argv)

    try:
        text = args.xml.read_text(encoding="utf-8")
    except OSError as e:
        print(f"loggergen: cannot read {args.xml}: {e}", file=sys.stderr)
        return 1

    try:
        scalings, pids, warnings = parse_logger_xml(text)
    except (ET.ParseError, ValueError) as e:
        print(f"loggergen: parse failure: {e}", file=sys.stderr)
        return 1

    output = emit_pack_toml(scalings, pids)
    if args.output is not None:
        args.output.write_text(output, encoding="utf-8")
        print(f"loggergen: wrote {args.output} "
              f"({len(pids)} PIDs, {len(scalings)} scalings)",
              file=sys.stderr)
    else:
        sys.stdout.write(output)

    if warnings:
        print(f"loggergen: {len(warnings)} warning(s):", file=sys.stderr)
        for kind, rec_id, detail in warnings:
            print(f"  {kind}: {rec_id}: {detail}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
