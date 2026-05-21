#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
descriptors — predicate library describing what a Subaru cal table looks like.

Goal: codify empirical observation of OEM cal-table shapes / value ranges /
axis types so downstream tools can answer:

  "I have some ROM bytes claimed to be table X. Do they actually look like
   table X, or are we mis-addressed?"

  "I have an unidentified byte region of unknown shape. Which descriptors
   could it plausibly satisfy?"

Used (or planned-to-be-used) by:
  - localize.py (--use-descriptors): smarter relocation candidate scoring
    than the current byte-pattern fallback.
  - categorize.py (--use-descriptors): shape/category inference when source
    XMLs didn't carry enough structure.
  - validate.py (future): post-cousin-seed sanity check that named tables
    in a pack actually look like their name claims.

A descriptor is (id, kind, description, id_patterns, predicate, evidence).
Predicates take raw bytes + a DecodeHint and return a Verdict carrying a
match flag, score, and human-readable reasons.

Clean-room posture (docs/15): descriptors codify FACTUAL observation ranges
from publicly-observable ROM bytes, not OEM-proprietary identifiers. A
predicate like "wastegate-duty values lie in [0, 10000] when interpreted
as uint16" is an arithmetic fact about the encoded data, not expression
copied from another tool.
"""

from __future__ import annotations

import fnmatch
import struct
from dataclasses import dataclass, field
from typing import Callable


# ---------------------------------------------------------------------------
# Decode helpers
# ---------------------------------------------------------------------------

# Map from our dtype strings (as used in pack TOML) to (struct fmt, width).
_DTYPE_FMT: dict[str, tuple[str, int]] = {
    "uint8":      (">B", 1),
    "int8":       (">b", 1),
    "uint16_be":  (">H", 2),
    "uint16_le":  ("<H", 2),
    "int16_be":   (">h", 2),
    "int16_le":   ("<h", 2),
    "uint32_be":  (">I", 4),
    "uint32_le":  ("<I", 4),
    "int32_be":   (">i", 4),
    "int32_le":   ("<i", 4),
    "float32_be": (">f", 4),
    "float32_le": ("<f", 4),
}


def decode_values(buf: bytes, dtype: str, count: int) -> list[float]:
    """Decode `count` typed values from the start of `buf`.

    Raises KeyError on unknown dtype, struct.error on truncated buf.
    """
    fmt, width = _DTYPE_FMT[dtype]
    return [float(struct.unpack_from(fmt, buf, i * width)[0]) for i in range(count)]


@dataclass(frozen=True)
class DecodeHint:
    """How to interpret a byte region.

    dims=0 is a scalar; dims=1 is a 1D axis/row of `length` values; dims=2
    is a 2D table of (rows × cols). Higher dims are not modeled.
    """
    dtype: str
    dims: int = 0
    length: int = 0
    rows: int = 0
    cols: int = 0

    def total_cells(self) -> int:
        if self.dims == 0:
            return 1
        if self.dims == 1:
            return self.length
        if self.dims == 2:
            return self.rows * self.cols
        return 0

    def byte_width(self) -> int:
        return _DTYPE_FMT[self.dtype][1]


# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------

@dataclass
class Verdict:
    """Outcome of a descriptor predicate.

    `score` is 0.0-1.0; consumers use it to rank competing descriptors.
    `reasons` carries short human-readable diagnostics for either path.
    """
    matches: bool
    score: float
    reasons: list[str] = field(default_factory=list)

    @classmethod
    def yes(cls, score: float = 1.0, *reasons: str) -> "Verdict":
        return cls(True, score, list(reasons))

    @classmethod
    def no(cls, *reasons: str) -> "Verdict":
        return cls(False, 0.0, list(reasons))


# ---------------------------------------------------------------------------
# Descriptor + Evidence
# ---------------------------------------------------------------------------

@dataclass
class Evidence:
    """A concrete observed table that should satisfy a descriptor.

    Lives alongside the descriptor as documentation + a stable handle for
    future regression tests that load the named pack/ROM out of the corpus.
    `descriptors.py` itself doesn't open these files; consumers do.
    """
    pack_id: str
    table_id: str
    rom_path: str = ""  # optional, relative to repo root


@dataclass
class Descriptor:
    """A predicate describing the expected shape/range of a cal table.

    Typical flow on the consumer side:
      1. Pick candidate descriptors by matching the table id against
         `id_patterns` (fnmatch globs against the lowercased id).
      2. Call `predicate(bytes, hint)` to get a Verdict for each candidate.
      3. Take the highest-scoring matching Verdict, or "unknown shape" if
         none match.
    """
    id: str
    kind: str  # "axis" or "table"
    description: str
    id_patterns: list[str]
    predicate: Callable[[bytes, DecodeHint], Verdict]
    evidence: list[Evidence] = field(default_factory=list)

    def id_matches(self, table_id: str) -> bool:
        lid = table_id.lower()
        return any(fnmatch.fnmatch(lid, p) for p in self.id_patterns)


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

_REGISTRY: list[Descriptor] = []


def register(d: Descriptor) -> Descriptor:
    """Add `d` to the global registry; replaces any prior with the same id."""
    for i, existing in enumerate(_REGISTRY):
        if existing.id == d.id:
            _REGISTRY[i] = d
            return d
    _REGISTRY.append(d)
    return d


def all_descriptors() -> list[Descriptor]:
    return list(_REGISTRY)


def by_id(descriptor_id: str) -> Descriptor | None:
    for d in _REGISTRY:
        if d.id == descriptor_id:
            return d
    return None


def candidates_for(table_id: str, kind: str = "") -> list[Descriptor]:
    """Return descriptors whose patterns match `table_id` (and optional kind)."""
    out: list[Descriptor] = []
    for d in _REGISTRY:
        if kind and d.kind != kind:
            continue
        if d.id_matches(table_id):
            out.append(d)
    return out


# ---------------------------------------------------------------------------
# Predicate building blocks
# ---------------------------------------------------------------------------

def values_in_range(values: list[float], lo: float, hi: float) -> tuple[bool, int]:
    """Return (all-in-range?, count-of-out-of-range)."""
    bad = sum(1 for v in values if v < lo or v > hi)
    return bad == 0, bad


def is_monotonic(values: list[float], strict: bool = True) -> bool:
    if len(values) < 2:
        return True
    if strict:
        return all(b > a for a, b in zip(values, values[1:]))
    return all(b >= a for a, b in zip(values, values[1:]))


def distinct_fraction(values: list[float]) -> float:
    """Fraction of distinct values. Constant array → 0; all-distinct → 1.0."""
    if not values:
        return 0.0
    return len(set(values)) / len(values)


# ---------------------------------------------------------------------------
# Seed descriptors
# ---------------------------------------------------------------------------
# Each descriptor below is hand-curated from observation of real Subaru cal
# tables in our known-good packs (a2tb002c Legacy GT, the bludgod corpus,
# Merp's canonical ecu_defs.xml). Ranges, dtypes, and shape rules are
# empirical facts, not derived from any closed-source tuning tool.

# --- Axes ------------------------------------------------------------------

def _engine_rpm_axis(buf: bytes, hint: DecodeHint) -> Verdict:
    # Engine-speed axes on Subaru DI/EJ ROMs span roughly 400-7500 RPM with
    # 8-21 breakpoints. We allow a slightly wider band (200-8500) for
    # tolerance and require strictly increasing values across the table.
    if hint.dims != 1 or hint.length < 5 or hint.length > 32:
        return Verdict.no(f"length {hint.length} outside [5,32]")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    in_range, bad = values_in_range(vals, 200.0, 8500.0)
    if not in_range:
        return Verdict.no(f"{bad}/{hint.length} values outside [200,8500]")
    if not is_monotonic(vals, strict=True):
        return Verdict.no("not strictly increasing")
    if max(vals) - min(vals) < 1000.0:
        return Verdict.no(f"span {max(vals)-min(vals):.0f} too small (<1000)")
    return Verdict.yes(1.0, f"monotonic {vals[0]:.0f}..{vals[-1]:.0f} RPM ({hint.length} pts)")


ENGINE_RPM_AXIS = register(Descriptor(
    id="engine_rpm_axis",
    kind="axis",
    description="Engine-speed (RPM) lookup axis: monotonic, 200-8500 RPM span, 5-32 pts.",
    id_patterns=[
        "*rpm*", "*engine_speed*", "*engine_rpm*", "*_rpm",
    ],
    predicate=_engine_rpm_axis,
    evidence=[
        Evidence("a2tb002c", "engine_speed"),
    ],
))


def _coolant_temp_axis(buf: bytes, hint: DecodeHint) -> Verdict:
    # Coolant axes are typically -40 to +120 °C, 5-12 breakpoints, scaled
    # signed int8/int16 or unsigned with offset. We check for monotonic
    # values within [-50, +150] (degrees) after type-decode. Strictly
    # increasing — same convention as other axes.
    if hint.dims != 1 or hint.length < 4 or hint.length > 24:
        return Verdict.no(f"length {hint.length} outside [4,24]")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    in_range, bad = values_in_range(vals, -50.0, 150.0)
    if not in_range:
        return Verdict.no(f"{bad}/{hint.length} values outside [-50,150]")
    if not is_monotonic(vals, strict=True):
        return Verdict.no("not strictly increasing")
    return Verdict.yes(1.0, f"monotonic {vals[0]:.0f}..{vals[-1]:.0f} °C ({hint.length} pts)")


COOLANT_TEMP_AXIS = register(Descriptor(
    id="coolant_temp_axis",
    kind="axis",
    description="Coolant/ECT axis: monotonic, -50 to 150 °C, 4-24 pts.",
    id_patterns=[
        "*ect*axis*", "*coolant*temp*axis*", "*coolant*axis*",
        "*ect_axis*", "*ect_x*", "*ect_y*",
    ],
    predicate=_coolant_temp_axis,
    evidence=[
        Evidence("a2tb002c", "coolant_temperature"),
    ],
))


# --- Tables ----------------------------------------------------------------

def _wastegate_duty_table(buf: bytes, hint: DecodeHint) -> Verdict:
    # Wastegate-duty maps encode 0-100% duty cycle in uint16. Subaru ROMs
    # use a few scalings depending on era / sub-platform:
    #   * raw × 0.01    → raw range [0, 10000]   (×100 scaling, EJ-era common)
    #   * raw / 256     → raw range [0, 25600]   (×1/256 scaling, a2tb002c)
    #   * raw / 128     → raw range [0, 12800]   (×1/128 scaling)
    # Accept any value in [0, 27000] to absorb all three with slight margin.
    # Maps are typically 8×8 to 17×17; a genuine cal table has many distinct
    # cell values, so a near-constant region rejects (likely padding).
    if hint.dims != 2 or hint.rows < 4 or hint.cols < 4:
        return Verdict.no(f"shape {hint.rows}x{hint.cols} too small (need 4×4+)")
    if hint.dtype not in ("uint16_be", "uint16_le", "uint8"):
        return Verdict.no(f"dtype {hint.dtype} not uint")
    cells = hint.total_cells()
    try:
        vals = decode_values(buf, hint.dtype, cells)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    in_range, bad = values_in_range(vals, 0.0, 27000.0)
    if not in_range:
        return Verdict.no(f"{bad}/{cells} values outside [0,27000]")
    df = distinct_fraction(vals)
    if df < 0.10:
        return Verdict.no(f"only {df:.0%} distinct (likely flat/padding)")
    return Verdict.yes(1.0, f"{hint.rows}x{hint.cols} in [0,27000], {df:.0%} distinct")


WASTEGATE_DUTY = register(Descriptor(
    id="wastegate_duty",
    kind="table",
    description="Wastegate-duty 2D map: uint16 in [0, 27000] (covers ×100, ×1/128, ×1/256 raw scalings), ≥4×4.",
    id_patterns=[
        "*wastegate*duty*", "*wgduty*", "*wg_duty*",
        "initial_wastegate_duty", "max_wastegate_duty",
    ],
    predicate=_wastegate_duty_table,
    evidence=[
        Evidence("a2tb002c", "max_wastegate_duty"),
        Evidence("a2tb002c", "initial_wastegate_duty"),
    ],
))


def _boost_target_table(buf: bytes, hint: DecodeHint) -> Verdict:
    # Boost-target maps store absolute manifold pressure. Across Subaru
    # eras the raw uint16 is decoded several ways. Map values typically
    # span vacuum (~20 kPa abs, decel-cut cells) up to ~300 kPa absolute
    # (high-boost target). The descriptor tries the common scalings and
    # accepts any that lands the whole table inside 20-350 kPa abs:
    #   * raw           kPa abs (modern DI direct kPa)
    #   * raw / 10      kPa abs
    #   * raw / 128     kPa abs (older EJ)
    #   * raw × 0.1334  kPa abs (EJ psi-derived: factor 0.01934 × 6.895)
    if hint.dims != 2 or hint.rows < 4 or hint.cols < 4:
        return Verdict.no(f"shape {hint.rows}x{hint.cols} too small")
    if hint.dtype not in ("uint16_be", "uint16_le"):
        return Verdict.no(f"dtype {hint.dtype} not uint16")
    cells = hint.total_cells()
    try:
        vals = decode_values(buf, hint.dtype, cells)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    lo, hi = 20.0, 350.0
    candidates = [
        ("raw kPa",        vals),
        ("raw/10 kPa",     [v / 10.0 for v in vals]),
        ("raw/128 kPa",    [v / 128.0 for v in vals]),
        ("raw×0.1334 kPa", [v * 0.1334 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, lo, hi)
        if in_range:
            df = distinct_fraction(scaled)
            if df < 0.10:
                continue
            return Verdict.yes(1.0,
                f"{hint.rows}x{hint.cols} in [{lo:.0f},{hi:.0f}] kPa via {label}, "
                f"{df:.0%} distinct")
    return Verdict.no(f"no scaling places all {cells} cells in {lo:.0f}-{hi:.0f} kPa")


BOOST_TARGET = register(Descriptor(
    id="boost_target",
    kind="table",
    description="Boost-target 2D map: uint16 decodes to 20-350 kPa absolute under some standard scaling.",
    id_patterns=[
        "*target_boost*", "*boost_target*", "*target*boost*main*",
    ],
    predicate=_boost_target_table,
    evidence=[
        Evidence("a2tb002c", "target_boost"),
    ],
))


def _base_timing_table(buf: bytes, hint: DecodeHint) -> Verdict:
    # Base-timing maps are signed values representing degrees BTDC, scaled
    # ×4 in older EJ ROMs (int16/4) or stored as int8 deg-BTDC in newer DI.
    # We accept values whose decoded magnitude lies in [-15, +60] degrees
    # under either scaling. Distinct-cell threshold guards against padding.
    if hint.dims != 2 or hint.rows < 4 or hint.cols < 4:
        return Verdict.no(f"shape {hint.rows}x{hint.cols} too small")
    cells = hint.total_cells()
    try:
        vals = decode_values(buf, hint.dtype, cells)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    candidates = [
        ("raw deg",       vals),
        ("raw/4 deg",     [v / 4.0 for v in vals]),
        ("raw/2 deg",     [v / 2.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, -15.0, 60.0)
        if in_range:
            df = distinct_fraction(scaled)
            if df < 0.10:
                continue
            return Verdict.yes(1.0,
                f"{hint.rows}x{hint.cols} in [-15,60]° via {label}, {df:.0%} distinct")
    return Verdict.no(f"no scaling places all {cells} cells in -15..60°")


BASE_TIMING = register(Descriptor(
    id="base_timing",
    kind="table",
    description="Base-ignition-timing 2D map: signed value decoding to -15..+60 ° BTDC.",
    id_patterns=[
        "*base_timing*", "*ignition*primary*", "*timing*primary*",
    ],
    predicate=_base_timing_table,
    evidence=[
        Evidence("a2tb002c", "base_timing_primary_cruise"),
        Evidence("a2tb002c", "base_timing_primary_non_cruise"),
    ],
))
