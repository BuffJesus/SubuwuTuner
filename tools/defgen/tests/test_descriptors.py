# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Unit tests for descriptors.py.

Each descriptor must:
  (a) classify its own synthetic positive evidence as matching;
  (b) reject obvious negatives (wrong shape, out-of-range values, padding);
  (c) gracefully decline (no exception) on degenerate input.

Tests build byte arrays inline rather than depending on real-pack ROMs in
fixtures/. Real-pack regression coverage lives in a future tools/defgen/
validate.py — descriptors.py itself is a pure-Python library.
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import descriptors  # noqa: E402


# ---------------------------------------------------------------------------
# Byte builders
# ---------------------------------------------------------------------------

def _pack_floats_be(values: list[float]) -> bytes:
    return b"".join(struct.pack(">f", v) for v in values)


def _pack_uint16_be(values: list[int]) -> bytes:
    return b"".join(struct.pack(">H", v) for v in values)


def _pack_int16_be(values: list[int]) -> bytes:
    return b"".join(struct.pack(">h", v) for v in values)


# ---------------------------------------------------------------------------
# Decode helpers
# ---------------------------------------------------------------------------

class DecodeHelperTest(unittest.TestCase):
    def test_decode_values_uint16_be(self):
        buf = _pack_uint16_be([1000, 2000, 3000])
        self.assertEqual(descriptors.decode_values(buf, "uint16_be", 3),
                         [1000.0, 2000.0, 3000.0])

    def test_decode_values_float32_be(self):
        buf = _pack_floats_be([800.0, 1200.0, 1600.0])
        out = descriptors.decode_values(buf, "float32_be", 3)
        self.assertEqual(len(out), 3)
        for got, want in zip(out, [800.0, 1200.0, 1600.0]):
            self.assertAlmostEqual(got, want)

    def test_decode_values_unknown_dtype_raises(self):
        with self.assertRaises(KeyError):
            descriptors.decode_values(b"\x00" * 4, "fancy_t", 1)

    def test_is_monotonic_strict(self):
        self.assertTrue(descriptors.is_monotonic([1, 2, 3]))
        self.assertFalse(descriptors.is_monotonic([1, 2, 2]))  # equal rejected
        self.assertTrue(descriptors.is_monotonic([1, 2, 2], strict=False))

    def test_distinct_fraction(self):
        self.assertEqual(descriptors.distinct_fraction([1, 2, 3, 4]), 1.0)
        self.assertEqual(descriptors.distinct_fraction([5, 5, 5, 5]), 0.25)
        self.assertEqual(descriptors.distinct_fraction([]), 0.0)


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

class RegistryTest(unittest.TestCase):
    def test_seed_descriptors_loaded(self):
        ids = {d.id for d in descriptors.all_descriptors()}
        for expected in ("engine_rpm_axis", "coolant_temp_axis",
                         "intake_temp_axis", "engine_load_axis",
                         "wastegate_duty", "boost_target", "base_timing",
                         "timing_compensation_1d", "boost_compensation_1d"):
            self.assertIn(expected, ids)

    def test_by_id(self):
        d = descriptors.by_id("engine_rpm_axis")
        self.assertIsNotNone(d)
        self.assertEqual(d.kind, "axis")

    def test_by_id_unknown_returns_none(self):
        self.assertIsNone(descriptors.by_id("not_a_descriptor"))

    def test_candidates_for_rpm_axis(self):
        # "engine_speed_rpm" matches *engine_speed*, *rpm*, *_rpm
        names = {d.id for d in descriptors.candidates_for("engine_speed_rpm")}
        self.assertIn("engine_rpm_axis", names)

    def test_candidates_for_kind_filter(self):
        # A wastegate-duty table id shouldn't surface axis descriptors.
        names = {d.id for d in descriptors.candidates_for("max_wastegate_duty",
                                                          kind="table")}
        self.assertIn("wastegate_duty", names)
        self.assertNotIn("engine_rpm_axis", names)

    def test_candidates_for_dims_filter_blocks_2d_predicate_on_1d_entry(self):
        # *target_boost* pattern would match target_boost_compensation_ect
        # (a 1D ECT compensation), but the boost_target predicate is
        # explicitly 2D. With dims=1 the 2D-only descriptor must be skipped.
        names = {d.id for d in descriptors.candidates_for(
            "target_boost_compensation_ect", kind="table", dims=1)}
        self.assertNotIn("boost_target", names)

    def test_candidates_for_dims_filter_allows_2d_predicate_on_2d_entry(self):
        names = {d.id for d in descriptors.candidates_for(
            "max_wastegate_duty", kind="table", dims=2)}
        self.assertIn("wastegate_duty", names)


# ---------------------------------------------------------------------------
# Engine RPM axis
# ---------------------------------------------------------------------------

class EngineRpmAxisTest(unittest.TestCase):
    def _hint(self, length: int) -> descriptors.DecodeHint:
        return descriptors.DecodeHint(dtype="float32_be", dims=1, length=length)

    def test_accepts_typical_subaru_rpm_axis(self):
        # Representative 11-point Subaru engine-speed axis (float32_be).
        rpms = [400.0, 800.0, 1200.0, 1600.0, 2000.0,
                2800.0, 3600.0, 4400.0, 5200.0, 6000.0, 6800.0]
        v = descriptors.ENGINE_RPM_AXIS.predicate(_pack_floats_be(rpms),
                                                  self._hint(11))
        self.assertTrue(v.matches, msg=v.reasons)
        self.assertGreaterEqual(v.score, 1.0)

    def test_rejects_non_monotonic(self):
        rpms = [400.0, 800.0, 600.0, 1600.0, 2000.0,
                2800.0, 3600.0, 4400.0, 5200.0, 6000.0, 6800.0]
        v = descriptors.ENGINE_RPM_AXIS.predicate(_pack_floats_be(rpms),
                                                  self._hint(11))
        self.assertFalse(v.matches)
        # Omnibus rejection — no scaling produced a monotonic sweep.
        self.assertTrue(any("monotonic" in r for r in v.reasons))

    def test_rejects_out_of_range(self):
        # Last value is unrealistically high (>8500).
        rpms = [400.0, 800.0, 1200.0, 1600.0, 2000.0,
                2800.0, 3600.0, 4400.0, 5200.0, 6000.0, 99999.0]
        v = descriptors.ENGINE_RPM_AXIS.predicate(_pack_floats_be(rpms),
                                                  self._hint(11))
        self.assertFalse(v.matches)

    def test_rejects_compressed_span(self):
        # Span = 900 RPM (< 1000 threshold) under raw scaling, and
        # /5.12 makes the span tinier still — neither candidate passes.
        rpms = [3000.0, 3090.0, 3180.0, 3270.0, 3360.0,
                3450.0, 3540.0, 3630.0, 3720.0, 3810.0, 3900.0]
        v = descriptors.ENGINE_RPM_AXIS.predicate(_pack_floats_be(rpms),
                                                  self._hint(11))
        self.assertFalse(v.matches)
        # Omnibus rejection — no scaling produced a wide-enough sweep.
        self.assertTrue(any("RPM band" in r for r in v.reasons))

    def test_rejects_truncated_buffer(self):
        # Hint claims 11 entries but buffer only holds 4.
        buf = _pack_floats_be([400.0, 800.0, 1200.0, 1600.0])
        v = descriptors.ENGINE_RPM_AXIS.predicate(buf, self._hint(11))
        self.assertFalse(v.matches)

    def test_accepts_va_era_x_5_12_scaling(self):
        # VA-era RPM axes encode RPM × 5.12 in uint16_be. Bundle XMLs
        # declare scaling expression="(x)/5.12". Real-world bytes for
        # an 8-point sweep from 800 to 6800 RPM under that scaling:
        rpms = [800, 1200, 1600, 2400, 3200, 4000, 5200, 6800]
        raw = [int(r * 5.12) for r in rpms]
        v = descriptors.ENGINE_RPM_AXIS.predicate(
            _pack_uint16_be(raw),
            descriptors.DecodeHint(dtype="uint16_be", dims=1, length=8))
        self.assertTrue(v.matches, msg=v.reasons)
        self.assertTrue(any("raw/5.12" in r for r in v.reasons))


# ---------------------------------------------------------------------------
# Coolant temperature axis
# ---------------------------------------------------------------------------

class CoolantTempAxisTest(unittest.TestCase):
    def test_accepts_typical_coolant_axis(self):
        # Representative 8-point ECT axis (int16_be in raw °C — many Subaru
        # axes scale identically; we accept the raw decoded values here).
        temps = [-40, -20, 0, 20, 40, 60, 80, 100]
        v = descriptors.COOLANT_TEMP_AXIS.predicate(
            _pack_int16_be(temps),
            descriptors.DecodeHint(dtype="int16_be", dims=1, length=8))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_rejects_out_of_range(self):
        temps = [-40, -20, 0, 20, 40, 60, 80, 500]
        v = descriptors.COOLANT_TEMP_AXIS.predicate(
            _pack_int16_be(temps),
            descriptors.DecodeHint(dtype="int16_be", dims=1, length=8))
        self.assertFalse(v.matches)


# ---------------------------------------------------------------------------
# Intake-temperature axis
# ---------------------------------------------------------------------------

class IntakeTempAxisTest(unittest.TestCase):
    def test_accepts_typical_iat_axis(self):
        # 6-point IAT axis (float32_be in °C, like a2tb002c).
        temps = [-20.0, -10.0, 0.0, 10.0, 20.0, 40.0]
        v = descriptors.INTAKE_TEMP_AXIS.predicate(
            _pack_floats_be(temps),
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=6))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_rejects_non_monotonic(self):
        temps = [-20.0, -10.0, 5.0, 0.0, 20.0, 40.0]  # 5 → 0 wrong
        v = descriptors.INTAKE_TEMP_AXIS.predicate(
            _pack_floats_be(temps),
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=6))
        self.assertFalse(v.matches)


# ---------------------------------------------------------------------------
# Engine-load axis
# ---------------------------------------------------------------------------

class EngineLoadAxisTest(unittest.TestCase):
    def test_accepts_typical_load_axis(self):
        # 15-pt engine-load axis: 0.27 → 2.56 g/rev, like a2tb002c.
        loads = [0.27, 0.57, 0.73, 1.00, 1.17, 1.30, 1.47, 1.64,
                 1.80, 1.98, 2.20, 2.30, 2.40, 2.48, 2.56]
        v = descriptors.ENGINE_LOAD_AXIS.predicate(
            _pack_floats_be(loads),
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=15))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_rejects_excessive_load(self):
        # Negative or way-high values flag this as not an engine-load axis.
        loads = [0.27, 0.57, 0.73, 1.00, 1.17, 1.30, 1.47, 1.64,
                 1.80, 1.98, 2.20, 2.30, 2.40, 2.48, 99.0]
        v = descriptors.ENGINE_LOAD_AXIS.predicate(
            _pack_floats_be(loads),
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=15))
        self.assertFalse(v.matches)

    def test_rejects_compressed_span(self):
        # Span < 0.5 → likely not a real engine-load sweep.
        loads = [1.00, 1.05, 1.10, 1.15, 1.20, 1.25, 1.30, 1.35]
        v = descriptors.ENGINE_LOAD_AXIS.predicate(
            _pack_floats_be(loads),
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=8))
        self.assertFalse(v.matches)
        self.assertTrue(any("span" in r for r in v.reasons))


# ---------------------------------------------------------------------------
# Wastegate-duty table
# ---------------------------------------------------------------------------

class WastegateDutyTest(unittest.TestCase):
    def test_accepts_typical_wastegate_map(self):
        # 8×8 (64 cells), spreading values across 1500-9500 raw (=15-95% duty)
        # to ensure distinct-fraction > 10%.
        cells = [(i * 7 + 1500) for i in range(64)]
        buf = _pack_uint16_be(cells)
        v = descriptors.WASTEGATE_DUTY.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=8, cols=8))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_rejects_padding_constant(self):
        # All cells identical — looks like padding, not a real map.
        cells = [5000] * 64
        buf = _pack_uint16_be(cells)
        v = descriptors.WASTEGATE_DUTY.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=8, cols=8))
        self.assertFalse(v.matches)

    def test_rejects_value_overrun(self):
        # 0xFFFF in every other cell — out of the [0, 12000] band.
        cells = [(0xFFFF if i % 2 else 1500 + i) for i in range(64)]
        buf = _pack_uint16_be(cells)
        v = descriptors.WASTEGATE_DUTY.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=8, cols=8))
        self.assertFalse(v.matches)

    def test_rejects_too_small(self):
        buf = _pack_uint16_be([1500] * 9)
        v = descriptors.WASTEGATE_DUTY.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=3, cols=3))
        self.assertFalse(v.matches)

    def test_rejects_wrong_dtype(self):
        # int16 (signed) flagged as not-uint.
        buf = _pack_int16_be([1500] * 64)
        v = descriptors.WASTEGATE_DUTY.predicate(
            buf,
            descriptors.DecodeHint(dtype="int16_be", dims=2, rows=8, cols=8))
        self.assertFalse(v.matches)


# ---------------------------------------------------------------------------
# Boost target
# ---------------------------------------------------------------------------

class BoostTargetTest(unittest.TestCase):
    def test_accepts_raw_kpa_scaling(self):
        # 8×8 boost map at 80-280 kPa raw (no further scaling), distinct cells.
        cells = [80 + (i * 3) % 200 for i in range(64)]
        buf = _pack_uint16_be(cells)
        v = descriptors.BOOST_TARGET.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=8, cols=8))
        self.assertTrue(v.matches, msg=v.reasons)
        self.assertTrue(any("raw kPa" in r for r in v.reasons))

    def test_accepts_raw_div128_scaling(self):
        # Values encoded as kPa × 128 (older EJ scaling).
        cells = [int((80 + (i * 3) % 200) * 128) for i in range(64)]
        buf = _pack_uint16_be(cells)
        v = descriptors.BOOST_TARGET.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=8, cols=8))
        self.assertTrue(v.matches, msg=v.reasons)
        self.assertTrue(any("raw/128" in r for r in v.reasons))

    def test_rejects_when_no_scaling_fits(self):
        # Values that won't land in 50-350 kPa under raw, /10, or /128:
        # raw 50000 → 50000; /10 → 5000; /128 → ~390. All exceed 350.
        cells = [50000 + (i * 3) for i in range(64)]
        buf = _pack_uint16_be(cells)
        v = descriptors.BOOST_TARGET.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=8, cols=8))
        self.assertFalse(v.matches)


# ---------------------------------------------------------------------------
# Base timing
# ---------------------------------------------------------------------------

class BaseTimingTest(unittest.TestCase):
    def test_accepts_int16_raw_degrees(self):
        # Distinct values from -5 to +35 °BTDC (raw degrees, no scaling).
        cells = [(-5 + (i % 41)) for i in range(64)]
        buf = _pack_int16_be(cells)
        v = descriptors.BASE_TIMING.predicate(
            buf,
            descriptors.DecodeHint(dtype="int16_be", dims=2, rows=8, cols=8))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_accepts_int16_quarter_degree_scaling(self):
        # Older EJ ROMs store timing as ×4 — same range, just larger raw.
        cells = [int((-5 + (i % 41)) * 4) for i in range(64)]
        buf = _pack_int16_be(cells)
        v = descriptors.BASE_TIMING.predicate(
            buf,
            descriptors.DecodeHint(dtype="int16_be", dims=2, rows=8, cols=8))
        self.assertTrue(v.matches, msg=v.reasons)
        self.assertTrue(any("raw/4" in r for r in v.reasons))

    def test_rejects_out_of_band(self):
        # 200° timing makes no physical sense.
        cells = [200] * 64
        buf = _pack_int16_be(cells)
        v = descriptors.BASE_TIMING.predicate(
            buf,
            descriptors.DecodeHint(dtype="int16_be", dims=2, rows=8, cols=8))
        self.assertFalse(v.matches)


# ---------------------------------------------------------------------------
# Timing-compensation 1D
# ---------------------------------------------------------------------------

def _pack_bytes(values: list[int]) -> bytes:
    return bytes(values)


class TimingCompensation1DTest(unittest.TestCase):
    def test_accepts_typical_ect_comp_curve(self):
        # Real bytes from a2tb002c timing_compensation_ect (ECT axis comp).
        # raw 151..117 with the canonical x*.3515625 − 45 scaling decode to
        # +8..−4° BTDC across the 16-point ECT sweep.
        raw = [151, 151, 151, 151, 148, 144, 139, 137,
               134, 131, 128, 128, 128, 128, 122, 117]
        v = descriptors.TIMING_COMPENSATION_1D.predicate(
            _pack_bytes(raw),
            descriptors.DecodeHint(dtype="uint8", dims=1, length=16))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_rejects_out_of_band(self):
        # All-zero with scaling x*.352 − 45 → −45° (out of −30..+30 band).
        # All other tried scalings push it further out.
        raw = [0] * 16
        v = descriptors.TIMING_COMPENSATION_1D.predicate(
            _pack_bytes(raw),
            descriptors.DecodeHint(dtype="uint8", dims=1, length=16))
        self.assertFalse(v.matches)

    def test_rejects_wrong_dtype(self):
        v = descriptors.TIMING_COMPENSATION_1D.predicate(
            _pack_uint16_be([128] * 16),
            descriptors.DecodeHint(dtype="uint16_be", dims=1, length=16))
        self.assertFalse(v.matches)


# ---------------------------------------------------------------------------
# Boost-compensation 1D
# ---------------------------------------------------------------------------

class BoostCompensation1DTest(unittest.TestCase):
    def test_accepts_flat_stock_curve(self):
        # a2tb002c target_boost_compensation_ect — all 128 raw bytes →
        # 0% adjustment across the table. Flat-stock curve must be PASS.
        raw = [128] * 16
        v = descriptors.BOOST_COMPENSATION_1D.predicate(
            _pack_bytes(raw),
            descriptors.DecodeHint(dtype="uint8", dims=1, length=16))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_accepts_asymmetric_compensation(self):
        # 128±some — typical asymmetric comp where cold ECT gets +20%
        # and hot ECT gets −10%.
        raw = [154, 150, 145, 140, 135, 132, 130, 128,
               128, 126, 124, 122, 120, 118, 117, 115]
        v = descriptors.BOOST_COMPENSATION_1D.predicate(
            _pack_bytes(raw),
            descriptors.DecodeHint(dtype="uint8", dims=1, length=16))
        self.assertTrue(v.matches, msg=v.reasons)


# ---------------------------------------------------------------------------
# Real-ROM evidence smoke check
# ---------------------------------------------------------------------------
# Optional cross-check: load a known-good (pack, ROM) pair from the in-tree
# corpus and verify that each descriptor's cited evidence still matches.
# Skipped when the ROM file isn't present (so the unit test pass doesn't
# depend on private fixtures being checked out).

_A2TB002C_ROM = (Path(__file__).resolve().parents[3]
                 / "fixtures/private/plaintext_corpus/bludgod-roms/USDM/Legacy"
                 / "A2TB002C-2009-USDM-Subaru-Legacy-GT-AT.hex")


@unittest.skipUnless(_A2TB002C_ROM.is_file(),
                     f"corpus ROM missing: {_A2TB002C_ROM}")
class RealRomEvidenceTest(unittest.TestCase):
    """Each cited evidence in seed descriptors must match its real bytes.

    Drift guard: if someone edits a predicate's accept band and breaks its
    own evidence, this fails immediately rather than silently invalidating
    every downstream consumer.
    """

    @classmethod
    def setUpClass(cls):
        cls.rom = _A2TB002C_ROM.read_bytes()

    def test_engine_rpm_axis_matches_a2tb002c_engine_speed(self):
        # engine_speed axis: 15 pts float32_be at 0x000C13A8.
        buf = self.rom[0x000C13A8:0x000C13A8 + 15 * 4]
        v = descriptors.ENGINE_RPM_AXIS.predicate(
            buf,
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=15))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_wastegate_duty_matches_a2tb002c_max_wastegate_duty(self):
        # max_wastegate_duty: 15×11 uint16_be at 0x000C0C98, scaling /256.
        buf = self.rom[0x000C0C98:0x000C0C98 + 15 * 11 * 2]
        v = descriptors.WASTEGATE_DUTY.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=15, cols=11))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_boost_target_matches_a2tb002c_target_boost(self):
        # target_boost: 15×11 uint16_be at 0x000C13E4, EJ psi-derived scaling.
        buf = self.rom[0x000C13E4:0x000C13E4 + 15 * 11 * 2]
        v = descriptors.BOOST_TARGET.predicate(
            buf,
            descriptors.DecodeHint(dtype="uint16_be", dims=2, rows=15, cols=11))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_intake_temp_axis_matches_a2tb002c_intake_temperature(self):
        # intake_temperature: 6 pts float32_be at 0x000C0B7C.
        buf = self.rom[0x000C0B7C:0x000C0B7C + 6 * 4]
        v = descriptors.INTAKE_TEMP_AXIS.predicate(
            buf,
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=6))
        self.assertTrue(v.matches, msg=v.reasons)

    def test_engine_load_axis_matches_a2tb002c_engine_load(self):
        # engine_load: 15 pts float32_be at 0x000CBCFC.
        buf = self.rom[0x000CBCFC:0x000CBCFC + 15 * 4]
        v = descriptors.ENGINE_LOAD_AXIS.predicate(
            buf,
            descriptors.DecodeHint(dtype="float32_be", dims=1, length=15))
        self.assertTrue(v.matches, msg=v.reasons)


if __name__ == "__main__":
    unittest.main()
