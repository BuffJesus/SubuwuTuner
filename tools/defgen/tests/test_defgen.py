# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Unit tests for defgen. Run with: python -m unittest discover -s tools/defgen"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

# Make `import defgen` work when running `python -m unittest` from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import defgen  # noqa: E402


FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures"


class MapDataTypeTest(unittest.TestCase):
    def test_uint8_ignores_endian(self):
        self.assertEqual(defgen.map_data_type("uint8", "big"),    "uint8")
        self.assertEqual(defgen.map_data_type("uint8", "little"), "uint8")

    def test_uint16_be(self):
        self.assertEqual(defgen.map_data_type("uint16", "big"), "uint16_be")

    def test_uint16_le(self):
        self.assertEqual(defgen.map_data_type("uint16", "little"), "uint16_le")

    def test_float(self):
        self.assertEqual(defgen.map_data_type("float", "big"), "float32_be")

    def test_default_when_storagetype_missing(self):
        self.assertEqual(defgen.map_data_type(None, "big"), "uint16_be")

    def test_unknown_raises(self):
        with self.assertRaises(ValueError):
            defgen.map_data_type("uint37", "big")


class ParseToExprTest(unittest.TestCase):
    def test_plain_x(self):
        self.assertEqual(defgen.parse_toexpr("x"), (1.0, 0.0))

    def test_multiplication(self):
        self.assertEqual(defgen.parse_toexpr("x*0.5"), (0.5, 0.0))

    def test_multiplication_with_spaces(self):
        self.assertEqual(defgen.parse_toexpr("x * 0.5"), (0.5, 0.0))

    def test_multiplication_then_offset(self):
        result = defgen.parse_toexpr("x*0.5+10")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 0.5)
        self.assertAlmostEqual(o, 10.0)

    def test_parenthesized_offset_then_multiply(self):
        # (x - 100) * 0.5  ==  x*0.5 - 50
        result = defgen.parse_toexpr("(x-100)*0.5")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 0.5)
        self.assertAlmostEqual(o, -50.0)

    def test_non_linear_returns_none(self):
        self.assertIsNone(defgen.parse_toexpr("x*x"))
        self.assertIsNone(defgen.parse_toexpr("sin(x)"))


class HexAndPrecisionTest(unittest.TestCase):
    def test_hex_format(self):
        self.assertEqual(defgen._hex(0), "0x00000000")
        self.assertEqual(defgen._hex(0x50000), "0x00050000")
        self.assertEqual(defgen._hex(0xDEADBEEF), "0xDEADBEEF")

    def test_format_to_precision(self):
        self.assertEqual(defgen._format_to_precision("0"), 0)
        self.assertEqual(defgen._format_to_precision("0.00"), 2)
        self.assertEqual(defgen._format_to_precision("0.000"), 3)
        self.assertEqual(defgen._format_to_precision(None), 0)

    def test_filesize(self):
        self.assertEqual(defgen._parse_filesize("1.5MB"), 1572864)
        self.assertEqual(defgen._parse_filesize("2MB"), 2097152)
        self.assertEqual(defgen._parse_filesize("512KB"), 524288)
        self.assertEqual(defgen._parse_filesize("1572864"), 1572864)

    def test_slugify(self):
        self.assertEqual(defgen._slugify("RPM Axis"), "rpm_axis")
        self.assertEqual(defgen._slugify("Boost Target High-Octane"),
                         "boost_target_high_octane")
        self.assertEqual(defgen._slugify("__"), "unnamed")


class FixtureEndToEndTest(unittest.TestCase):
    """Parse the synthetic XML fixture and assert the emitted TOML is sane."""

    def setUp(self):
        text = (FIXTURE_DIR / "minimal_rom.xml").read_text(encoding="utf-8")
        self.packs = defgen.parse_rom_xml(text)

    def test_exactly_one_pack(self):
        self.assertEqual(len(self.packs), 1)

    def test_pack_metadata(self):
        p = self.packs[0]
        self.assertEqual(p.rom_id, "syn_va_wrx_mt_2019")
        self.assertEqual(p.display_name, "SYN_VA_WRX_MT_2019")
        self.assertEqual(p.transmission, "mt")
        self.assertEqual(p.rom_size_bytes, 1572864)
        self.assertEqual(p.years, [2019])

    def test_identification(self):
        p = self.packs[0]
        self.assertEqual(len(p.identifications), 1)
        ident = p.identifications[0]
        self.assertEqual(ident.cid_address, 0x2000)
        self.assertEqual(ident.cid_match, "AS80U   ")
        self.assertEqual(ident.cid_length, 8)

    def test_scalings_deduplicated(self):
        p = self.packs[0]
        ids = [s.id for s in p.scalings]
        # We deduplicate by id, so each scaling appears once even though it
        # was declared inline in the table and at the top level.
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn("rpm", ids)
        self.assertIn("engine_load", ids)
        self.assertIn("boost_kpa", ids)  # _slugify of "Boost kPa"
        self.assertIn("percent_x1", ids)

    def test_axes_extracted(self):
        p = self.packs[0]
        axis_ids = [a.id for a in p.axes]
        self.assertIn("rpm_axis", axis_ids)
        self.assertIn("load_axis", axis_ids)
        rpm_axis = next(a for a in p.axes if a.id == "rpm_axis")
        self.assertEqual(rpm_axis.address, 0x40000)
        self.assertEqual(rpm_axis.length, 16)
        self.assertEqual(rpm_axis.data_type, "uint16_be")

    def test_tables_extracted(self):
        p = self.packs[0]
        table_ids = [t.id for t in p.tables]
        self.assertIn("boost_target_high_octane", table_ids)
        boost = next(t for t in p.tables if t.id == "boost_target_high_octane")
        self.assertEqual(boost.dimensions, 2)
        self.assertEqual(boost.address, 0x50000)
        self.assertEqual(boost.axis_x, "rpm_axis")
        self.assertEqual(boost.axis_y, "load_axis")
        self.assertEqual(boost.category, "boost")

    def test_boost_scaling_linear_offset(self):
        p = self.packs[0]
        boost = next(s for s in p.scalings if s.id == "boost_kpa")
        # (x-100)*0.5  =>  factor=0.5, offset=-50
        self.assertAlmostEqual(boost.factor, 0.5)
        self.assertAlmostEqual(boost.offset, -50.0)
        self.assertEqual(boost.unit, "kPa")
        self.assertEqual(boost.precision, 1)

    def test_emitted_toml_round_trips_through_tomllib(self):
        import tomllib
        p = self.packs[0]
        toml_text = defgen.pack_to_toml(p)
        # Must be valid TOML
        parsed = tomllib.loads(toml_text)
        self.assertEqual(parsed["pack"]["id"], "syn_va_wrx_mt_2019")
        self.assertEqual(parsed["pack"]["rom_size_bytes"], 1572864)
        self.assertGreaterEqual(len(parsed["scaling"]), 4)
        self.assertGreaterEqual(len(parsed["axis"]), 2)
        self.assertGreaterEqual(len(parsed["table"]), 2)


class RomIdFilterTest(unittest.TestCase):
    def test_filter_matches(self):
        text = (FIXTURE_DIR / "minimal_rom.xml").read_text(encoding="utf-8")
        packs = defgen.parse_rom_xml(text, rom_id_filter="SYN_VA_WRX_MT_2019")
        self.assertEqual(len(packs), 1)

    def test_filter_misses(self):
        text = (FIXTURE_DIR / "minimal_rom.xml").read_text(encoding="utf-8")
        packs = defgen.parse_rom_xml(text, rom_id_filter="NOPE")
        self.assertEqual(len(packs), 0)


class ErrorPathTest(unittest.TestCase):
    def test_bad_root_element(self):
        with self.assertRaises(ValueError):
            defgen.parse_rom_xml("<other/>")

    def test_empty_input_raises(self):
        from xml.etree.ElementTree import ParseError
        with self.assertRaises(ParseError):
            defgen.parse_rom_xml("")


if __name__ == "__main__":
    unittest.main()
