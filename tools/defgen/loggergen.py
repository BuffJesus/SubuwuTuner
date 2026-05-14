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
                                          list[dict[str, Any]],
                                          list[tuple[str, str, str]]]:
    """Return (scalings, pid_dicts, switch_dicts, warnings).

    `pid_dicts` and `switch_dicts` carry the [[pid]]/[[switch]] field
    values as plain dicts so we can emit them through defgen's TOML
    writer without inventing a new dataclass.
    """
    root = ET.fromstring(text)
    proto = root.find('protocols/protocol[@id="SSM"]')
    if proto is None:
        raise ValueError("no SSM protocol in logger XML")

    scalings: list[defgen.ScalingRecord] = []
    seen_scaling_ids: set[str] = set()
    pids: list[dict[str, Any]] = []
    switches: list[dict[str, Any]] = []
    warnings: list[tuple[str, str, str]] = []

    params_el = proto.find("parameters")
    if params_el is not None:
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

    switches_el = proto.find("switches")
    if switches_el is not None:
        for sw in switches_el.findall("switch"):
            raw_id = (sw.get("id") or "").strip()
            if not raw_id:
                continue
            sw_id = defgen._slugify(raw_id)
            byte_str = sw.get("byte")
            bit_str  = sw.get("bit")
            if not byte_str or not bit_str:
                warnings.append(("switch", sw_id, "missing byte or bit; skipped"))
                continue
            try:
                bit = int(bit_str)
            except ValueError:
                warnings.append(("switch", sw_id, f"non-numeric bit {bit_str!r}; skipped"))
                continue
            if not (0 <= bit <= 7):
                warnings.append(("switch", sw_id, f"bit {bit} out of range 0..7; skipped"))
                continue
            switches.append({
                "id":          sw_id,
                "name":        (sw.get("name") or "").strip(),
                "ssm_address": defgen._hex(_parse_address(byte_str)),
                "bit":         bit,
                "default_log": False,
            })

    return (scalings, pids, switches, warnings)


def parse_ecuparams(text: str) -> tuple[list[defgen.ScalingRecord],
                                         dict[str, list[dict[str, Any]]],
                                         list[tuple[str, str, str]]]:
    """Return (scalings, per_ecuid_pids, warnings).

    `per_ecuid_pids` maps a RomRaider ecuid (the long hex like
    `1B14400305`) to a list of [[pid]] dicts for the ecuparams that
    target that ecuid. The same scaling collection format as
    `parse_logger_xml` so the caller can union them.

    An <ecu id="X Y Z"> with space-separated ids fans out — every listed
    ecuid receives a copy of the same address/scaling.
    """
    root = ET.fromstring(text)
    proto = root.find('protocols/protocol[@id="SSM"]')
    if proto is None:
        raise ValueError("no SSM protocol in logger XML")

    scalings: list[defgen.ScalingRecord] = []
    seen_scaling_ids: set[str] = set()
    per_ecuid: dict[str, list[dict[str, Any]]] = {}
    warnings: list[tuple[str, str, str]] = []

    ecuparams_el = proto.find("ecuparams")
    if ecuparams_el is None:
        return (scalings, per_ecuid, warnings)

    for ep in ecuparams_el.findall("ecuparam"):
        raw_id = (ep.get("id") or "").strip()
        if not raw_id:
            continue
        pid_id = defgen._slugify(raw_id)
        name   = (ep.get("name") or "").strip()

        # The shared conversion applies to every ecu under this ecuparam.
        conv_el = ep.find("conversions/conversion")
        if conv_el is None:
            warnings.append(("ecuparam", pid_id, "no <conversion>; skipped"))
            continue

        # Address length defaults to 1 byte but each <ecu> can override.
        # Treat the conversion's data_type as a placeholder; the per-ecu
        # length determines the real one.
        for ecu in ep.findall("ecu"):
            addr_el = ecu.find("address")
            if addr_el is None:
                continue
            try:
                length, data_type = _length_to_data_type(addr_el.get("length"))
            except ValueError as e:
                warnings.append(("ecuparam", pid_id,
                                 f"ecuid={ecu.get('id')!r}: {e}; skipped"))
                continue
            address = _parse_address(addr_el.text)

            scaling = _scaling_for_conversion(conv_el, data_type, warnings, pid_id)
            if scaling is None:
                continue
            if scaling.id not in seen_scaling_ids:
                scalings.append(scaling)
                seen_scaling_ids.add(scaling.id)

            pid_record = {
                "id":          pid_id,
                "name":        name,
                "ssm_address": defgen._hex(address),
                "length":      length,
                "data_type":   data_type,
                "scaling":     scaling.id,
                "unit":        scaling.unit,
                "default_log": False,
            }
            for ecuid in (ecu.get("id") or "").split():
                ecuid = ecuid.strip()
                if not ecuid:
                    continue
                per_ecuid.setdefault(ecuid, []).append(pid_record)

    return (scalings, per_ecuid, warnings)


def emit_pack_toml(scalings: list[defgen.ScalingRecord],
                   pids: list[dict[str, Any]],
                   switches: list[dict[str, Any]] | None = None,
                   *,
                   pack_id: str = "subaru-ssm-pids",
                   pack_display: str = "Subaru SSM datalogger PIDs",
                   platform: str = "subaru.ssm",
                   header_comment: str = (
                       "# Generated by tools/defgen/loggergen.py from a public RomRaider\n"
                       "# logger XML. SSM datalogger PIDs (protocol-level addresses, shared\n"
                       "# across all SSM-capable Subaru ECUs). See definitions/README.md.\n\n"
                   )) -> str:
    """Build a standalone pack TOML with [[scaling]], [[pid]], [[switch]] records."""
    out: list[str] = [header_comment]
    out.append(defgen._emit_table("[pack]", {
        "schema_version": 1,
        "id":             pack_id,
        "display_name":   pack_display,
        "platform":       platform,
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
    for sw in switches or []:
        out.append(defgen._emit_table("[[switch]]", sw))
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def _resolve_ecuid_to_packs(packs_dir: Path) -> dict[str, list[tuple[Path, str]]]:
    """Scan a packs dir and return ecuid -> [(pack_path, rom_id), ...]."""
    import tomllib
    out: dict[str, list[tuple[Path, str]]] = {}
    for f in packs_dir.rglob("*.toml"):
        try:
            raw = tomllib.loads(f.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        pack_id = raw.get("pack", {}).get("id", "")
        for ident in raw.get("identification", []):
            ecuid = (ident.get("ecu_part") or "").strip()
            if ecuid:
                out.setdefault(ecuid, []).append((f, pack_id))
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("xml", type=Path, help="Path to a RomRaider logger XML")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output TOML path for the shared PID + switch "
                             "pack (default: stdout)")
    parser.add_argument("--ecuparams-into", type=Path, metavar="OUTDIR",
                        help="Also emit per-CID ecuparam fragments. For every "
                             "<ecuparam>, match its <ecu id=…> against the "
                             "`ecu_part` field in --packs-dir to discover the "
                             "ROM id, then write OUTDIR/<rom_id>.toml.")
    parser.add_argument("--packs-dir", type=Path, metavar="DIR",
                        help="Directory of existing pack TOMLs to scan for "
                             "ecuid -> rom_id mapping. Required with "
                             "--ecuparams-into.")
    args = parser.parse_args(argv)

    try:
        text = args.xml.read_text(encoding="utf-8")
    except OSError as e:
        print(f"loggergen: cannot read {args.xml}: {e}", file=sys.stderr)
        return 1

    try:
        scalings, pids, switches, warnings = parse_logger_xml(text)
    except (ET.ParseError, ValueError) as e:
        print(f"loggergen: parse failure: {e}", file=sys.stderr)
        return 1

    output = emit_pack_toml(scalings, pids, switches)
    if args.output is not None:
        args.output.write_text(output, encoding="utf-8")
        print(f"loggergen: wrote {args.output} "
              f"({len(pids)} PIDs, {len(switches)} switches, "
              f"{len(scalings)} scalings)",
              file=sys.stderr)
    else:
        sys.stdout.write(output)

    if args.ecuparams_into is not None:
        if args.packs_dir is None:
            print("loggergen: --ecuparams-into requires --packs-dir",
                  file=sys.stderr)
            return 2
        ep_scalings, per_ecuid, ep_warnings = parse_ecuparams(text)
        warnings.extend(ep_warnings)
        ecuid_to_packs = _resolve_ecuid_to_packs(args.packs_dir)
        args.ecuparams_into.mkdir(parents=True, exist_ok=True)

        # Group per rom_id (multiple ecuids can hit the same pack since some
        # ROMs have multiple identifications).
        per_rom: dict[Path, dict[str, dict[str, Any]]] = {}
        unmatched = 0
        for ecuid, records in per_ecuid.items():
            matches = ecuid_to_packs.get(ecuid)
            if not matches:
                unmatched += 1
                continue
            for pack_path, pack_id in matches:
                bucket = per_rom.setdefault(pack_path, {})
                for r in records:
                    bucket.setdefault(r["id"], r)  # dedup by pid id

        written = 0
        for pack_path, by_pid in per_rom.items():
            rom_id = pack_path.stem
            frag = emit_pack_toml(
                ep_scalings,
                list(by_pid.values()),
                switches=None,
                pack_id=f"subaru-ssm-ecuparams-{rom_id}",
                pack_display=f"SSM extended PIDs for {rom_id.upper()}",
                platform=f"subaru.ssm.ecuparams.{rom_id}",
                header_comment=(
                    f"# Generated by tools/defgen/loggergen.py: SSM extended\n"
                    f"# PIDs (<ecuparam>) targeting ROM {rom_id.upper()}.\n"
                    f"# Loaded alongside the ECU pack at the same CID.\n\n"
                ),
            )
            out_path = args.ecuparams_into / f"{rom_id}.toml"
            out_path.write_text(frag, encoding="utf-8")
            written += 1

        print(f"loggergen: wrote {written} ecuparam fragments to "
              f"{args.ecuparams_into} ({unmatched} ecuid(s) had no matching pack)",
              file=sys.stderr)

    if warnings:
        print(f"loggergen: {len(warnings)} warning(s):", file=sys.stderr)
        for kind, rec_id, detail in warnings:
            print(f"  {kind}: {rec_id}: {detail}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
