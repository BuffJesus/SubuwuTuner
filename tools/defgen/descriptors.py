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
         `id_patterns` (fnmatch globs against the lowercased id) AND
         (when known) by the entry's dimensionality.
      2. Call `predicate(bytes, hint)` to get a Verdict for each candidate.
      3. Take the highest-scoring matching Verdict, or "unknown shape" if
         none match.

    `expected_dims` declares the predicate's intended dimensionality:
    use 1 for axes / 1D tables and 2 for 2D maps. Setting it lets the
    registry skip descriptors that would over-match by id alone — e.g.
    a `*target_boost*` pattern catching a 1D ECT-compensation curve that
    the 2D predicate would then correctly but loudly reject. `None`
    means "applies regardless of dims" (rare).
    """
    id: str
    kind: str  # "axis" or "table"
    description: str
    id_patterns: list[str]
    predicate: Callable[[bytes, DecodeHint], Verdict]
    evidence: list[Evidence] = field(default_factory=list)
    expected_dims: int | None = None

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


def candidates_for(table_id: str, kind: str = "",
                   dims: int | None = None) -> list[Descriptor]:
    """Return descriptors that could apply to an entry.

    Filters by id-pattern match, optional `kind` ("axis" / "table"), and
    optional `dims`. When `dims` is given, descriptors whose
    `expected_dims` is set to a different value are excluded — this
    keeps a 2D-only predicate from matching a 1D entry by id alone.
    Descriptors with `expected_dims=None` pass through regardless.
    """
    out: list[Descriptor] = []
    for d in _REGISTRY:
        if kind and d.kind != kind:
            continue
        if dims is not None and d.expected_dims is not None \
                and d.expected_dims != dims:
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
    # Engine-speed axes on Subaru ROMs span roughly 400-8500 RPM with
    # 8-32 breakpoints. The raw encoding varies by era / table:
    #   * float32_be RPM            (EJ-era Legacy/Impreza, Merp canonical)
    #   * uint16_be raw RPM         (some packs use raw 1:1)
    #   * uint16_be ×5.12  (DI-era; raw_decoded = raw/5.12 → RPM)
    #   * uint16_be ×10    (DI-era timing-compensation axes)
    #   * uint16_be ×4     (some idle / cold-start RPM axes)
    # Bounds extend to 12000 RPM since some compensation tables include
    # rev-limit-and-beyond breakpoints. Accept any candidate that places
    # all values monotonically in the band with a 1000+ RPM total span.
    if hint.dims != 1 or hint.length < 5 or hint.length > 32:
        return Verdict.no(f"length {hint.length} outside [5,32]")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    candidates = [
        ("raw",       vals),
        ("raw/5.12",  [v / 5.12 for v in vals]),
        ("raw/10",    [v / 10.0 for v in vals]),
        ("raw/4",     [v / 4.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, 200.0, 12000.0)
        if not in_range:
            continue
        if not is_monotonic(scaled, strict=True):
            continue
        if max(scaled) - min(scaled) < 1000.0:
            continue
        return Verdict.yes(1.0,
            f"monotonic {scaled[0]:.0f}..{scaled[-1]:.0f} RPM "
            f"({hint.length} pts) via {label}")
    return Verdict.no(f"no scaling places {hint.length} cells in "
                       f"monotonic 200..12000 RPM band")


ENGINE_RPM_AXIS = register(Descriptor(
    id="engine_rpm_axis",
    kind="axis",
    description="Engine-speed (RPM) lookup axis: monotonic, 200-8500 RPM span, 5-32 pts.",
    id_patterns=[
        "*rpm*", "*engine_speed*", "*engine_rpm*",
    ],
    predicate=_engine_rpm_axis,
    evidence=[
        Evidence("a2tb002c", "engine_speed"),
    ],
    expected_dims=1,
))


def _coolant_temp_axis(buf: bytes, hint: DecodeHint) -> Verdict:
    # Coolant axes are typically -40 to +120 °C, 4-16 breakpoints. The
    # raw encoding varies:
    #   * float32 / int8 / int16     decoded value already in °C
    #   * uint16_be / 256             °C (e.g. DI-era 16-point tables)
    #   * uint16_be / 256 - 192       offset-encoded °C
    # Accept any candidate that places all values monotonically in
    # [-50, 150] °C.
    if hint.dims != 1 or hint.length < 4 or hint.length > 24:
        return Verdict.no(f"length {hint.length} outside [4,24]")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    candidates = [
        ("raw",                vals),
        ("raw/256",            [v / 256.0 for v in vals]),
        ("raw/256 - 192",      [v / 256.0 - 192.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, -50.0, 150.0)
        if not in_range:
            continue
        if not is_monotonic(scaled, strict=True):
            continue
        return Verdict.yes(1.0,
            f"monotonic {scaled[0]:.0f}..{scaled[-1]:.0f} °C "
            f"({hint.length} pts) via {label}")
    return Verdict.no(
        f"no scaling places {hint.length} cells in monotonic -50..150 °C")


COOLANT_TEMP_AXIS = register(Descriptor(
    id="coolant_temp_axis",
    kind="axis",
    description="Coolant/ECT axis: monotonic, -50 to 150 °C, 4-24 pts.",
    id_patterns=[
        "*ect*axis*", "*coolant*temp*axis*", "*coolant*axis*",
        "*coolant_temperature*", "*ect_x*", "*ect_y*",
    ],
    predicate=_coolant_temp_axis,
    evidence=[
        Evidence("a2tb002c", "coolant_temperature"),
    ],
    expected_dims=1,
))


def _intake_temp_axis(buf: bytes, hint: DecodeHint) -> Verdict:
    # IAT axes share the same physical band as coolant (-40 to +120 °C
    # typical, with margin -50 to +150) and the same encoding variants.
    # Split descriptor so id_patterns don't conflate IAT with coolant.
    if hint.dims != 1 or hint.length < 3 or hint.length > 24:
        return Verdict.no(f"length {hint.length} outside [3,24]")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    candidates = [
        ("raw",                vals),
        ("raw/256",            [v / 256.0 for v in vals]),
        ("raw/256 - 192",      [v / 256.0 - 192.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, -50.0, 150.0)
        if not in_range:
            continue
        if not is_monotonic(scaled, strict=True):
            continue
        return Verdict.yes(1.0,
            f"monotonic {scaled[0]:.0f}..{scaled[-1]:.0f} °C "
            f"({hint.length} pts) via {label}")
    return Verdict.no(
        f"no scaling places {hint.length} cells in monotonic -50..150 °C")


INTAKE_TEMP_AXIS = register(Descriptor(
    id="intake_temp_axis",
    kind="axis",
    description="Intake-air-temperature axis: monotonic, -50 to 150 °C, 3-24 pts.",
    id_patterns=[
        "*intake_temperature*", "*intake_air_temperature*",
        "*iat*axis*", "*iat_x*", "*iat_y*",
    ],
    predicate=_intake_temp_axis,
    evidence=[
        Evidence("a2tb002c", "intake_temperature"),
    ],
    expected_dims=1,
))


def _engine_load_axis(buf: bytes, hint: DecodeHint) -> Verdict:
    # Engine-load axes come in two physical flavours on Subaru:
    #   * "g/rev"  — mass-per-revolution, typical 0.2..5.0 g/rev (EJ-era
    #               float32, DI-era uint16 ×256 or ×2560).
    #   * "calc-load %" — normalized to a reference (FA-DIT specifically),
    #               range 0..400 % with most cells in 0..300 % via raw/256.
    # Try both physical interpretations across the common raw scalings.
    # Accept the first that comes out monotonic in its range.
    if hint.dims != 1 or hint.length < 4 or hint.length > 32:
        return Verdict.no(f"length {hint.length} outside [4,32]")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    grev_candidates = [
        ("raw g/rev",          vals),
        ("raw/2560 g/rev",     [v / 2560.0 for v in vals]),
        ("raw/32768 g/rev",    [v / 32768.0 for v in vals]),
    ]
    pct_candidates = [
        ("raw/256 %",          [v / 256.0 for v in vals]),
        ("raw/100 %",          [v / 100.0 for v in vals]),
    ]
    for label, scaled in grev_candidates:
        in_range, bad = values_in_range(scaled, 0.0, 10.0)
        if not in_range or not is_monotonic(scaled, strict=True):
            continue
        if max(scaled) - min(scaled) < 0.5:
            continue
        return Verdict.yes(1.0,
            f"monotonic {scaled[0]:.2f}..{scaled[-1]:.2f} g/rev "
            f"({hint.length} pts) via {label}")
    for label, scaled in pct_candidates:
        in_range, bad = values_in_range(scaled, 0.0, 400.0)
        if not in_range or not is_monotonic(scaled, strict=True):
            continue
        if max(scaled) - min(scaled) < 10.0:
            continue
        return Verdict.yes(1.0,
            f"monotonic {scaled[0]:.1f}..{scaled[-1]:.1f} % "
            f"({hint.length} pts) via {label}")
    return Verdict.no(
        f"no scaling places {hint.length} cells in monotonic "
        f"[0,10] g/rev or [0,400] % bands")


ENGINE_LOAD_AXIS = register(Descriptor(
    id="engine_load_axis",
    kind="axis",
    description="Engine-load axis: monotonic float in [0, 10] g/rev, 4-32 pts.",
    id_patterns=[
        "*engine_load*", "*load_axis*", "*_load",
    ],
    predicate=_engine_load_axis,
    evidence=[
        Evidence("a2tb002c", "engine_load"),
    ],
    expected_dims=1,
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
    expected_dims=2,
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
    expected_dims=2,
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
    expected_dims=2,
))


def _timing_compensation_1d(buf: bytes, hint: DecodeHint) -> Verdict:
    # Timing-compensation 1D curves (ECT comp, IAT comp, MRP comp,
    # per-cylinder, etc.) carry a signed degree adjustment relative to
    # base timing. On Subaru EJ/DI ROMs the raw is typically uint8 with
    # one of a few signed-mapping scalings. The descriptor tries the
    # common ones and accepts any that places all cells inside ±30°,
    # which covers everything from gentle cold-engine advance to
    # aggressive knock-protection retard. Flat curves are accepted
    # (stock cars run with comp = 0 across the table); the only reject
    # is "no scaling fits".
    if hint.dims != 1 or hint.length < 4 or hint.length > 32:
        return Verdict.no(f"length {hint.length} outside [4,32]")
    if hint.dtype not in ("uint8", "int8"):
        return Verdict.no(f"dtype {hint.dtype} not byte-wide")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    # Common Subaru scalings for timing comp (factor, offset).
    candidates = [
        ("raw×0.352−45°",     [v * 0.3515625 - 45.0 for v in vals]),
        ("raw×0.5−64°",       [v * 0.5 - 64.0 for v in vals]),
        ("raw×0.25−32°",      [v * 0.25 - 32.0 for v in vals]),
        ("raw−128°",          [v - 128.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, -30.0, 30.0)
        if in_range:
            df = distinct_fraction(vals)
            return Verdict.yes(1.0,
                f"{hint.length}-pt in [−30,+30]° via {label}, {df:.0%} distinct")
    return Verdict.no(f"no scaling places all {hint.length} cells in −30..+30°")


TIMING_COMPENSATION_1D = register(Descriptor(
    id="timing_compensation_1d",
    kind="table",
    description="1D timing-compensation curve: signed degree adjustment in ±30°.",
    id_patterns=[
        "*timing_compensation_ect*", "*timing_compensation_iat*",
        "*timing_compensation_mrp*", "*timing_compensation_per_cylinder*",
        "*timing_compensation_a_iat*", "*timing_compensation_b_iat*",
        "*ignition_timing_correction*",
    ],
    predicate=_timing_compensation_1d,
    evidence=[
        Evidence("a2tb002c", "timing_compensation_ect"),
    ],
    expected_dims=1,
))


def _boost_compensation_1d(buf: bytes, hint: DecodeHint) -> Verdict:
    # Boost-target / wastegate-duty 1D compensation curves carry a
    # percent or delta-pressure adjustment. Common Subaru scalings:
    #   * raw×0.78125 − 100  → range −100..+99.2% (uint8 stock)
    #   * raw×0.39062 − 50   → range −50..+49.6% (some sub-platforms)
    #   * raw − 128          → straight signed-byte percent
    # We accept any scaling that places all cells in ±100% with at
    # least 1 distinct cell value (covers all-flat stock curves).
    if hint.dims != 1 or hint.length < 4 or hint.length > 32:
        return Verdict.no(f"length {hint.length} outside [4,32]")
    if hint.dtype not in ("uint8", "int8"):
        return Verdict.no(f"dtype {hint.dtype} not byte-wide")
    try:
        vals = decode_values(buf, hint.dtype, hint.length)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    candidates = [
        ("raw×0.781−100%",  [v * 0.78125 - 100.0 for v in vals]),
        ("raw×0.391−50%",   [v * 0.390625 - 50.0 for v in vals]),
        ("raw−128%",        [v - 128.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, -100.0, 100.0)
        if in_range:
            df = distinct_fraction(vals)
            return Verdict.yes(1.0,
                f"{hint.length}-pt in [−100,+100]% via {label}, {df:.0%} distinct")
    return Verdict.no(f"no scaling places all {hint.length} cells in −100..+100%")


BOOST_COMPENSATION_1D = register(Descriptor(
    id="boost_compensation_1d",
    kind="table",
    description=(
        "1D signed-percent compensation curve: ±100% adjustment via the "
        "Subaru _x_78125_100 scaling family (×0.78125 − 100). Despite the "
        "`boost` prefix in the id, the same predicate covers any 1D "
        "compensation that lives in this scaling family — boost-target, "
        "wastegate-duty, turbo-dynamics, injector-PW, tip-in enrichment, "
        "fuel-cranking comp, etc."),
    id_patterns=[
        # Boost / wastegate
        "*target_boost_compensation*", "*boost_compensation*",
        "*wastegate_duty_compensation*", "*wastegate_compensation*",
        "*initial_max_wastegate_duty_compensation*",
        # Turbo dynamics (proportional / integral comp)
        "*td_proportional_compensation*", "*td_integral*_compensation*",
        # Injector pulse-width
        "*injector_pulse_width_compensation*",
        "*cranking_fuel_ipw_compensation*",
        # Tip-in enrichment
        "*tip_in_enrichment_compensation*",
    ],
    predicate=_boost_compensation_1d,
    evidence=[
        Evidence("a2tb002c", "target_boost_compensation_ect"),
        Evidence("a2tb002c", "td_proportional_compensation_iat"),
    ],
    expected_dims=1,
))


def _compensation_2d_signed_percent(buf: bytes, hint: DecodeHint) -> Verdict:
    # 2D variant of _boost_compensation_1d. Same scaling family, same
    # bands. Subaru uses 2D compensation maps for cases where the
    # adjustment depends on two axes (e.g. per-injector IPW comp by
    # RPM × load, atm-pressure comp by RPM × something).
    if hint.dims != 2 or hint.rows < 4 or hint.cols < 4:
        return Verdict.no(f"shape {hint.rows}x{hint.cols} too small (need 4×4+)")
    if hint.dtype not in ("uint8", "int8"):
        return Verdict.no(f"dtype {hint.dtype} not byte-wide")
    cells = hint.total_cells()
    try:
        vals = decode_values(buf, hint.dtype, cells)
    except (KeyError, struct.error) as exc:
        return Verdict.no(f"decode failed: {exc}")
    candidates = [
        ("raw×0.781−100%",  [v * 0.78125 - 100.0 for v in vals]),
        ("raw×0.391−50%",   [v * 0.390625 - 50.0 for v in vals]),
        ("raw−128%",        [v - 128.0 for v in vals]),
    ]
    for label, scaled in candidates:
        in_range, bad = values_in_range(scaled, -100.0, 100.0)
        if in_range:
            df = distinct_fraction(vals)
            return Verdict.yes(1.0,
                f"{hint.rows}x{hint.cols} in [−100,+100]% via {label}, "
                f"{df:.0%} distinct")
    return Verdict.no(f"no scaling places all {cells} cells in −100..+100%")


COMPENSATION_2D_SIGNED_PERCENT = register(Descriptor(
    id="compensation_2d_signed_percent",
    kind="table",
    description=(
        "2D signed-percent compensation map (uint8/int8 ≥4×4): adjustment "
        "in ±100% via the same _x_78125_100 scaling family the 1D variant "
        "covers. Examples: per-injector pulse-width comp, cranking-fuel "
        "IPW comp by RPM, atm-pressure-keyed wastegate-duty comp."),
    id_patterns=[
        "*per_injector_pulse_width_compensation*",
        "*injector_pulse_width_compensation*",
        "*cranking_fuel_ipw_compensation*",
        "*initial_max_wastegate_duty_compensation_atm_pressure*",
        # 2D timing-comp activation tables share the same scaling band
        # (the activation map is the COMP MULTIPLIER, not absolute timing).
        "*timing_compensation_*_activation*",
    ],
    predicate=_compensation_2d_signed_percent,
    evidence=[
        Evidence("a2tb002c", "per_injector_pulse_width_compensation_a"),
        Evidence("a2tb002c", "cranking_fuel_ipw_compensation_imm_cruise_rpm"),
    ],
    expected_dims=2,
))
