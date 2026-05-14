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

    def test_division_by_constant(self):
        # `(x)/256.0` and `(x)/5.12` are the most-common Subaru scaling
        # shapes; the previous regex parser didn't accept `/` at all.
        result = defgen.parse_toexpr("(x)/256.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 1.0 / 256.0)
        self.assertAlmostEqual(o, 0.0)

        result = defgen.parse_toexpr("(x)/5.12")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 1.0 / 5.12)
        self.assertAlmostEqual(o, 0.0)

    def test_paren_x_times_constant(self):
        # `(x)*0.5` — paren around x alone, then multiplication.
        result = defgen.parse_toexpr("(x)*0.5")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 0.5)
        self.assertAlmostEqual(o, 0.0)

    def test_paren_offset_then_divide(self):
        # `((x)-16000.0)/80.0` — subtract then divide.
        result = defgen.parse_toexpr("((x)-16000.0)/80.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 1.0 / 80.0)
        self.assertAlmostEqual(o, -16000.0 / 80.0)

    def test_chained_divide_then_offset(self):
        # `((x)/256.0)+1.0` — the AFR scaling.
        result = defgen.parse_toexpr("((x)/256.0)+1.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 1.0 / 256.0)
        self.assertAlmostEqual(o, 1.0)

    def test_chained_divide_then_multiply(self):
        # `(((x)-128.0)/128.0)*100.0` — typical short-trim percentage shape.
        result = defgen.parse_toexpr("(((x)-128.0)/128.0)*100.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 100.0 / 128.0)
        self.assertAlmostEqual(o, -100.0)

    def test_inversion_by_negative(self):
        # `(x)*-1.0` and `((x)-1260.0)*-1.0`.
        result = defgen.parse_toexpr("(x)*-1.0")
        assert result is not None
        self.assertEqual(result, (-1.0, 0.0))

        result = defgen.parse_toexpr("((x)-1260.0)*-1.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, -1.0)
        self.assertAlmostEqual(o, 1260.0)

    def test_deeply_nested_linear_composition(self):
        # Real VA chain: `((((x)*256.0)/25600.0)*14.5038)/5.0`.
        result = defgen.parse_toexpr("((((x)*256.0)/25600.0)*14.5038)/5.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 256.0 / 25600.0 * 14.5038 / 5.0)
        self.assertAlmostEqual(o, 0.0)

    def test_unary_minus_on_x(self):
        result = defgen.parse_toexpr("-x*2")
        assert result is not None
        self.assertEqual(result, (-2.0, 0.0))

    def test_division_by_x_is_non_linear(self):
        # `c/x` and `c/(x+k)` are non-linear; must return None rather than
        # silently producing nonsense from the constant numerator.
        self.assertIsNone(defgen.parse_toexpr("1/x"))
        self.assertIsNone(defgen.parse_toexpr("1.0/(x+5)"))

    def test_hardware_shift_comment_is_non_linear(self):
        # RomRaider embeds RSHIFT(N)/LSHIFT(N)/INVERSE_DIVIDE(N) in C-style
        # block comments to encode hardware semantics that change the
        # meaning of the surrounding expression. We can't safely strip and
        # evaluate the remainder — flag as non-linear and let the user
        # hand-edit.
        self.assertIsNone(defgen.parse_toexpr("/*RSHIFT(8.0)*/x"))
        self.assertIsNone(defgen.parse_toexpr("(/*INVERSE_DIVIDE(1.0)*/x)*12500.0"))

    def test_empty_string_is_identity(self):
        self.assertEqual(defgen.parse_toexpr(""), (1.0, 0.0))
        self.assertEqual(defgen.parse_toexpr("   "), (1.0, 0.0))

    def test_division_by_zero_is_non_linear(self):
        self.assertIsNone(defgen.parse_toexpr("x/0"))
        self.assertIsNone(defgen.parse_toexpr("(x+1)/0.0"))


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


class InheritanceTest(unittest.TestCase):
    """Tests for RomRaider <base> inheritance flattening."""

    def setUp(self):
        text = (FIXTURE_DIR / "inherited_rom.xml").read_text(encoding="utf-8")
        self.packs = defgen.parse_rom_xml(text)
        self.by_id = {p.rom_id: p for p in self.packs}

    def test_pure_base_is_dropped_from_output(self):
        # BASE_VA_MT has no internalidstring -> not a real tunable rom.
        self.assertNotIn("base_va_mt", self.by_id)

    def test_two_concrete_roms_emitted(self):
        self.assertEqual(len(self.packs), 2)
        self.assertIn("as80u_2019", self.by_id)
        self.assertIn("as80u_2020", self.by_id)

    def test_child_inherits_base_axes_and_scalings(self):
        # Both children should have the RPM scaling from the base.
        for pid in ("as80u_2019", "as80u_2020"):
            p = self.by_id[pid]
            scaling_ids = {s.id for s in p.scalings}
            self.assertIn("rpm", scaling_ids,
                          f"{pid} missing inherited 'rpm' scaling")

    def test_child_with_override_uses_its_own_address(self):
        # 2019 overrides Boost Target with address 0x50100; 2020 inherits
        # the base's 0x50000.
        p2019 = self.by_id["as80u_2019"]
        p2020 = self.by_id["as80u_2020"]

        bt_2019 = next(t for t in p2019.tables if t.id == "boost_target")
        bt_2020 = next(t for t in p2020.tables if t.id == "boost_target")
        self.assertEqual(bt_2019.address, 0x50100)
        self.assertEqual(bt_2020.address, 0x50000)

    def test_child_inherits_non_overridden_tables(self):
        # Boost Limit lives only in the base; both children should inherit it.
        for pid in ("as80u_2019", "as80u_2020"):
            p     = self.by_id[pid]
            t_ids = {t.id for t in p.tables}
            self.assertIn("boost_limit", t_ids,
                          f"{pid} missing inherited 'boost_limit' table")

    def test_child_inherits_romid_fields_from_base(self):
        # The base declares transmission=MT; the children don't redeclare it
        # and should inherit it.
        for pid in ("as80u_2019", "as80u_2020"):
            self.assertEqual(self.by_id[pid].transmission, "mt",
                             f"{pid} did not inherit transmission")

    def test_filter_can_select_a_pure_base(self):
        # Even pure bases are emittable when explicitly requested.
        text = (FIXTURE_DIR / "inherited_rom.xml").read_text(encoding="utf-8")
        packs = defgen.parse_rom_xml(text, rom_id_filter="BASE_VA_MT")
        self.assertEqual(len(packs), 1)
        self.assertEqual(packs[0].rom_id, "base_va_mt")


class InheritanceCycleTest(unittest.TestCase):
    def test_cycle_raises(self):
        xml = """<roms>
          <rom><romid><xmlid>A</xmlid><base>B</base></romid></rom>
          <rom><romid><xmlid>B</xmlid><base>A</base></romid></rom>
        </roms>"""
        with self.assertRaises(ValueError):
            defgen.parse_rom_xml(xml)


class NonLinearFormulaWarningTest(unittest.TestCase):
    def test_non_linear_toexpr_records_warning(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <scaling name="QuadThing" units="x" toexpr="x*x+1" fromexpr="x"
                   format="0.00" endian="big" storagetype="uint16"/>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        self.assertEqual(len(packs), 1)
        # Identity scaling: factor=1, offset=0.
        s = next(s for s in packs[0].scalings if s.id == "quadthing")
        self.assertEqual((s.factor, s.offset), (1.0, 0.0))
        # And a warning was recorded for that record.
        scaling_warnings = [w for w in packs[0].warnings if w[0] == "scaling"]
        self.assertTrue(
            any(w[1] == "quadthing" and "non-linear" in w[2]
                for w in scaling_warnings),
            f"expected non-linear warning for quadthing; got {scaling_warnings!r}",
        )

    def test_linear_toexpr_records_no_warning(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <scaling name="Boring" units="" toexpr="x*0.5+10" fromexpr="x"
                   format="0" endian="big" storagetype="uint16"/>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        self.assertEqual(packs[0].warnings, [])


class ApplyToPackTest(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.tmp = Path(tempfile.mkdtemp(prefix="defgen_apply_"))
        # Seed an existing pack from the minimal fixture, then trim it.
        text = (FIXTURE_DIR / "minimal_rom.xml").read_text(encoding="utf-8")
        packs = defgen.parse_rom_xml(text)
        self.full = packs[0]
        # Build a "partial" pack that only knows about scaling "rpm" + the
        # identification.
        partial = defgen.Pack(
            rom_id=self.full.rom_id,
            display_name=self.full.display_name,
            platform=self.full.platform,
            transmission=self.full.transmission,
            years=list(self.full.years),
            rom_size_bytes=self.full.rom_size_bytes,
            identifications=list(self.full.identifications),
            scalings=[s for s in self.full.scalings if s.id == "rpm"],
            axes=[],
            tables=[],
        )
        self.pack_path = self.tmp / "base.toml"
        self.pack_path.write_text(defgen.pack_to_toml(partial),
                                  encoding="utf-8")

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_collect_existing_ids(self):
        ids = defgen._collect_existing_ids(self.pack_path)
        self.assertEqual(ids.scalings, {"rpm"})
        self.assertEqual(ids.axes, set())
        self.assertEqual(ids.tables, set())
        self.assertEqual(ids.identifications, {"SYN_VA_WRX_MT_2019"})

    def test_filter_drops_existing_records(self):
        ids = defgen._collect_existing_ids(self.pack_path)
        filt = defgen._filter_pack_by_existing(self.full, ids)
        self.assertNotIn("rpm", [s.id for s in filt.scalings])
        self.assertIn("boost_kpa", [s.id for s in filt.scalings])
        self.assertEqual(filt.identifications, [])

    def test_merge_via_main_appends_missing_records(self):
        rc = defgen.main([
            str(FIXTURE_DIR / "minimal_rom.xml"),
            "--apply-to-pack", str(self.pack_path),
        ])
        self.assertEqual(rc, 0)
        import tomllib
        raw = tomllib.loads(self.pack_path.read_text(encoding="utf-8"))
        scaling_ids = {s["id"] for s in raw.get("scaling", [])}
        # Original "rpm" still there; new scalings appended.
        self.assertIn("rpm", scaling_ids)
        self.assertIn("boost_kpa", scaling_ids)
        # Only one copy of "rpm" — no duplicate.
        self.assertEqual([s["id"] for s in raw["scaling"]].count("rpm"), 1)
        # Axes and tables made it in.
        self.assertTrue(raw.get("axis"))
        self.assertTrue(raw.get("table"))

    def test_second_merge_is_a_no_op(self):
        defgen.main([
            str(FIXTURE_DIR / "minimal_rom.xml"),
            "--apply-to-pack", str(self.pack_path),
        ])
        size_after_first = self.pack_path.stat().st_size
        rc = defgen.main([
            str(FIXTURE_DIR / "minimal_rom.xml"),
            "--apply-to-pack", str(self.pack_path),
        ])
        self.assertEqual(rc, 0)
        self.assertEqual(self.pack_path.stat().st_size, size_after_first)


if __name__ == "__main__":
    unittest.main()
