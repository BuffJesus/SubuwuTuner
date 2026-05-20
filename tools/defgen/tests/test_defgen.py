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

    def test_rshift_comment_is_linear_scaling(self):
        # `/*RSHIFT(N)*/x` is a fixed-point hint: read the raw bytes and
        # divide by 2**N. Linear with factor=1/2**N.
        self.assertEqual(defgen.parse_toexpr("/*RSHIFT(7.0)*/x"),
                         (1.0 / 128.0, 0.0))
        self.assertEqual(defgen.parse_toexpr("/*RSHIFT(8.0)*/x"),
                         (1.0 / 256.0, 0.0))

    def test_lshift_comment_is_linear_scaling(self):
        # `/*LSHIFT(N)*/x` is the inverse — multiply by 2**N.
        self.assertEqual(defgen.parse_toexpr("/*LSHIFT(8.0)*/x"),
                         (256.0, 0.0))

    def test_shift_comment_composes_with_outer_expression(self):
        # Real VA shape: `(/*RSHIFT(11.0)*/(x)*5.0)-40.0` ≡ x*5/2048 - 40.
        result = defgen.parse_toexpr("(/*RSHIFT(11.0)*/(x)*5.0)-40.0")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 5.0 / 2048.0)
        self.assertAlmostEqual(o, -40.0)

    def test_stacked_shift_comments_compose(self):
        # `/*RSHIFT(7.0)*//*RSHIFT(1.0)*/x` ≡ x/(128*2) = x/256.
        result = defgen.parse_toexpr("/*RSHIFT(7.0)*//*RSHIFT(1.0)*/x")
        assert result is not None
        f, o = result
        self.assertAlmostEqual(f, 1.0 / 256.0)
        self.assertAlmostEqual(o, 0.0)

    def test_inverse_divide_comment_is_non_linear(self):
        # `/*INVERSE_DIVIDE(N)*/x` is N/x — genuinely non-linear.
        self.assertIsNone(defgen.parse_toexpr("/*INVERSE_DIVIDE(1.0)*/x"))
        self.assertIsNone(defgen.parse_toexpr("(/*INVERSE_DIVIDE(1.0)*/x)*12500.0"))

    def test_and_mask_comment_is_non_linear(self):
        # `/*AND(N)*/x` is a bitmask, non-linear in general.
        self.assertIsNone(defgen.parse_toexpr("/*AND(1.0)*/x"))

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

    def test_filesize_defaults_to_1mb_when_missing(self):
        # When the source XML omits <filesize> we fall back to the
        # Subaru-default 1MB rather than 0. Returning 0 used to cause
        # silent regressions on every bulk_regen pass — see commit
        # 0008695 (the regression-fix that motivated this default).
        self.assertEqual(defgen._parse_filesize(None), 1_048_576)
        self.assertEqual(defgen._parse_filesize(""), 1_048_576)
        self.assertEqual(defgen._parse_filesize("not a size"), 1_048_576)

    def test_slugify(self):
        self.assertEqual(defgen._slugify("RPM Axis"), "rpm_axis")
        self.assertEqual(defgen._slugify("Boost Target High-Octane"),
                         "boost_target_high_octane")
        self.assertEqual(defgen._slugify("__"), "unnamed")

    def test_table_address_accepts_bare_hex(self):
        """Some community-sourced XMLs (e.g. aidx_aid43764_EP5G600A.xml)
        ship the table address as bare hex like `address="dc938"`
        without the `0x` prefix. The legacy _parse_int call would
        interpret that as decimal and crash on hex digits A-F."""
        import xml.etree.ElementTree as ET
        bare = ET.fromstring('<table address="dc938"/>')
        prefixed = ET.fromstring('<table address="0xDC938"/>')
        storage = ET.fromstring('<table storageaddress="dc938"/>')
        self.assertEqual(defgen._table_address(bare), 0xDC938)
        self.assertEqual(defgen._table_address(prefixed), 0xDC938)
        self.assertEqual(defgen._table_address(storage), 0xDC938)

    def test_table_address_handles_empty_and_missing(self):
        import xml.etree.ElementTree as ET
        self.assertEqual(defgen._table_address(ET.fromstring('<table/>')), 0)
        self.assertEqual(
            defgen._table_address(ET.fromstring('<table address=""/>')),
            0)


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


class AxisLengthTest(unittest.TestCase):
    def test_size_attribute_populates_axis_length(self):
        # RomRaider's canonical attribute is `size`; some synthetic fixtures
        # (including this test suite's older fixtures) use `elements`. defgen
        # must accept both — production XML uses `size`, so prior to this
        # fix every real-world axis came out with length=0.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="curve" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="curve_x" storageaddress="0x200"
                   storagetype="uint16" endian="big" size="12">
              <scaling units="" expression="x" to_byte="x"
                       format="0" endian="big" storagetype="uint16"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["curve_x"].length, 12)

    def test_parent_sizex_sizey_fallback(self):
        # Merp's ecu_defs.xml puts axis length on the PARENT table's
        # `sizex=` / `sizey=` attributes, not on the axis element. defgen
        # must fall back to those when the axis itself carries no length.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="3D" name="vvt" sizex="20" sizey="12"
                 storageaddress="0x100" storagetype="uint8" endian="big">
            <table type="X Axis" name="vvt_x"
                   storageaddress="0x200" storagetype="uint8" endian="big"/>
            <table type="Y Axis" name="vvt_y"
                   storageaddress="0x210" storagetype="uint8" endian="big"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["vvt_x"].length, 20)
        self.assertEqual(axes["vvt_y"].length, 12)

    def test_axis_own_size_wins_over_parent_fallback(self):
        # An axis with its own `size=` overrides whatever the parent says.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="curve" sizey="99"
                 storageaddress="0x100" storagetype="uint8" endian="big">
            <table type="X Axis" name="curve_x" size="7"
                   storageaddress="0x200" storagetype="uint8" endian="big"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["curve_x"].length, 7)

    def test_elements_still_works_as_fallback(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="curve" storageaddress="0x100" storagetype="uint8" endian="big">
            <table type="X Axis" name="curve_x" storageaddress="0x200"
                   storagetype="uint16" endian="big" elements="7"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["curve_x"].length, 7)

    def test_float_axis_data_type_from_element(self):
        # RomRaider declares float axes as `storagetype="float"` on the axis
        # <table> element itself. Before this fix, defgen ignored the axis
        # element's storagetype and used the inline scaling's data_type
        # default (uint16_be), making float-axis decoding produce alternating
        # high-half/low-half garbage at runtime.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="3D" name="vvt" sizex="16" sizey="16"
                 storageaddress="0x100" storagetype="uint16" endian="big">
            <table type="X Axis" name="vvt_x"
                   storageaddress="0x200" storagetype="float" endian="little">
              <scaling units="raw ecu value" expression="x" to_byte="x"
                       format="0.0"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        # Float axes — actual Subaru storage is big-endian regardless of the
        # XML's endian attribute (documented Merp/RR quirk). Verify the
        # axis emits float32_be, not the previous uint16_be default.
        self.assertEqual(axes["vvt_x"].data_type, "float32_be")

    def test_float_axis_endian_quirk_overridden_to_big(self):
        # Even when the XML insists endian="little", we override to big for
        # float axes — see _axis_from_element for the empirical note.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="foo" sizex="8"
                 storageaddress="0x100" storagetype="uint16" endian="big">
            <table type="X Axis" name="foo_x"
                   storageaddress="0x200" storagetype="float" endian="little"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["foo_x"].data_type, "float32_be")

    def test_uint16_axis_endian_preserved(self):
        # The float-axis override does NOT apply to uint16/uint8 axes, which
        # use the XML's declared endian as-is.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="foo" sizex="8"
                 storageaddress="0x100" storagetype="uint16" endian="big">
            <table type="X Axis" name="foo_x"
                   storageaddress="0x200" storagetype="uint16" endian="big"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["foo_x"].data_type, "uint16_be")

    def test_axis_without_storagetype_falls_through_to_scaling(self):
        # If the axis element omits storagetype (rare — usually means it
        # inherited from a base that wasn't merged), fall back to the
        # inline scaling's data_type (the prior behaviour). The inline
        # scaling here carries a real `units` so `_scaling_from_element`
        # actually returns a record instead of dropping it as nameless.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="foo" sizex="8"
                 storageaddress="0x100" storagetype="uint16" endian="big">
            <table type="X Axis" name="foo_x" storageaddress="0x200">
              <scaling units="RPM" expression="x" to_byte="x" format="0"
                       storagetype="uint8" endian="big"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axes = {a.id: a for a in packs[0].axes}
        self.assertEqual(axes["foo_x"].data_type, "uint8")


class EmissionsRelevanceTest(unittest.TestCase):
    def test_closed_loop_fueling_is_flagged(self):
        # Real Merp category strings — verbatim except for case.
        self.assertTrue(defgen.is_emissions_relevant("Fueling - Closed Loop"))
        self.assertTrue(defgen.is_emissions_relevant("fueling - cl/ol transition"))
        self.assertTrue(defgen.is_emissions_relevant("fueling - a/f learning"))
        self.assertTrue(defgen.is_emissions_relevant("OBD-II"))
        self.assertTrue(defgen.is_emissions_relevant("Fueling - AF Correction / Learning"))

    def test_open_loop_and_safety_categories_are_not_flagged(self):
        # Edits to these affect emissions indirectly but flagging them would
        # drown out the genuinely-emissions edits.
        self.assertFalse(defgen.is_emissions_relevant("Fueling - Primary Open Loop"))
        self.assertFalse(defgen.is_emissions_relevant("Ignition Timing - Knock Control"))
        self.assertFalse(defgen.is_emissions_relevant("Boost Control - Target"))
        self.assertFalse(defgen.is_emissions_relevant("Idle Control"))

    def test_dtc_is_not_flagged_emissions(self):
        # DTC editing is a separate concern; see comment in defgen.
        self.assertFalse(defgen.is_emissions_relevant("Diagnostic Trouble Codes"))

    def test_empty_category_is_not_flagged(self):
        self.assertFalse(defgen.is_emissions_relevant(""))
        self.assertFalse(defgen.is_emissions_relevant("   "))

    def test_table_record_carries_flag_from_xml(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="cl_target" category="Fueling - Closed Loop"
                 storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="afr" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="cl_target_x" storageaddress="0x200"
                   storagetype="uint8" endian="big" size="4"/>
          </table>
          <table type="2D" name="boost_target" category="Boost Control - Target"
                 storageaddress="0x300" storagetype="uint8" endian="big">
            <scaling units="kpa" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="boost_x" storageaddress="0x400"
                   storagetype="uint8" endian="big" size="4"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        flags = {t.id: t.emissions_relevant for t in packs[0].tables}
        self.assertTrue(flags["cl_target"])
        self.assertFalse(flags["boost_target"])


class EngineSafetyHeuristicTest(unittest.TestCase):
    def test_knock_control_is_flagged(self):
        self.assertTrue(defgen.is_engine_safety_critical(
            "Ignition Timing - Knock Control"))
        # Sub-categories that name knock control elsewhere also hit.
        self.assertTrue(defgen.is_engine_safety_critical(
            "Some Section - Knock Control - Detail"))

    def test_limits_categories_are_flagged(self):
        # Both `boost control - limits` and `miscellaneous - limits` should
        # match the " - limits" suffix.
        self.assertTrue(defgen.is_engine_safety_critical(
            "Boost Control - Limits"))
        self.assertTrue(defgen.is_engine_safety_critical(
            "Miscellaneous - Limits"))

    def test_checksum_is_flagged(self):
        self.assertTrue(defgen.is_engine_safety_critical("Checksum Fix"))

    def test_non_safety_categories_are_not_flagged(self):
        # Tuning surface that doesn't directly disable a protection
        # mechanism. False-positives on the safety side would block
        # legitimate edits across every profile.
        self.assertFalse(defgen.is_engine_safety_critical(
            "Ignition Timing - Advance"))
        self.assertFalse(defgen.is_engine_safety_critical(
            "Fueling - Closed Loop"))
        self.assertFalse(defgen.is_engine_safety_critical(
            "Boost Control - Target"))
        self.assertFalse(defgen.is_engine_safety_critical(
            "Miscellaneous - Thresholds"))
        self.assertFalse(defgen.is_engine_safety_critical(""))

    def test_table_record_carries_both_flags(self):
        # A knock-control table (engine-safety) should pop both flags
        # cleanly: safety=true, emissions=false. Closed-loop fueling pops
        # the inverse pair. Plain ignition advance pops neither.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="knock_retard" category="Ignition Timing - Knock Control"
                 storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="deg" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="knock_x" storageaddress="0x200"
                   storagetype="uint8" endian="big" size="4"/>
          </table>
          <table type="2D" name="cl_target" category="Fueling - Closed Loop"
                 storageaddress="0x300" storagetype="uint8" endian="big">
            <scaling units="afr" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="cl_x" storageaddress="0x400"
                   storagetype="uint8" endian="big" size="4"/>
          </table>
          <table type="2D" name="adv" category="Ignition Timing - Advance"
                 storageaddress="0x500" storagetype="uint8" endian="big">
            <scaling units="deg" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="adv_x" storageaddress="0x600"
                   storagetype="uint8" endian="big" size="4"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        flags = {t.id: (t.emissions_relevant, t.engine_safety_critical)
                 for t in packs[0].tables}
        self.assertEqual(flags["knock_retard"], (False, True))
        self.assertEqual(flags["cl_target"],    (True,  False))
        self.assertEqual(flags["adv"],          (False, False))


class DimensionsTest(unittest.TestCase):
    def test_romraider_1d_maps_to_scalar(self):
        # RomRaider counts the value dimension; "1D" = single scalar with
        # no axis. Our schema needs dimensions=0 so the C++ validator
        # doesn't demand an axis_x.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="1D" name="rev_limit" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="rpm" expression="(x)*100.0" to_byte="(x)/100.0"
                     format="0" endian="big" storagetype="uint8"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        self.assertEqual(len(packs), 1)
        self.assertEqual(len(packs[0].tables), 1)
        self.assertEqual(packs[0].tables[0].dimensions, 0)
        self.assertEqual(packs[0].tables[0].axis_x, "")
        self.assertEqual(packs[0].tables[0].axis_y, "")

    def test_romraider_2d_maps_to_one_axis(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="boost" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="kPa" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="boost_x" storageaddress="0x200" storagetype="uint8" endian="big" size="8">
              <scaling units="rpm" expression="x" to_byte="x"
                       format="0" endian="big" storagetype="uint8"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        self.assertEqual(packs[0].tables[0].dimensions, 1)
        self.assertEqual(packs[0].tables[0].axis_x, "boost_x")

    def test_romraider_3d_maps_to_two_axes(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="3D" name="vvt" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="deg" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="vvt_x" storageaddress="0x200" storagetype="uint8" endian="big" size="8">
              <scaling units="rpm" expression="x" to_byte="x"
                       format="0" endian="big" storagetype="uint8"/>
            </table>
            <table type="Y Axis" name="vvt_y" storageaddress="0x208" storagetype="uint8" endian="big" size="8">
              <scaling units="load" expression="x" to_byte="x"
                       format="0" endian="big" storagetype="uint8"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        self.assertEqual(packs[0].tables[0].dimensions, 2)
        self.assertEqual(packs[0].tables[0].axis_x, "vvt_x")
        self.assertEqual(packs[0].tables[0].axis_y, "vvt_y")

    def test_2d_with_unresolved_axis_demotes_to_scalar_with_warning(self):
        # RomRaider's scrubbed XML can leave a 2D table with an X-axis
        # element that has no storageaddress (no inherited override either).
        # The table can't be read as a multi-axis lookup; defgen demotes
        # to scalar so the pack still validates, with a warning so the user
        # can hand-edit the axis address later.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="ghost" storageaddress="0x100" storagetype="uint8" endian="big" sizey="8">
            <scaling units="%" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint8"/>
            <table type="X Axis" name="ghost_x" storagetype="uint16" endian="big" size="8">
              <scaling units="C" expression="x" to_byte="x"
                       format="0" endian="big" storagetype="uint16"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        self.assertEqual(packs[0].tables[0].dimensions, 0)
        self.assertEqual(packs[0].tables[0].axis_x, "")
        table_warnings = [w for w in packs[0].warnings if w[0] == "table"]
        self.assertTrue(
            any(w[1] == "ghost" and "demoted to scalar" in w[2] for w in table_warnings),
            f"expected demote warning for ghost; got {table_warnings!r}",
        )


class ScalingMinMaxOmissionTest(unittest.TestCase):
    """RomRaider XML often omits min= / max= on scalings (the canonical Merp
    ecu_defs.xml does so for ~all table scalings). defgen used to emit
    `min = 0.0` / `max = 0.0` regardless, making every scaling look like
    it had a 0..0 deadband range. Now: omit the field when the source
    didn't carry it. The C++ loader already tolerates absent keys."""

    def test_present_min_max_emitted(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="1D" name="t" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling name="bounded" units="rpm" expression="x" to_byte="x"
                     format="0" min="100" max="9000" endian="big" storagetype="uint16"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        s = next(s for s in packs[0].scalings if s.id == "bounded")
        self.assertEqual(s.minimum, 100.0)
        self.assertEqual(s.maximum, 9000.0)
        toml_text = defgen.pack_to_toml(packs[0])
        self.assertIn("min       = 100.0", toml_text)
        self.assertIn("max       = 9000.0", toml_text)

    def test_absent_min_max_omitted(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="1D" name="t" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling name="unbounded" units="" expression="x" to_byte="x"
                     format="0" endian="big" storagetype="uint16"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        s = next(s for s in packs[0].scalings if s.id == "unbounded")
        self.assertIsNone(s.minimum)
        self.assertIsNone(s.maximum)
        toml_text = defgen.pack_to_toml(packs[0])
        # No `min = ` / `max = ` line should appear in the emitted scaling
        # block — but the table block can still reference axis x/y. Slice
        # the unbounded scaling's own block.
        block = toml_text.split('id        = "unbounded"', 1)[1]
        block = block.split("[[", 1)[0]
        self.assertNotIn("min ", block)
        self.assertNotIn("max ", block)

    def test_partial_min_only(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="1D" name="t" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling name="lower_only" units="" expression="x" to_byte="x"
                     format="0" min="0" endian="big" storagetype="uint16"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        s = next(s for s in packs[0].scalings if s.id == "lower_only")
        self.assertEqual(s.minimum, 0.0)
        self.assertIsNone(s.maximum)
        toml_text = defgen.pack_to_toml(packs[0])
        block = toml_text.split('id        = "lower_only"', 1)[1]
        block = block.split("[[", 1)[0]
        self.assertIn("min       = 0.0", block)
        self.assertNotIn("max ", block)

    def test_empty_string_min_max_omitted(self):
        # `<scaling min="" max="">` is rare but appeared in the wild; the
        # old `float(el.get("min") or 0.0)` swallowed it as 0.0. We now
        # treat empty string the same as missing — omit from output.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="1D" name="t" storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling name="empty_attrs" units="" expression="x" to_byte="x"
                     format="0" min="" max="" endian="big" storagetype="uint16"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        s = next(s for s in packs[0].scalings if s.id == "empty_attrs")
        self.assertIsNone(s.minimum)
        self.assertIsNone(s.maximum)

    def test_emit_table_skips_none(self):
        # Direct unit on _emit_table — None-valued keys drop out.
        out = defgen._emit_table("[[x]]", {"a": 1, "b": None, "c": "v"})
        self.assertIn("a = 1", out)
        self.assertIn('c = "v"', out)
        self.assertNotIn("b ", out)


class DisplayNameFallbackTest(unittest.TestCase):
    """When the OEM name fails the clean-room filter (too long, contains a
    period or comma), we used to emit `name = ""`. That made `pack-info`
    and `table-list` show blank rows for the entries that needed a label
    most (DTC tables with terse abbreviation-laden descriptions). Fall back
    to title-cased slug instead — same factual content, readable form."""

    def test_short_factual_name_is_kept(self):
        self.assertEqual(defgen._display_name("Boost Target", "boost_target"),
                         "Boost Target")

    def test_long_name_falls_back_to_titlecased_slug(self):
        long_name = "A" * 70  # > 64 chars
        slug = "boost_target_high_octane"
        self.assertEqual(defgen._display_name(long_name, slug),
                         "Boost Target High Octane")

    def test_name_with_period_falls_back(self):
        self.assertEqual(
            defgen._display_name("Neutral pos. switch high input", "neutral_pos_switch_high_input"),
            "Neutral Pos Switch High Input",
        )

    def test_name_with_comma_falls_back(self):
        self.assertEqual(
            defgen._display_name("Boost, target, high octane", "boost_target_high_octane"),
            "Boost Target High Octane",
        )

    def test_empty_slug_yields_empty_string(self):
        # Defensive: if the slug somehow ended up empty too, don't crash.
        self.assertEqual(defgen._display_name("A" * 70, ""), "")

    def test_table_with_rejected_name_uses_titlecased_slug(self):
        # End-to-end: a DTC table whose name contains a period should still
        # come out of parse_rom_xml with a readable name.
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="1D" name="P1590 Neutral pos. switch high input" category="diagnostic trouble codes"
                 storageaddress="0x100" storagetype="uint8" endian="big">
            <scaling units="" expression="x" to_byte="x" format="0"
                     endian="big" storagetype="uint8"/>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        table = packs[0].tables[0]
        self.assertEqual(table.id, "p1590_neutral_pos_switch_high_input")
        self.assertEqual(table.name, "P1590 Neutral Pos Switch High Input")

    def test_axis_with_rejected_name_uses_titlecased_slug(self):
        xml = """<roms><rom>
          <romid><xmlid>X</xmlid><internalidaddress>0x0</internalidaddress>
            <internalidstring>X</internalidstring></romid>
          <table type="2D" name="boost_target" storageaddress="0x100"
                 storagetype="uint8" endian="big" sizex="8">
            <scaling units="" expression="x" to_byte="x" format="0"
                     endian="big" storagetype="uint8"/>
            <table type="X Axis" name="RPM, primary axis"
                   storageaddress="0x200" storagetype="uint16" endian="big" size="8">
              <scaling units="rpm" expression="x" to_byte="x" format="0"
                       endian="big" storagetype="uint16"/>
            </table>
          </table>
        </rom></roms>"""
        packs = defgen.parse_rom_xml(xml)
        axis = next(a for a in packs[0].axes if a.id == "rpm_primary_axis")
        self.assertEqual(axis.name, "Rpm Primary Axis")


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
