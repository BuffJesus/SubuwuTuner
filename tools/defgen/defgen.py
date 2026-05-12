#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
defgen — convert RomRaider EcuFlash-style XML definitions into SubuwuTuner TOML.

This tool extracts *facts* (memory addresses, scaling coefficients, axis
breakpoints, CRC polynomials) from public RomRaider XML and re-encodes them
in our own schema (see docs/11-definition-format.md). It does NOT copy
descriptive text, comments, or other expressive content; the resulting TOML
carries only objective ECU data plus a generated header noting the source.

Clean-room rules (see docs/01-reverse-engineering.md):
  - Facts are not copyrightable; addresses and scalings are facts.
  - Description text is expression; we strip it.
  - We do not redistribute the source XML.

Usage:
    python defgen.py <rom.xml> [-o output.toml]
    python defgen.py <rom.xml> --rom-id AS80U   # filter to a specific romid
"""

from __future__ import annotations

import argparse
import math
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Type mapping
# ---------------------------------------------------------------------------

# RomRaider storagetype + endian -> our DataType string.
_STORAGE_BASE = {
    "uint8":  "uint8",
    "int8":   "int8",
    "uint16": "uint16",
    "int16":  "int16",
    "uint32": "uint32",
    "int32":  "int32",
    "float":  "float32",
    "float32": "float32",
}


def map_data_type(storagetype: str | None, endian: str | None) -> str:
    """Map RomRaider storagetype+endian to our canonical DataType string."""
    if not storagetype:
        return "uint16_be"  # most-common Subaru default
    base = _STORAGE_BASE.get(storagetype.lower().strip())
    if base is None:
        raise ValueError(f"unknown storagetype: {storagetype!r}")
    if base in ("uint8", "int8"):
        return base
    suffix = "_be" if (endian or "big").lower() == "big" else "_le"
    return base + suffix


# ---------------------------------------------------------------------------
# toexpr parsing (linear only, the 95% case)
# ---------------------------------------------------------------------------

# Matches expressions like:
#   x
#   x*0.5
#   x * 0.5
#   x*0.5+10
#   x*0.5 - 10
#   (x-100)*0.5     -> factor=0.5, offset=-50
# Returns (factor, offset) for the equivalent `raw * factor + offset`, or None
# if we can't parse it as linear.
_LINEAR_RE = re.compile(
    r"""^\s*
        (?:\(\s*x\s*([-+])\s*([0-9]+(?:\.[0-9]+)?)\s*\)\s*\*\s*([0-9]+(?:\.[0-9]+)?)
        |   x\s*\*\s*([-+]?[0-9]+(?:\.[0-9]+)?)\s*(?:([-+])\s*([0-9]+(?:\.[0-9]+)?))?
        |   x
        )\s*$""",
    re.VERBOSE,
)


def parse_toexpr(expr: str) -> tuple[float, float] | None:
    """Return (factor, offset) for a linear `toexpr`, or None if non-linear."""
    if expr is None:
        return (1.0, 0.0)
    m = _LINEAR_RE.match(expr)
    if m is None:
        return None
    paren_sign, paren_offset, paren_factor, mul_factor, post_sign, post_offset = m.groups()
    if paren_factor is not None:
        # (x ± off) * factor   ==   x*factor ± off*factor
        f = float(paren_factor)
        o = float(paren_offset) * f
        if paren_sign == "-":
            o = -o
        return (f, o)
    if mul_factor is not None:
        f = float(mul_factor)
        o = 0.0
        if post_factor := post_offset:
            o = float(post_factor)
            if post_sign == "-":
                o = -o
        return (f, o)
    # plain "x"
    return (1.0, 0.0)


# ---------------------------------------------------------------------------
# XML extraction
# ---------------------------------------------------------------------------

@dataclass
class ScalingRecord:
    id: str
    factor: float = 1.0
    offset: float = 0.0
    unit: str = ""
    minimum: float = 0.0
    maximum: float = 0.0
    precision: int = 0
    data_type: str = "uint16_be"
    formula: str = "linear"  # only "linear" is generated; piecewise is manual

    def to_toml(self) -> str:
        return _emit_table("[[scaling]]", {
            "id":        self.id,
            "formula":   self.formula,
            "factor":    self.factor,
            "offset":    self.offset,
            "unit":      self.unit,
            "min":       self.minimum,
            "max":       self.maximum,
            "precision": self.precision,
            "data_type": self.data_type,
        })


@dataclass
class AxisRecord:
    id: str
    name: str = ""
    unit: str = ""
    type: str = "static"
    address: int = 0
    length: int = 0
    data_type: str = "uint16_be"
    scaling: str = ""

    def to_toml(self) -> str:
        return _emit_table("[[axis]]", {
            "id":        self.id,
            "name":      self.name,
            "unit":      self.unit,
            "type":      self.type,
            "address":   _hex(self.address),
            "length":    self.length,
            "data_type": self.data_type,
            "scaling":   self.scaling,
        })


@dataclass
class TableRecord:
    id: str
    name: str = ""
    category: str = ""
    dimensions: int = 2
    address: int = 0
    data_type: str = "uint16_be"
    scaling: str = ""
    axis_x: str = ""
    axis_y: str = ""
    axis_z: str = ""
    emissions_relevant: bool = False
    engine_safety_critical: bool = False

    def to_toml(self) -> str:
        body: dict[str, Any] = {
            "id":         self.id,
            "name":       self.name,
            "category":   self.category,
            "dimensions": self.dimensions,
            "address":    _hex(self.address),
            "data_type":  self.data_type,
            "scaling":    self.scaling,
        }
        if self.axis_x:
            body["axis_x"] = self.axis_x
        if self.axis_y:
            body["axis_y"] = self.axis_y
        if self.axis_z:
            body["axis_z"] = self.axis_z
        body["emissions_relevant"]     = self.emissions_relevant
        body["engine_safety_critical"] = self.engine_safety_critical
        return _emit_table("[[table]]", body)


@dataclass
class IdentificationRecord:
    name: str
    cid_address: int
    cid_length: int
    cid_match: str
    ecu_part: str = ""

    def to_toml(self) -> str:
        return _emit_table("[[identification]]", {
            "name":        self.name,
            "cid_address": _hex(self.cid_address),
            "cid_length":  self.cid_length,
            "cid_match":   self.cid_match,
            "ecu_part":    self.ecu_part,
        })


@dataclass
class Pack:
    rom_id: str
    display_name: str = ""
    platform: str = "subaru"
    transmission: str = ""
    years: list[int] = field(default_factory=list)
    rom_size_bytes: int = 0
    identifications: list[IdentificationRecord] = field(default_factory=list)
    scalings: list[ScalingRecord] = field(default_factory=list)
    axes: list[AxisRecord] = field(default_factory=list)
    tables: list[TableRecord] = field(default_factory=list)


# ---------------------------------------------------------------------------
# TOML emit helpers (hand-rolled to avoid Python deps)
# ---------------------------------------------------------------------------

def _hex(value: int) -> str:
    return f"0x{value:08X}"


def _toml_value(v: Any) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if math.isfinite(v):
            return repr(v)
        return "nan"
    if isinstance(v, str):
        # Hex literals come through as already-formatted strings starting with 0x.
        # Pass them through unquoted so they parse as integers in TOML.
        if re.fullmatch(r"0x[0-9A-Fa-f]+", v):
            return v
        return _toml_string(v)
    if isinstance(v, list):
        return "[" + ", ".join(_toml_value(x) for x in v) + "]"
    raise TypeError(f"unsupported TOML value type: {type(v).__name__}")


def _toml_string(s: str) -> str:
    # Always use basic strings with escapes; the strings we emit are short and
    # ASCII-only after we strip description text.
    escaped = (s.replace("\\", "\\\\")
                .replace('"', '\\"')
                .replace("\n", "\\n")
                .replace("\r", "\\r")
                .replace("\t", "\\t"))
    return f'"{escaped}"'


def _emit_table(header: str, fields: dict[str, Any]) -> str:
    lines = [header]
    key_width = max((len(k) for k in fields), default=0)
    for k, v in fields.items():
        lines.append(f"{k.ljust(key_width)} = {_toml_value(v)}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Pack assembly + TOML emission
# ---------------------------------------------------------------------------

_HEADER = """\
# Generated by tools/defgen from a public RomRaider EcuFlash-format XML
# source. Only factual ECU data is reproduced; descriptive text is stripped
# per the clean-room rules in docs/01-reverse-engineering.md. Hand-edit any
# descriptions you want in this file; defgen will not overwrite them unless
# you re-run it explicitly.

"""


def pack_to_toml(pack: Pack) -> str:
    out: list[str] = [_HEADER]
    out.append(_emit_table("[pack]", {
        "schema_version": 1,
        "id":             pack.rom_id,
        "display_name":   pack.display_name,
        "platform":       pack.platform,
        "transmission":   pack.transmission,
        "years":          pack.years,
        "endianness":     "big",
        "rom_size_bytes": pack.rom_size_bytes,
        "license":        "Apache-2.0",
    }))
    out.append("")
    for ident in pack.identifications:
        out.append(ident.to_toml())
        out.append("")
    for s in pack.scalings:
        out.append(s.to_toml())
        out.append("")
    for a in pack.axes:
        out.append(a.to_toml())
        out.append("")
    for t in pack.tables:
        out.append(t.to_toml())
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ---------------------------------------------------------------------------
# XML -> Pack
# ---------------------------------------------------------------------------

def _parse_int(s: str | None) -> int:
    if s is None or s == "":
        return 0
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s)


def _parse_filesize(s: str | None) -> int:
    """Accepts '1.5MB', '1572864', '1MB', etc."""
    if s is None or s == "":
        return 0
    s = s.strip().upper()
    m = re.match(r"([0-9.]+)\s*(KB|MB)?", s)
    if not m:
        return 0
    value = float(m.group(1))
    unit = m.group(2) or ""
    if unit == "MB":
        return int(value * 1024 * 1024)
    if unit == "KB":
        return int(value * 1024)
    return int(value)


def _slugify(s: str) -> str:
    """Convert a free-text name to a snake_case id."""
    s = s.lower().strip()
    s = re.sub(r"[^a-z0-9]+", "_", s)
    s = s.strip("_")
    return s or "unnamed"


def _rom_xmlid(rom: ET.Element) -> str:
    romid = rom.find("romid")
    if romid is None:
        return ""
    return (romid.findtext("xmlid") or "").strip()


def _rom_base(rom: ET.Element) -> str:
    """Return the parent xmlid this <rom> inherits from, or empty string.

    RomRaider supports inheritance in two equivalent forms in the wild:
        <rom base="PARENT_XMLID">              (attribute on <rom>)
        <romid><base>PARENT_XMLID</base></romid>
    We accept either.
    """
    if (b := rom.get("base")) is not None and b.strip():
        return b.strip()
    romid = rom.find("romid")
    if romid is None:
        return ""
    # Either <base> child element or `base` attribute on <romid>.
    if (b := romid.findtext("base")) is not None and b.strip():
        return b.strip()
    if (b := romid.get("base")) is not None and b.strip():
        return b.strip()
    return ""


def _flatten_rom_inheritance(rom: ET.Element, by_xmlid: dict[str, ET.Element],
                              visiting: set[str] | None = None) -> ET.Element:
    """Return a virtual <rom> with this rom's parents merged in.

    Inherits <table> and <scaling> elements by name from each ancestor;
    closer-to-leaf elements override more distant ancestors. The original
    XML tree is not mutated — we return a fresh `<rom>` element whose
    children are references to the original elements (good enough for our
    read-only parsing).
    """
    visiting = visiting if visiting is not None else set()
    xmlid    = _rom_xmlid(rom)
    if xmlid in visiting:
        raise ValueError(f"inheritance cycle detected at <rom xmlid={xmlid!r}>")
    visiting = visiting | {xmlid}

    base_id = _rom_base(rom)
    if not base_id:
        return rom
    parent_raw = by_xmlid.get(base_id)
    if parent_raw is None:
        # Missing parent — treat as standalone with a warning printed by
        # caller. Returning self avoids a hard error so partial XML still
        # produces something useful.
        return rom

    parent = _flatten_rom_inheritance(parent_raw, by_xmlid, visiting)

    # Build a fresh <rom> element that combines both. Copy attributes from
    # the child (the child wins on attributes too).
    merged = ET.Element("rom", rom.attrib)

    # romid: child wins wholesale; if the child's romid is sparse, fill in
    # missing leaf fields from the parent's romid.
    child_romid  = rom.find("romid")
    parent_romid = parent.find("romid")
    if child_romid is not None:
        merged_romid = ET.SubElement(merged, "romid")
        # Start from parent fields...
        if parent_romid is not None:
            for el in parent_romid:
                ET.SubElement(merged_romid, el.tag).text = el.text
        # ...then let child override.
        for el in child_romid:
            existing = merged_romid.find(el.tag)
            if existing is not None:
                existing.text = el.text
            else:
                ET.SubElement(merged_romid, el.tag).text = el.text

    # Indexes from parent that the child can override by name.
    parent_tables   = {(t.get("name") or ""): t for t in parent.findall("table")}
    parent_scalings = {(s.get("name") or ""): s for s in parent.findall("scaling")}

    child_table_names   = {(t.get("name") or "") for t in rom.findall("table")}
    child_scaling_names = {(s.get("name") or "") for s in rom.findall("scaling")}

    # Emit parent's items that the child didn't override.
    for name, el in parent_tables.items():
        if name not in child_table_names:
            merged.append(el)
    for name, el in parent_scalings.items():
        if name not in child_scaling_names:
            merged.append(el)
    # Then child's own items.
    for t in rom.findall("table"):
        merged.append(t)
    for s in rom.findall("scaling"):
        merged.append(s)
    return merged


def parse_rom_xml(xml_text: str, rom_id_filter: str | None = None) -> list[Pack]:
    """Parse a RomRaider XML string into one or more Pack records.

    A single XML file may contain multiple <rom> entries (often one per CID
    in a model year range, with shared definitions in a <base> rom). We
    flatten <base> inheritance before emitting Packs.

    rom_id_filter, when set, narrows the output to a single xmlid.

    Pure-base entries (those whose xmlid is referenced by another rom but
    don't define their own identification) are dropped from the output;
    they exist only to provide inherited data.
    """
    root = ET.fromstring(xml_text)
    if root.tag == "roms":
        rom_elements = list(root.findall("rom"))
    elif root.tag == "rom":
        rom_elements = [root]
    else:
        raise ValueError(f"unexpected root element: <{root.tag}>")

    # Index for base resolution.
    by_xmlid: dict[str, ET.Element] = {}
    for rom in rom_elements:
        xid = _rom_xmlid(rom)
        if xid:
            by_xmlid[xid] = rom

    # Roms referenced as a base by some other rom — skip them in the output
    # unless they themselves carry an internalidstring (a real CID).
    referenced_as_base = {_rom_base(rom) for rom in rom_elements if _rom_base(rom)}

    packs: list[Pack] = []
    for rom in rom_elements:
        xmlid = _rom_xmlid(rom)
        if not xmlid:
            continue
        if rom_id_filter is not None and xmlid != rom_id_filter:
            continue

        flat = _flatten_rom_inheritance(rom, by_xmlid)
        # Drop pure bases: they're referenced but don't themselves declare a
        # CID, so they're not a tunable rom. Allow them through when the
        # caller explicitly asks for them via --rom-id.
        if rom_id_filter is None and xmlid in referenced_as_base:
            romid = flat.find("romid")
            cid   = (romid.findtext("internalidstring") if romid is not None else "") or ""
            if not cid.strip():
                continue

        pack = _rom_to_pack(flat)
        if pack is not None:
            packs.append(pack)
    return packs


def _rom_to_pack(rom: ET.Element) -> Pack | None:
    romid = rom.find("romid")
    if romid is None:
        return None

    xmlid = (romid.findtext("xmlid") or "").strip()
    if not xmlid:
        return None

    pack = Pack(rom_id=_slugify(xmlid))
    pack.display_name   = xmlid
    pack.transmission   = (romid.findtext("transmission") or "").strip().lower() or ""
    pack.rom_size_bytes = _parse_filesize(romid.findtext("filesize"))

    year_text = (romid.findtext("year") or "").strip()
    if year_text.isdigit():
        pack.years = [int(year_text)]

    market = (romid.findtext("market") or "").strip()
    make = (romid.findtext("make") or "subaru").strip().lower()
    model = (romid.findtext("model") or "").strip().lower()
    if market and model:
        pack.platform = f"{make}.{model}"
    elif make:
        pack.platform = make

    # Identification — DO NOT strip cid_str: real ECUs pad with spaces and the
    # padding is part of the byte sequence we have to match.
    cid_addr = _parse_int(romid.findtext("internalidaddress"))
    cid_str  = romid.findtext("internalidstring") or ""
    if cid_str.strip():
        pack.identifications.append(IdentificationRecord(
            name=xmlid,
            cid_address=cid_addr,
            cid_length=len(cid_str),
            cid_match=cid_str,
            ecu_part=(romid.findtext("ecuid") or "").strip(),
        ))

    # Top-level scalings (often outside any table)
    seen_scaling_ids: set[str] = set()
    for s_el in rom.findall("scaling"):
        rec = _scaling_from_element(s_el)
        if rec is not None and rec.id not in seen_scaling_ids:
            pack.scalings.append(rec)
            seen_scaling_ids.add(rec.id)

    # Tables (and their inline scalings + axes)
    for t_el in rom.findall("table"):
        ttype = (t_el.get("type") or "").strip()
        # Skip axis-only top-level tables (rare, but possible)
        if ttype.lower() in ("x axis", "y axis", "static y axis"):
            continue
        _extract_table(t_el, pack, seen_scaling_ids)

    return pack


def _scaling_from_element(el: ET.Element) -> ScalingRecord | None:
    name = (el.get("name") or "").strip()
    if not name:
        return None
    storagetype = el.get("storagetype")
    endian = el.get("endian")
    toexpr = el.get("toexpr") or "x"
    parsed = parse_toexpr(toexpr)
    if parsed is None:
        # Non-linear toexpr; emit a linear identity and let the user replace
        # the formula by hand. Better than dropping the scaling entirely.
        factor, offset = (1.0, 0.0)
    else:
        factor, offset = parsed

    try:
        data_type = map_data_type(storagetype, endian)
    except ValueError:
        data_type = "uint16_be"

    return ScalingRecord(
        id=_slugify(name),
        factor=factor,
        offset=offset,
        unit=(el.get("units") or "").strip(),
        minimum=float(el.get("min") or 0.0),
        maximum=float(el.get("max") or 0.0),
        precision=_format_to_precision(el.get("format")),
        data_type=data_type,
    )


def _format_to_precision(fmt: str | None) -> int:
    """`0.00` -> 2, `0.000` -> 3, `0` -> 0."""
    if not fmt:
        return 0
    m = re.search(r"\.([0#]+)", fmt)
    return len(m.group(1)) if m else 0


def _extract_table(t_el: ET.Element, pack: Pack, seen_scaling_ids: set[str]) -> None:
    name = (t_el.get("name") or "").strip()
    if not name:
        return
    ttype = (t_el.get("type") or "").strip()
    address = _parse_int(t_el.get("address"))

    # Dimensions
    if ttype.endswith("3D") or ttype == "3D":
        dims = 2  # RomRaider's "3D" = 2-axis lookup
    elif ttype.endswith("2D") or ttype == "2D":
        dims = 1
    else:
        dims = 1

    # Inline scaling
    inline_scaling = t_el.find("scaling")
    scaling_id = ""
    data_type = "uint16_be"
    if inline_scaling is not None:
        rec = _scaling_from_element(inline_scaling)
        if rec is not None:
            scaling_id = rec.id
            data_type = rec.data_type
            if rec.id not in seen_scaling_ids:
                pack.scalings.append(rec)
                seen_scaling_ids.add(rec.id)

    # Axes (nested <table type="X Axis"/> and <table type="Y Axis"/>)
    axis_x_id = ""
    axis_y_id = ""
    for child in t_el.findall("table"):
        child_type = (child.get("type") or "").strip().lower()
        if child_type not in ("x axis", "y axis", "static y axis", "static x axis"):
            continue
        axis = _axis_from_element(child)
        if axis is None:
            continue
        if axis.id not in {a.id for a in pack.axes}:
            pack.axes.append(axis)
            # also capture the axis's inline scaling if present
            axis_scaling_el = child.find("scaling")
            if axis_scaling_el is not None:
                rec = _scaling_from_element(axis_scaling_el)
                if rec is not None and rec.id not in seen_scaling_ids:
                    pack.scalings.append(rec)
                    seen_scaling_ids.add(rec.id)
        if child_type in ("x axis", "static x axis"):
            axis_x_id = axis.id
        elif child_type in ("y axis", "static y axis"):
            axis_y_id = axis.id

    table = TableRecord(
        id=_slugify(name),
        name=name if _is_factual_name(name) else "",
        category=(t_el.get("category") or "").strip().lower(),
        dimensions=dims,
        address=address,
        data_type=data_type,
        scaling=scaling_id,
        axis_x=axis_x_id,
        axis_y=axis_y_id,
    )
    pack.tables.append(table)


def _axis_from_element(el: ET.Element) -> AxisRecord | None:
    name = (el.get("name") or "").strip()
    if not name:
        return None
    elements = el.get("elements")
    inline_scaling = el.find("scaling")
    data_type = "uint16_be"
    scaling_id = ""
    if inline_scaling is not None:
        rec = _scaling_from_element(inline_scaling)
        if rec is not None:
            data_type = rec.data_type
            scaling_id = rec.id

    return AxisRecord(
        id=_slugify(name),
        name=name if _is_factual_name(name) else "",
        unit="",  # we don't carry the axis's unit text from XML
        type="static",
        address=_parse_int(el.get("address")),
        length=int(elements) if elements and elements.isdigit() else 0,
        data_type=data_type,
        scaling=scaling_id,
    )


def _is_factual_name(name: str) -> bool:
    """Allow short, technical-looking names; strip flowery descriptions.

    The clean-room rule is to leave descriptive text out. A short identifier-
    like name (e.g. "Boost Target", "RPM Axis") is borderline; we keep it
    and trust the user to override if they want. A long sentence-like name
    we drop.
    """
    return len(name) <= 64 and "." not in name and "," not in name


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("xml", type=Path, help="Path to a RomRaider XML file")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output TOML path (default: stdout)")
    parser.add_argument("--rom-id",
                        help="Only emit the rom matching this xmlid")
    args = parser.parse_args(argv)

    try:
        text = args.xml.read_text(encoding="utf-8")
    except OSError as e:
        print(f"defgen: cannot read {args.xml}: {e}", file=sys.stderr)
        return 1

    try:
        packs = parse_rom_xml(text, rom_id_filter=args.rom_id)
    except (ET.ParseError, ValueError) as e:
        print(f"defgen: parse failure: {e}", file=sys.stderr)
        return 1

    if not packs:
        print("defgen: no matching <rom> entries", file=sys.stderr)
        return 1

    if len(packs) > 1 and args.output and args.output.is_file():
        print(f"defgen: {len(packs)} roms found; pass --rom-id to select one,"
              f" or pass a directory to -o.", file=sys.stderr)
        return 1

    if args.output and args.output.is_dir():
        for pack in packs:
            path = args.output / f"{pack.rom_id}.toml"
            path.write_text(pack_to_toml(pack), encoding="utf-8")
            print(f"wrote {path}", file=sys.stderr)
    elif args.output:
        args.output.write_text(pack_to_toml(packs[0]), encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        for i, pack in enumerate(packs):
            if i > 0:
                print()
            sys.stdout.write(pack_to_toml(pack))

    return 0


if __name__ == "__main__":
    sys.exit(main())
