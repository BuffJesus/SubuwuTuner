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
import ast
import math
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import axis_corrections


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
# toexpr parsing (linear via AST simplification)
# ---------------------------------------------------------------------------
#
# We accept any expression composed of `+`, `-`, `*`, `/`, unary `-`/`+`,
# parentheses, numeric literals, and the variable `x`, and reduce it to
# `(factor, offset)` such that `value == raw * factor + offset`. Each AST
# node carries the same (factor, offset) representation; binary operators
# combine the two operands' linear forms and raise `_NonLinear` when the
# result would be non-linear (e.g. `x*x`, `1/x`).
#
# RomRaider also writes hardware-shift semantics inside C-style block
# comments. RSHIFT(N) and LSHIFT(N) compose as linear scaling factors
# `2^-N` and `2^N` respectively, so we pre-expand them into ordinary
# multiplications before parsing. INVERSE_DIVIDE(N) is `N/x`, genuinely
# non-linear, and AND(N) is a bitmask — both leave the `/*…*/` marker
# in place, which trips the `"/*" in expr` short-circuit and falls
# through to the hand-edit warning. The interpretation of RSHIFT/LSHIFT
# as a `2^N` factor is the standard fixed-point convention in Subaru
# ECU work but should be re-verified against real ROM data when the
# bench rig lands.


_SHIFT_RE = re.compile(
    r"/\*(?P<op>RSHIFT|LSHIFT)\(\s*(?P<n>[0-9]+(?:\.[0-9]+)?)\s*\)\*/")


def _expand_shifts(expr: str) -> str:
    """Replace `/*RSHIFT(N)*/` with `(1.0/2**N)*` and `/*LSHIFT(N)*/` with
    `(2**N)*`. Other `/*…*/` markers are left untouched so the upstream
    `"/*" in expr` check still flags them."""

    def repl(m: re.Match[str]) -> str:
        n = float(m.group("n"))
        if m.group("op") == "RSHIFT":
            return f"(1.0/{2.0**n!r})*"
        return f"({2.0**n!r})*"

    return _SHIFT_RE.sub(repl, expr)


class _NonLinear(Exception):
    """Raised internally when an AST node can't be reduced to a*x + b."""


def _simplify(node: ast.AST) -> tuple[float, float]:
    """Reduce `node` to (factor, offset) with the contract value == factor*x + offset.

    Pure constants come back as (0, c); `x` comes back as (1, 0). Operators
    combine according to linear algebra; anything that would produce a
    non-linear term (x*x, division by x, function calls, etc.) raises
    `_NonLinear` and bubbles up to `parse_toexpr`, which returns `None`.
    """
    if isinstance(node, ast.Constant):
        if isinstance(node.value, bool) or not isinstance(node.value, (int, float)):
            raise _NonLinear
        return (0.0, float(node.value))
    if isinstance(node, ast.Name):
        if node.id == "x":
            return (1.0, 0.0)
        raise _NonLinear
    if isinstance(node, ast.UnaryOp):
        if isinstance(node.op, ast.UAdd):
            return _simplify(node.operand)
        if isinstance(node.op, ast.USub):
            f, o = _simplify(node.operand)
            return (-f, -o)
        raise _NonLinear
    if isinstance(node, ast.BinOp):
        lf, lo = _simplify(node.left)
        rf, ro = _simplify(node.right)
        op = node.op
        if isinstance(op, ast.Add):
            return (lf + rf, lo + ro)
        if isinstance(op, ast.Sub):
            return (lf - rf, lo - ro)
        if isinstance(op, ast.Mult):
            # (lf*x + lo) * (rf*x + ro): the x^2 term is lf*rf, which must be 0
            # for the product to stay linear.
            if lf != 0.0 and rf != 0.0:
                raise _NonLinear
            return (lf * ro + rf * lo, lo * ro)
        if isinstance(op, ast.Div):
            # (lf*x + lo) / (rf*x + ro): linear only when the denominator is a
            # nonzero constant (rf == 0, ro != 0). Anything with x in the
            # denominator is `c/x`, non-linear.
            if rf != 0.0:
                raise _NonLinear
            if ro == 0.0:
                raise _NonLinear
            return (lf / ro, lo / ro)
        raise _NonLinear
    raise _NonLinear


def parse_toexpr(expr: str) -> tuple[float, float] | None:
    """Return (factor, offset) for a linear `toexpr`, or None if non-linear."""
    if expr is None:
        return (1.0, 0.0)
    expr = expr.strip()
    if not expr:
        return (1.0, 0.0)
    expr = _expand_shifts(expr)
    # Any leftover `/*…*/` marker (INVERSE_DIVIDE, AND, etc.) is RomRaider
    # hardware semantics we can't safely simplify.
    if "/*" in expr:
        return None
    try:
        tree = ast.parse(expr, mode="eval")
    except SyntaxError:
        return None
    try:
        return _simplify(tree.body)
    except _NonLinear:
        return None


# Canonical Subaru AFR-enrichment expression: numerator / (1 + x * k).
# Used by primary open-loop fueling tables where raw uint8 0..255 encodes
# an enrichment offset that decodes to AFR via the rational function.
# Merp's canonical ecu_defs.xml writes this as `14.7/(1+x*.0078125)`;
# defgen recognizes the form, extracts the constants, and emits a
# `formula = "subaru_afr_enrichment"` scaling that the C++ loader
# evaluates exactly. Without this match, the expression would fall
# through `parse_toexpr` as non-linear and get flattened to identity.
#
# Match shape: tolerant of four Subaru AFR-enrichment variants in
# Merp's ecu_defs.xml:
#   `N/(1+x*K)`        — canonical (e.g. 14.7/(1+x*.0078125))
#   `N/(1+(x*K))`      — parens around the x*K product
#   `N/(1+(x)*K)`      — parens around x only
#   `N/(1+x)`          — implicit k=1 (no *K factor)
# All reduce to value = N / (1 + raw*k). When *K is absent, k=1.
# Optional close-parens both before and after the `*K` group absorb the
# variant surface syntaxes uniformly.
_AFR_ENRICHMENT_RE = re.compile(
    r"""^\s*
        (?P<num>[0-9]+(?:\.[0-9]+)?)        # numerator (e.g. 14.7)
        \s*/\s*
        \(\s*
        1(?:\.0+)?                          # the "1" in "1 + ..."
        \s*\+\s*
        \(?\s*x\s*\)?\s*                    # x with optional surrounding (...)
        (?:                                  # optional *K group:
            \*\s*
            (?P<k>[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)
            \s*\)?\s*                        # K's optional close paren
        )?
        \)\s*$""",                           # outer close paren of `1+...`
    re.VERBOSE)


# Inverse-divide pattern: `N / x` or `N / (x)`. Used by Subaru injector
# flow scaling (expression="2707090/x") and gear-determination thresholds
# (expression="96560.6/x"). Falls through parse_toexpr as non-linear; we
# match it explicitly and emit a `formula = "inverse_divide"` scaling
# that the C++ loader evaluates exactly.
_INVERSE_DIVIDE_RE = re.compile(
    r"""^\s*
        (?P<num>[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)  # numerator
        \s*/\s*
        \(?\s*x\s*\)?                                      # x or (x)
        \s*$""",
    re.VERBOSE)


def match_inverse_divide(expr: str) -> float | None:
    """Return `numerator` if `expr` matches the form `N/x` or `N/(x)`,
    else None. Used to recognize Subaru's injector-flow / gear-threshold
    reciprocal scalings."""
    if expr is None:
        return None
    expr = expr.strip()
    if not expr:
        return None
    m = _INVERSE_DIVIDE_RE.match(expr)
    if m is None:
        return None
    try:
        return float(m.group("num"))
    except ValueError:
        return None


def match_afr_enrichment(expr: str) -> tuple[float, float] | None:
    """Return (numerator, k) if `expr` matches the Subaru AFR-enrichment
    form `N/(1+x*K)`, else None. The match is structural, not numeric —
    the function does not check that the constants are physically
    plausible; that's the caller's concern."""
    if expr is None:
        return None
    expr = expr.strip()
    if not expr:
        return None
    m = _AFR_ENRICHMENT_RE.match(expr)
    if m is None:
        return None
    try:
        numerator = float(m.group("num"))
        k_str = m.group("k")
        # k absent (the `14.7/(1+x)` form) → implicit k=1.
        k = float(k_str) if k_str is not None else 1.0
    except ValueError:
        return None
    return (numerator, k)


# ---------------------------------------------------------------------------
# Emissions-relevance heuristic
# ---------------------------------------------------------------------------
#
# `Table.emissions_relevant` drives the EmissionsLinter behaviour described
# in `docs/06-legal-ethics.md`. RomRaider's `category=` attribute already
# groups tables by function, so we pattern-match against that. Categories
# we mark relevant cover the bits that emissions regulators actually care
# about: DTC behaviour (suppressing codes), OBD-II readiness, closed-loop
# fueling target / lambda learning. We deliberately do NOT flag open-loop
# fueling, knock control, boost target, ignition timing, etc. — those
# affect emissions indirectly but are core tuning surface; flagging them
# would make the badge meaningless.

_EMISSIONS_CATEGORY_KEYWORDS = (
    "obd-ii",
    "obd ii",
    "closed loop",
    "cl/ol",
    "af correction",
    "af learning",
    "a/f learning",
    "a/f correction",
    "catalyst",
    "egr",
    "evap",
    "o2 sensor",
    "lambda",
    "readiness",
    "emission",
)
# We deliberately do NOT match `diagnostic trouble codes` here. DTC editing
# is a separate concern from "edits that change measurable emissions
# output" — a future `obd_inspection_relevant` flag is the right home for
# that. Folding DTCs in would push the badge to ~55% of tables and drown
# out the genuinely-emissions edits.


def is_emissions_relevant(category: str) -> bool:
    """Heuristic match against the keyword set above. Case-insensitive,
    substring match — RomRaider categories like 'Fueling - Closed Loop'
    and 'Fueling - CL/OL Transition' both hit."""
    if not category:
        return False
    c = category.lower()
    return any(kw in c for kw in _EMISSIONS_CATEGORY_KEYWORDS)


# ---------------------------------------------------------------------------
# Engine-safety heuristic
# ---------------------------------------------------------------------------
#
# `Table.engine_safety_critical` is the other policy flag (see docs/06).
# Edits that would let the engine destroy itself if cranked the wrong way
# — over-rev, over-boost, knock retard reductions, checksum-corruption —
# fall here. The policy module blocks flashing these in EVERY profile,
# so be conservative: false-positives mean a legitimate edit gets
# refused; false-negatives mean a dangerous edit gets through unflagged.
#
# Categories we mark safety-critical:
#   - "knock control"  : all feedback/fine-correction tables. A bad edit
#                        here disables knock protection — engine damage
#                        on the first detonation event.
#   - " - limits"      : both `boost control - limits` and
#                        `miscellaneous - limits` (rev limit, fuel cut,
#                        speed limit, EGT failsafe). Cranking these
#                        relaxes the protections that keep parts intact.
#   - "checksum"       : CRC fixups. A wrong byte here bricks the ECU.

_ENGINE_SAFETY_CATEGORY_KEYWORDS = (
    "knock control",
    " - limits",
    "checksum",
)


def is_engine_safety_critical(category: str) -> bool:
    """Heuristic match against the safety-keyword set. Substring match,
    case-insensitive."""
    if not category:
        return False
    c = category.lower()
    return any(kw in c for kw in _ENGINE_SAFETY_CATEGORY_KEYWORDS)


# ---------------------------------------------------------------------------
# XML extraction
# ---------------------------------------------------------------------------

@dataclass
class ScalingRecord:
    id: str
    factor: float = 1.0
    offset: float = 0.0
    unit: str = ""
    # min/max default to None when the source XML didn't carry them. The
    # C++ loader tolerates an absent key (defaults to 0.0); emitting the
    # explicit 0.0 sentinel made every Merp scaling look like it had a
    # 0..0 range, which is indistinguishable from a real deadband.
    minimum: float | None = None
    maximum: float | None = None
    precision: int = 0
    data_type: str = "uint16_be"
    # `formula` is "linear" by default; piecewise is hand-edited; defgen
    # emits named non-linear formulas when source XML expressions match:
    #   * "subaru_afr_enrichment"  for `14.7/(1+x*K)` patterns (uses numerator + k).
    #   * "inverse_divide"         for `N/x` patterns (uses numerator only).
    formula: str = "linear"
    numerator: float | None = None
    k: float | None = None

    def to_toml(self) -> str:
        if self.formula == "subaru_afr_enrichment":
            return _emit_table("[[scaling]]", {
                "id":        self.id,
                "formula":   self.formula,
                "numerator": self.numerator,
                "k":         self.k,
                "unit":      self.unit,
                "min":       self.minimum,
                "max":       self.maximum,
                "precision": self.precision,
                "data_type": self.data_type,
            })
        if self.formula == "inverse_divide":
            return _emit_table("[[scaling]]", {
                "id":        self.id,
                "formula":   self.formula,
                "numerator": self.numerator,
                "unit":      self.unit,
                "min":       self.minimum,
                "max":       self.maximum,
                "precision": self.precision,
                "data_type": self.data_type,
            })
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


# CID prefixes that defgen emits with cid_scan=true by default.
# Per docs/11 §"Scan mode", FA-DIT WRX firmware (LF*/LV*/LH*/AF*/AE*
# families) embeds the CID descriptor at a per-firmware variable offset
# — hardcoded cid_address values from the source XML are wrong for
# every dump that wasn't the exact one the XML was authored against
# (including any bootloader-stripped / full-2MB variation). Scan mode
# makes the loader find the descriptor wherever it lives.
_CID_SCAN_PREFIXES: tuple[str, ...] = ("LF", "LV", "LH", "AF", "AE")


def _should_default_scan(cid_match: str) -> bool:
    s = cid_match.strip().upper()
    return any(s.startswith(p) for p in _CID_SCAN_PREFIXES)


@dataclass
class IdentificationRecord:
    name: str
    cid_address: int
    cid_length: int
    cid_match: str
    ecu_part: str = ""
    cid_scan: bool = False

    def to_toml(self) -> str:
        # In scan mode, cid_address is ignored by the loader (docs/11
        # §"Scan mode"). Omit it from the emitted TOML so it's not
        # misleading + so a future re-import / hand-tune sees only the
        # fields the loader actually consults.
        body: dict[str, str | int] = {"name": self.name}
        if self.cid_scan:
            body["cid_scan"]  = True
            body["cid_match"] = self.cid_match
        else:
            body["cid_address"] = _hex(self.cid_address)
            body["cid_length"]  = self.cid_length
            body["cid_match"]   = self.cid_match
        body["ecu_part"] = self.ecu_part
        return _emit_table("[[identification]]", body)


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
    # Per-pack diagnostics produced during parsing. Each entry is a
    # (kind, record_id, detail) triple, e.g.
    # ("scaling", "rpm_x100", "non-linear toexpr flattened to identity: ...").
    # Surfaced to stderr by main(); apply-to-pack filters this list to
    # only mention warnings tied to records that were actually added.
    warnings: list[tuple[str, str, str]] = field(default_factory=list)


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
    # Skip fields whose value is None — this is how callers express "the
    # source didn't carry this field, leave it out of the TOML so the
    # loader's default kicks in" (e.g. absent min/max on a scaling).
    present = {k: v for k, v in fields.items() if v is not None}
    lines = [header]
    key_width = max((len(k) for k in present), default=0)
    for k, v in present.items():
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


def _parse_hex_address(s: str | None) -> int:
    """Parse a hex address as written in RomRaider XML.

    RomRaider uses two conventions: `address="0x1234"` (attribute form, with
    0x prefix) and `<internalidaddress>CC176</internalidaddress>` (element
    form, bare hex). Both are hex; the bare form is the source of the
    "CC176" -> ValueError bug when blindly fed through `int()`.
    """
    if s is None or s == "":
        return 0
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 16)


# Default ROM size when the source XML doesn't declare <filesize>.
# Subaru: 1MB is by far the most common cal-ROM size across the
# 2002-2015 fleet (5xx-era and earlier are 512KB; LF7x/LF9x DIT
# generations are 2-4MB). Falling back to 1MB is right for the
# majority and surfaces a clean size mismatch at pack-load time
# for the minority (better than the previous default of 0, which
# breaks every range-check silently).
_DEFAULT_ROM_SIZE_BYTES = 1_048_576


def _parse_filesize(s: str | None) -> int:
    """Accepts '1.5MB', '1572864', '1MB', etc.

    When the XML omits <filesize> entirely (string None/empty) or
    contains an unparseable value, returns the Subaru-default of 1MB
    rather than 0. A 0-size pack still loads but breaks every
    size-aware code path silently — see commits 0008695 / 085450a /
    7fa4beb for the regression pattern this default prevents.
    """
    if s is None or s == "":
        return _DEFAULT_ROM_SIZE_BYTES
    s = s.strip().upper()
    m = re.match(r"([0-9.]+)\s*(KB|MB)?", s)
    if not m:
        return _DEFAULT_ROM_SIZE_BYTES
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

    # Inheriting <rom>s in RomRaider XML write a sparse <table> that ONLY
    # carries the attributes that differ from the base (typically just
    # `name` + `storageaddress`). The base table holds type / storagetype /
    # endian / sizex / sizey / inline <scaling> / nested axis <table>s. So
    # for each child table that names a base table, we overlay attrs +
    # children rather than replacing — the previous behaviour dropped
    # storageaddress on the floor because the child's bare <table> had no
    # type / storagetype to fall back on.
    parent_tables   = {(t.get("name") or ""): t for t in parent.findall("table")}
    parent_scalings = {(s.get("name") or ""): s for s in parent.findall("scaling")}

    child_table_by_name   = {(t.get("name") or ""): t for t in rom.findall("table")}
    child_scaling_by_name = {(s.get("name") or ""): s for s in rom.findall("scaling")}

    # Tables: parent's first (with child overlays), then any child-only ones.
    for name, base_el in parent_tables.items():
        if name in child_table_by_name:
            merged.append(_overlay_table(base_el, child_table_by_name[name]))
        else:
            merged.append(base_el)
    for name, child_el in child_table_by_name.items():
        if name not in parent_tables:
            merged.append(child_el)

    # Scalings: same overlay pattern.
    for name, base_el in parent_scalings.items():
        if name in child_scaling_by_name:
            merged.append(_overlay_scaling(base_el, child_scaling_by_name[name]))
        else:
            merged.append(base_el)
    for name, child_el in child_scaling_by_name.items():
        if name not in parent_scalings:
            merged.append(child_el)
    return merged


def _overlay_table(base: ET.Element, child: ET.Element) -> ET.Element:
    """Overlay child's attrs + children onto base, returning a fresh element.

    Child attributes override base. For children, axis sub-tables are matched
    by `type` (X Axis / Y Axis / Static X Axis / Static Y Axis); other child
    elements (scaling, description) are matched by tag.
    """
    merged = ET.Element(base.tag, base.attrib)
    for k, v in child.attrib.items():
        merged.set(k, v)

    def key(c: ET.Element) -> tuple:
        if c.tag == "table":
            return ("table", (c.get("type") or "").strip().lower())
        return (c.tag,)

    base_children  = {key(c): c for c in base}
    child_children = {key(c): c for c in child}
    for k, base_c in base_children.items():
        if k in child_children:
            child_c = child_children[k]
            if base_c.tag == "table":
                merged.append(_overlay_table(base_c, child_c))
            else:
                # scaling / description / anything else: child wins wholesale.
                merged.append(child_c)
        else:
            merged.append(base_c)
    for k, child_c in child_children.items():
        if k not in base_children:
            merged.append(child_c)
    return merged


def _overlay_scaling(base: ET.Element, child: ET.Element) -> ET.Element:
    """Overlay child's attrs onto base. Scalings rarely carry children."""
    merged = ET.Element(base.tag, base.attrib)
    for k, v in child.attrib.items():
        merged.set(k, v)
    for c in base:
        merged.append(c)
    for c in child:
        merged.append(c)
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
    cid_addr = _parse_hex_address(romid.findtext("internalidaddress"))
    cid_str  = romid.findtext("internalidstring") or ""
    if cid_str.strip():
        # FA-DIT WRX family CIDs live at variable per-firmware offsets;
        # default to scan mode so the loader finds them anywhere in the
        # ROM. cid_address from the XML is preserved in the record (we
        # still pass it through) but to_toml omits it under scan mode.
        pack.identifications.append(IdentificationRecord(
            name=xmlid,
            cid_address=cid_addr,
            cid_length=len(cid_str),
            cid_match=cid_str,
            ecu_part=(romid.findtext("ecuid") or "").strip(),
            cid_scan=_should_default_scan(cid_str),
        ))

    # Top-level scalings (often outside any table)
    seen_scaling_ids: set[str] = set()
    for s_el in rom.findall("scaling"):
        rec = _scaling_from_element(s_el, pack)
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


def _scaling_from_element(el: ET.Element,
                          pack: "Pack | None" = None,
                          fallback_storagetype: str | None = None,
                          fallback_endian: str | None = None) -> ScalingRecord | None:
    name = (el.get("name") or "").strip()
    # Merp's canonical ecu_defs.xml puts storagetype/endian on the parent
    # <table> element and leaves the inline <scaling> child without them.
    # Without the fallback, defgen would default to uint16_be at line ~874
    # below and pass that back up to _extract_table, which then inherits
    # it as the table's data_type — silently overriding the parent's real
    # storagetype. Caller (_extract_table) passes the parent's attributes
    # so the scaling can honor them when its own element omits them.
    storagetype = el.get("storagetype") or fallback_storagetype
    endian = el.get("endian") or fallback_endian
    # Merp's ecu_defs.xml declares endian="little" on float values but
    # the actual ROM bytes are big-endian (decoding as little-endian
    # yields nonsense like 1e35 g/s for what should be a 0..500 g/s
    # MAF scaling). The float-axis quirk is documented in _axis_from_element;
    # apply the same override here so non-axis scalings (e.g. MAF sensor
    # scaling tables that aren't an axis but ARE a float row) decode
    # correctly. No legitimate little-endian-float Subaru data exists,
    # so the override is safe.
    if storagetype and storagetype.lower().strip() == "float":
        endian = "big"
    # RomRaider's canonical attribute is `expression`; some fixtures use
    # `toexpr`. Accept either, expression winning.
    toexpr = (el.get("expression") or el.get("toexpr") or "x").strip()
    # Try the Subaru AFR-enrichment matcher first — non-linear but a
    # named formula type the loader knows how to evaluate exactly.
    afr_match = match_afr_enrichment(toexpr)
    # Then try the inverse-divide matcher (N/x form, used by injector
    # flow scaling and gear-determination thresholds).
    inv_match = match_inverse_divide(toexpr) if afr_match is None else None
    parsed = (None if (afr_match is not None or inv_match is not None)
              else parse_toexpr(toexpr))
    # Inline scalings inside <table> typically have no name= attribute; only
    # `units` + `expression`. Synthesise a stable id from those so identical
    # inline scalings dedup across tables that share them, and so the table
    # can reference the resulting record by id.
    if not name:
        units = (el.get("units") or "").strip()
        if units or toexpr != "x":
            name = f"{units} {toexpr}".strip() or "identity"
        else:
            return None
    slug = _slugify(name)
    afr_formula: tuple[float, float] | None = afr_match
    inv_formula: float | None = inv_match
    if afr_formula is not None or inv_formula is not None:
        # Constants captured; formula type set below at record build.
        factor, offset = (1.0, 0.0)
    elif parsed is None:
        # Non-linear toexpr; emit a linear identity and let the user replace
        # the formula by hand. Better than dropping the scaling entirely.
        factor, offset = (1.0, 0.0)
        if pack is not None:
            pack.warnings.append((
                "scaling", slug,
                f"non-linear toexpr flattened to identity: {toexpr.strip()!r}",
            ))
    else:
        factor, offset = parsed

    try:
        data_type = map_data_type(storagetype, endian)
    except ValueError:
        data_type = "uint16_be"
        if pack is not None:
            pack.warnings.append((
                "scaling", slug,
                f"unknown storage/endian "
                f"(storagetype={storagetype!r} endian={endian!r}); "
                f"defaulted to uint16_be",
            ))

    # `raw_min or None` collapses both `None` (attr absent) and `""`
    # (attr present but empty, which the old code swallowed via `or 0.0`)
    # into a single "omit it" signal — anything else gets parsed as float.
    raw_min = el.get("min") or None
    raw_max = el.get("max") or None
    rec = ScalingRecord(
        id=slug,
        factor=factor,
        offset=offset,
        unit=(el.get("units") or "").strip(),
        minimum=float(raw_min) if raw_min is not None else None,
        maximum=float(raw_max) if raw_max is not None else None,
        precision=_format_to_precision(el.get("format")),
        data_type=data_type,
    )
    if afr_formula is not None:
        rec.formula = "subaru_afr_enrichment"
        rec.numerator, rec.k = afr_formula
    elif inv_formula is not None:
        rec.formula = "inverse_divide"
        rec.numerator = inv_formula
    return rec


def _format_to_precision(fmt: str | None) -> int:
    """`0.00` -> 2, `0.000` -> 3, `0` -> 0."""
    if not fmt:
        return 0
    m = re.search(r"\.([0#]+)", fmt)
    return len(m.group(1)) if m else 0


def _table_address(t_el: ET.Element) -> int:
    """RomRaider's canonical attribute is `storageaddress`; some synthetic
    fixtures use `address`. Accept both, with `storageaddress` winning.

    Always hex — most XMLs prefix with `0x` (e.g. `address="0x000DC938"`)
    but some community-sourced or per-aid extracted XMLs ship bare hex
    without the prefix (e.g. `address="dc938"` in
    `aidx_aid43764_EP5G600A.xml`). `_parse_int` would interpret the
    bare form as decimal and crash on hex digits A-F; `_parse_hex_address`
    handles both prefix styles consistently."""
    return _parse_hex_address(t_el.get("storageaddress") or t_el.get("address"))


def _extract_table(t_el: ET.Element, pack: Pack, seen_scaling_ids: set[str]) -> None:
    name = (t_el.get("name") or "").strip()
    if not name:
        return
    # A table without a storageaddress (after inheritance merge) is a base
    # entry that this specific ROM doesn't expose. Emitting it would put a
    # phantom record at offset 0 in the pack, where the C++ side would read
    # garbage. RomRaider's effective semantic is the same: no storageaddress
    # → not applicable to this CID.
    if t_el.get("storageaddress") is None and t_el.get("address") is None:
        return
    ttype = (t_el.get("type") or "").strip()
    address = _table_address(t_el)

    # Dimensions — RomRaider counts the value dimension, so:
    #   "1D" = single scalar, no axes
    #   "2D" = 1-axis lookup
    #   "3D" = 2-axis lookup
    if ttype.endswith("3D") or ttype == "3D":
        dims = 2
    elif ttype.endswith("2D") or ttype == "2D":
        dims = 1
    elif ttype.endswith("1D") or ttype == "1D":
        dims = 0
    else:
        dims = 1

    # Inline scaling. Pass the parent table's storagetype/endian through
    # so an inline <scaling> without its own storagetype inherits from
    # the table — Merp's canonical XML convention.
    inline_scaling = t_el.find("scaling")
    scaling_id = ""
    data_type = "uint16_be"
    if inline_scaling is not None:
        rec = _scaling_from_element(inline_scaling, pack,
                                    fallback_storagetype=t_el.get("storagetype"),
                                    fallback_endian=t_el.get("endian"))
        if rec is not None:
            scaling_id = rec.id
            data_type = rec.data_type
            if rec.id not in seen_scaling_ids:
                pack.scalings.append(rec)
                seen_scaling_ids.add(rec.id)

    # Axes (nested <table type="X Axis"/> and <table type="Y Axis"/>).
    # In Merp's canonical ecu_defs.xml, axis length lives on the PARENT
    # table's `sizex=` / `sizey=` attribute, not on the axis element. The
    # VA/VB scrubber, by contrast, puts `size=N` on the axis itself. Pass
    # the parent's sizex/sizey through so `_axis_from_element` can fall
    # back to them when the axis carries no length of its own.
    parent_sizex = t_el.get("sizex")
    parent_sizey = t_el.get("sizey")
    axis_x_id = ""
    axis_y_id = ""
    for child in t_el.findall("table"):
        child_type = (child.get("type") or "").strip().lower()
        if child_type not in ("x axis", "y axis", "static y axis", "static x axis"):
            continue
        fallback = (parent_sizex
                    if child_type in ("x axis", "static x axis")
                    else parent_sizey)
        axis = _axis_from_element(child, fallback_length=fallback)
        if axis is None:
            continue
        # Register the axis, disambiguating slug collisions where the
        # content differs (e.g. two source-XML "RPM" axes with different
        # row counts). See _find_or_register_axis for the policy.
        registered_id, was_added = _find_or_register_axis(pack, axis)
        if was_added:
            # Also capture the axis's inline scaling if present.
            # Same storagetype-inheritance rule as the table case above.
            axis_scaling_el = child.find("scaling")
            if axis_scaling_el is not None:
                rec = _scaling_from_element(axis_scaling_el, pack,
                                            fallback_storagetype=child.get("storagetype"),
                                            fallback_endian=child.get("endian"))
                if rec is not None and rec.id not in seen_scaling_ids:
                    pack.scalings.append(rec)
                    seen_scaling_ids.add(rec.id)
        if child_type in ("x axis", "static x axis"):
            axis_x_id = registered_id
        elif child_type in ("y axis", "static y axis"):
            axis_y_id = registered_id

    # RomRaider's 2D (= our 1D) tables put the single axis on `Y Axis`. Our
    # schema needs it under `axis_x` for dimensions=1, so demote.
    if dims == 1 and not axis_x_id and axis_y_id:
        axis_x_id, axis_y_id = axis_y_id, ""

    # A non-scalar table with no resolvable axis storage means the source XML
    # named an axis but didn't (after inheritance flatten) supply its
    # storageaddress. Demote to a scalar so the pack still validates; the
    # warning lets the user spot the demotion and hand-edit if the axis is
    # actually present at a known address.
    table_slug = _slugify(name)
    if dims >= 1 and not axis_x_id:
        pack.warnings.append((
            "table", table_slug,
            f"RomRaider {ttype} table has no resolvable axis storage; "
            f"demoted to scalar (dimensions=0)",
        ))
        dims = 0
        axis_y_id = ""
    elif dims >= 2 and not axis_y_id:
        pack.warnings.append((
            "table", table_slug,
            f"RomRaider {ttype} table has no resolvable Y axis storage; "
            f"demoted to 1D (dimensions=1)",
        ))
        dims = 1

    # Category resolution. RomRaider has two equivalent conventions:
    #   (a) explicit `<table category="Boost Control - Target">` attribute
    #   (b) hierarchy encoded in the name itself: "Boost Control - Target -
    #       Boost Compensation (ECT)" — segments split on " - ", leaf is
    #       the actual table name, the prefix is the category path.
    # When (a) is missing but (b) is present we derive category from the
    # name and trim the display name to just the leaf so the GUI tree
    # doesn't show the category path repeated in every row.
    display_name = _display_name(name, table_slug)
    category = (t_el.get("category") or "").strip().lower()
    if not category and " - " in display_name:
        parts = [p.strip() for p in display_name.split(" - ")]
        if len(parts) >= 2 and all(parts):
            category = " - ".join(parts[:-1]).lower()
            display_name = parts[-1]
    table = TableRecord(
        id=table_slug,
        name=display_name,
        category=category,
        dimensions=dims,
        address=address,
        data_type=data_type,
        scaling=scaling_id,
        axis_x=axis_x_id,
        axis_y=axis_y_id,
        emissions_relevant=is_emissions_relevant(category),
        engine_safety_critical=is_engine_safety_critical(category),
    )
    pack.tables.append(table)


def _find_or_register_axis(pack: "PackBuilder",
                           axis: AxisRecord) -> tuple[str, bool]:
    """Add `axis` to `pack.axes` if not already present.

    Returns `(registered_id, was_added)`.

    Dedup policy:
      - No axis with the same id exists → append; return its id, True.
      - An axis with the same id AND the same (length, address,
        data_type, scaling) exists → reuse; return its id, False.
      - An axis with the same id but DIFFERENT content exists →
        disambiguate by appending `_len{length}` to the id and try
        again. If that still conflicts, append `_v{n}` until unique.

    The disambiguation matters because RomRaider source XMLs often
    have multiple inline axes named "RPM" (one per table that uses an
    RPM axis), with different lengths, addresses, or storage types.
    The previous behaviour (dedup by slug alone) silently dropped all
    but the first such axis and pointed every later table at the wrong
    one — visible in dumps as tables that read fewer rows/cols than
    the source XML specifies, with the trailing bytes invisible. The
    pathological example on lf79103p.toml was a 20-row Y-axis being
    truncated to an 8-row axis, hiding ~24% of every affected table
    from the dumper.
    """
    def content_match(a: AxisRecord, b: AxisRecord) -> bool:
        return (a.length    == b.length
            and a.address   == b.address
            and a.data_type == b.data_type
            and a.scaling   == b.scaling)

    # Fast path: no id collision.
    if not any(existing.id == axis.id for existing in pack.axes):
        pack.axes.append(axis)
        return axis.id, True

    # An axis with this id already exists.
    for existing in pack.axes:
        if existing.id == axis.id and content_match(existing, axis):
            return existing.id, False

    # Slug collision with different content. Try `_len{N}` first.
    candidates = [f"{axis.id}_len{axis.length}"]
    for n in range(2, 100):
        candidates.append(f"{axis.id}_len{axis.length}_v{n}")
    for candidate in candidates:
        match = next((a for a in pack.axes if a.id == candidate), None)
        if match is None:
            new_axis = replace(axis, id=candidate)
            pack.axes.append(new_axis)
            return candidate, True
        if content_match(match, axis):
            return candidate, False
    # Genuinely should never happen — 99 disambiguations is plenty.
    raise RuntimeError(
        f"could not disambiguate axis id '{axis.id}' after 99 attempts")


def _axis_from_element(el: ET.Element,
                       fallback_length: str | None = None) -> AxisRecord | None:
    name = (el.get("name") or "").strip()
    if not name:
        return None
    # See _extract_table — drop dynamic axes that this ROM doesn't override.
    # Static axes carry their values literally via <data> children, so they
    # legitimately have no storageaddress.
    ttype = (el.get("type") or "").strip().lower()
    is_static = ttype.startswith("static")
    if not is_static \
            and el.get("storageaddress") is None and el.get("address") is None:
        return None
    # RomRaider's canonical attribute is `size`; some fixtures use `elements`.
    # Merp's `ecu_defs.xml` does neither — axis length lives on the PARENT
    # table's `sizex=` / `sizey=`. Caller in `_extract_table` passes the
    # parent's attribute through `fallback_length` so we can use it when the
    # axis element carries no length of its own.
    length_str = el.get("size") or el.get("elements") or fallback_length
    inline_scaling = el.find("scaling")

    # RomRaider's axis storagetype + endian live on the axis <table> element
    # itself (e.g. `<table type="X Axis" storagetype="float" endian="little">`),
    # not on its inner <scaling>. The previous code only sourced data_type
    # from the inline scaling, which meant float axes silently became
    # `uint16_be`. Read the axis attrs first; fall through to the inline
    # scaling's data_type only if the element omits storagetype.
    #
    # Empirical note on Subaru float axes: Merp's ecu_defs.xml declares
    # `endian="little"` on float axes, but the bytes on SH-2A ROMs are
    # actually big-endian (decoding as little-endian yields all-zeros for
    # plausible torque/RPM values). Treat axis endianness as a fact-source
    # signal but override to big when the element type is `float` — the
    # XML attribute is a documented RR quirk, not the actual storage.
    storagetype = el.get("storagetype")
    endian      = el.get("endian")
    if storagetype:
        if storagetype.lower().strip() == "float":
            endian = "big"  # See note above re: Merp/Subaru float-axis quirk
        try:
            axis_data_type = map_data_type(storagetype, endian)
        except ValueError:
            axis_data_type = None
    else:
        axis_data_type = None

    data_type = axis_data_type or "uint16_be"
    scaling_id = ""
    if inline_scaling is not None:
        rec = _scaling_from_element(inline_scaling,
                                    fallback_storagetype=el.get("storagetype"),
                                    fallback_endian=el.get("endian"))
        if rec is not None:
            if axis_data_type is None:
                # No axis-level storagetype to override the scaling's
                # default — fall back to whatever the scaling carries.
                data_type = rec.data_type
            scaling_id = rec.id

    axis_slug = _slugify(name)
    return AxisRecord(
        id=axis_slug,
        name=_display_name(name, axis_slug),
        unit="",  # we don't carry the axis's unit text from XML
        type="static",
        address=_table_address(el),
        length=int(length_str) if length_str and length_str.isdigit() else 0,
        data_type=data_type,
        scaling=scaling_id,
    )


def _is_factual_name(name: str) -> bool:
    """Allow short, technical-looking names; strip flowery descriptions.

    The clean-room rule is to leave descriptive text out. Identifier-like
    names (e.g. "Boost Target", "Airflow - Turbo - Wastegate - Wastegate
    Duty Maximum") are factual table tags used industry-wide; we keep
    them. Long sentence-like prose with punctuation we drop.

    The 256-char cap exists to catch genuine OEM description text that
    occasionally leaks into the `name` attribute. Real RomRaider names
    rarely exceed 100 chars; the cap leaves headroom for the longer
    dashed-hierarchy names without admitting paragraph-shaped prose.
    """
    return len(name) <= 256 and "." not in name and "," not in name


def _display_name(name: str, slug: str) -> str:
    """Pick a readable display name for a table/axis record.

    If the OEM-provided `name` passes the clean-room filter we keep it. Else
    we fall back to the slug with underscores → spaces and Title-Case so
    `pack-info`/`table-list` show something readable instead of an empty
    string. The slug is already a defgen-derived identifier (lowercased,
    punctuation-stripped) — title-casing it doesn't republish OEM prose,
    just presents the same factual content in human-readable form.
    """
    if _is_factual_name(name):
        return name
    return slug.replace("_", " ").title() if slug else ""


# ---------------------------------------------------------------------------
# Incremental merge into an existing pack
# ---------------------------------------------------------------------------
#
# `--apply-to-pack` is an additive merge: we read an existing single-file
# TOML pack to collect the ids already present, then append only the new
# scalings / axes / tables / identifications from the XML. The existing
# file is preserved byte-for-byte (no re-emit), so hand-edits, comments,
# and whitespace survive. New records are appended at the end of the
# file in a single block delimited by a banner comment so the user can
# see what came from the merge run.

@dataclass
class _ExistingIds:
    scalings: set[str] = field(default_factory=set)
    axes: set[str] = field(default_factory=set)
    tables: set[str] = field(default_factory=set)
    identifications: set[str] = field(default_factory=set)


def _collect_existing_ids(path: Path) -> _ExistingIds:
    """Parse an existing TOML pack and return the set of record ids."""
    import tomllib
    raw = tomllib.loads(path.read_text(encoding="utf-8"))
    ids = _ExistingIds()
    for s in raw.get("scaling", []) or []:
        if isinstance(s, dict) and "id" in s:
            ids.scalings.add(str(s["id"]))
    for a in raw.get("axis", []) or []:
        if isinstance(a, dict) and "id" in a:
            ids.axes.add(str(a["id"]))
    for t in raw.get("table", []) or []:
        if isinstance(t, dict) and "id" in t:
            ids.tables.add(str(t["id"]))
    for ident in raw.get("identification", []) or []:
        if isinstance(ident, dict) and "name" in ident:
            ids.identifications.add(str(ident["name"]))
    return ids


def _filter_pack_by_existing(pack: Pack, existing: _ExistingIds) -> Pack:
    """Return a copy of `pack` with records whose id matches existing
    dropped. Warnings are filtered to mention only retained records."""
    filtered = Pack(
        rom_id=pack.rom_id,
        display_name=pack.display_name,
        platform=pack.platform,
        transmission=pack.transmission,
        years=list(pack.years),
        rom_size_bytes=pack.rom_size_bytes,
        identifications=[i for i in pack.identifications
                         if i.name not in existing.identifications],
        scalings=[s for s in pack.scalings if s.id not in existing.scalings],
        axes=[a for a in pack.axes if a.id not in existing.axes],
        tables=[t for t in pack.tables if t.id not in existing.tables],
    )
    kept_scalings = {s.id for s in filtered.scalings}
    filtered.warnings = [
        w for w in pack.warnings
        if not (w[0] == "scaling" and w[1] not in kept_scalings)
    ]
    return filtered


def _emit_appendix(pack: Pack) -> str:
    """Emit just the new records (no [pack] header) for appending to an
    existing pack file. Begins with a banner comment naming the run."""
    out: list[str] = []
    out.append("")
    out.append("# --- defgen --apply-to-pack additions ---")
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
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("xml", type=Path, help="Path to a RomRaider XML file")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output TOML path (default: stdout)")
    parser.add_argument("--rom-id",
                        help="Only emit the rom matching this xmlid")
    parser.add_argument("--apply-to-pack", type=Path, metavar="EXISTING.TOML",
                        help="Additive merge: append only records whose id is "
                             "not already present in EXISTING.TOML. Requires "
                             "--rom-id when the XML has multiple roms.")
    parser.add_argument("--axis-corrections", type=Path, nargs="?",
                        const=axis_corrections.DEFAULT_CORRECTIONS,
                        metavar="TSV",
                        help="Override axis lengths from a firmware-derived "
                             "corrections TSV (fixes the halved-dimension XML "
                             "bug). With no path, uses the bundled "
                             "corrections/axis_length_corrections.tsv.")
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

    if args.axis_corrections is not None:
        try:
            corrections = axis_corrections.load_axis_corrections(args.axis_corrections)
        except (OSError, ValueError) as e:
            print(f"defgen: axis corrections: {e}", file=sys.stderr)
            return 1
        for pack in packs:
            changed = axis_corrections.apply_axis_corrections(pack.axes, corrections)
            for addr, old_len, new_len in changed:
                print(f"defgen: axis 0x{addr:X} length {old_len} -> {new_len} "
                      f"(firmware) in {pack.rom_id}", file=sys.stderr)

    if args.apply_to_pack is not None:
        if len(packs) > 1:
            print("defgen: --apply-to-pack requires --rom-id when the XML has "
                  f"multiple roms ({len(packs)} found)", file=sys.stderr)
            return 1
        target = args.apply_to_pack
        if not target.is_file():
            print(f"defgen: --apply-to-pack target is not a file: {target}",
                  file=sys.stderr)
            return 1
        try:
            existing = _collect_existing_ids(target)
        except (OSError, ValueError) as e:
            print(f"defgen: cannot parse existing pack {target}: {e}",
                  file=sys.stderr)
            return 1
        filtered = _filter_pack_by_existing(packs[0], existing)
        added = (len(filtered.identifications)
                 + len(filtered.scalings)
                 + len(filtered.axes)
                 + len(filtered.tables))
        if added == 0:
            print(f"defgen: no new records to merge into {target}",
                  file=sys.stderr)
        else:
            appendix = _emit_appendix(filtered)
            with target.open("a", encoding="utf-8") as f:
                f.write(appendix)
            print(f"defgen: merged {len(filtered.scalings)} scaling(s), "
                  f"{len(filtered.axes)} axis/axes, "
                  f"{len(filtered.tables)} table(s), "
                  f"{len(filtered.identifications)} identification(s) "
                  f"into {target}", file=sys.stderr)
        if filtered.warnings:
            print(f"defgen: {filtered.rom_id}: "
                  f"{len(filtered.warnings)} warning(s):", file=sys.stderr)
            for kind, rec_id, detail in filtered.warnings:
                print(f"  {kind}: {rec_id}: {detail}", file=sys.stderr)
        return 0

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

    for pack in packs:
        if pack.warnings:
            print(f"defgen: {pack.rom_id}: {len(pack.warnings)} warning(s):",
                  file=sys.stderr)
            for kind, rec_id, detail in pack.warnings:
                print(f"  {kind}: {rec_id}: {detail}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
