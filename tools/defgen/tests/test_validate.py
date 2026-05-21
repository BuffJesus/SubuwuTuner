# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Unit tests for validate.py.

Tests build a synthetic single-file pack TOML + a small ROM buffer in
tempfiles, then exercise the loader, single-entry validator, and main()
exit-code paths. No dependence on the real corpus.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import descriptors  # noqa: E402
import validate  # noqa: E402


def _pack_uint16_be(values: list[int]) -> bytes:
    return b"".join(struct.pack(">H", v) for v in values)


def _pack_floats_be(values: list[float]) -> bytes:
    return b"".join(struct.pack(">f", v) for v in values)


# A synthetic mini-pack with one each of: matching axis, matching 2D table,
# matching 2D table that will fail (bad bytes), and a table with no
# descriptor in the registry.
_PACK_TOML = textwrap.dedent("""\
    [pack]
    schema_version = 1
    id             = "synth"
    display_name   = "SYNTH"
    platform       = "subaru.synth"
    rom_size_bytes = 4096
    endianness     = "big"
    license        = "Apache-2.0"

    [[identification]]
    name        = "SYNTH"
    cid_address = 0x0
    cid_length  = 4
    cid_match   = "SYNT"

    [[axis]]
    id        = "engine_speed"
    name      = "Engine Speed"
    type      = "static"
    address   = 0x0100
    length    = 8
    data_type = "float32_be"

    [[axis]]
    id        = "load_axis"
    name      = "Load"
    type      = "static"
    address   = 0x0200
    length    = 8
    data_type = "float32_be"

    [[table]]
    id                     = "max_wastegate_duty"
    name                   = "Max Wastegate Duty"
    category               = "boost"
    dimensions             = 2
    address                = 0x0300
    data_type              = "uint16_be"
    scaling                = ""
    axis_x                 = "engine_speed"
    axis_y                 = "load_axis"
    emissions_relevant     = false
    engine_safety_critical = false

    [[table]]
    id                     = "max_wastegate_duty_broken"
    name                   = "Max Wastegate Duty Broken"
    category               = "boost"
    dimensions             = 2
    address                = 0x0500
    data_type              = "uint16_be"
    scaling                = ""
    axis_x                 = "engine_speed"
    axis_y                 = "load_axis"
    emissions_relevant     = false
    engine_safety_critical = false

    [[table]]
    id                     = "no_descriptor_for_me"
    name                   = "Unmatched"
    category               = "misc"
    dimensions             = 2
    address                = 0x0700
    data_type              = "uint16_be"
    scaling                = ""
    axis_x                 = "engine_speed"
    axis_y                 = "load_axis"
    emissions_relevant     = false
    engine_safety_critical = false
""")


def _build_rom() -> bytes:
    """Assemble a 4 KB ROM with the bytes each table expects to read."""
    rom = bytearray(4096)
    # Engine RPM axis at 0x0100: 8 floats, monotonic 800..6400 RPM.
    rpms = [800.0, 1200.0, 1600.0, 2400.0, 3200.0, 4000.0, 5200.0, 6400.0]
    rom[0x0100:0x0100 + 8 * 4] = _pack_floats_be(rpms)
    # Load axis at 0x0200: 8 floats — not RPM-matching, descriptor for it
    # won't exist either, so it becomes NO_DESCRIPTOR.
    rom[0x0200:0x0200 + 8 * 4] = _pack_floats_be([0.5, 1.0, 1.5, 2.0,
                                                  2.5, 3.0, 3.5, 4.0])
    # Wastegate duty (good) at 0x0300: 8×8 uint16 spread, distinct.
    cells = [(i * 11 + 1000) for i in range(64)]
    rom[0x0300:0x0300 + 64 * 2] = _pack_uint16_be(cells)
    # Wastegate duty (broken) at 0x0500: all 0xFFFF — out-of-band.
    broken = [0xFFFF] * 64
    rom[0x0500:0x0500 + 64 * 2] = _pack_uint16_be(broken)
    return bytes(rom)


class LoadPackEntriesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".toml",
                                               delete=False, encoding="utf-8")
        self.tmp.write(_PACK_TOML)
        self.tmp.close()
        self.path = Path(self.tmp.name)

    def tearDown(self):
        self.path.unlink(missing_ok=True)

    def test_loads_axes_and_tables(self):
        entries, axis_len = validate.load_pack_entries(self.path)
        kinds = [e.kind for e in entries]
        self.assertEqual(kinds.count("axis"), 2)
        self.assertEqual(kinds.count("table"), 3)
        self.assertEqual(axis_len["engine_speed"], 8)
        self.assertEqual(axis_len["load_axis"], 8)

    def test_table_shape_resolved_from_axes(self):
        entries, _ = validate.load_pack_entries(self.path)
        table = next(e for e in entries if e.id == "max_wastegate_duty")
        self.assertEqual(table.rows, 8)
        self.assertEqual(table.cols, 8)
        self.assertEqual(table.dimensions, 2)


class ValidateEntryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".toml",
                                               delete=False, encoding="utf-8")
        self.tmp.write(_PACK_TOML)
        self.tmp.close()
        self.path = Path(self.tmp.name)
        self.rom = _build_rom()
        self.entries, _ = validate.load_pack_entries(self.path)

    def tearDown(self):
        self.path.unlink(missing_ok=True)

    def _entry(self, eid: str) -> validate.PackEntry:
        return next(e for e in self.entries if e.id == eid)

    def test_engine_speed_axis_passes(self):
        r = validate.validate_entry(self._entry("engine_speed"), self.rom)
        self.assertEqual(r.status, "PASS", msg=r.matched)

    def test_wastegate_duty_passes(self):
        r = validate.validate_entry(self._entry("max_wastegate_duty"),
                                    self.rom)
        self.assertEqual(r.status, "PASS", msg=r.matched)

    def test_broken_wastegate_fails(self):
        r = validate.validate_entry(self._entry("max_wastegate_duty_broken"),
                                    self.rom)
        self.assertEqual(r.status, "FAIL", msg=r.matched)
        self.assertTrue(any("outside" in reason for _, v in r.matched
                            for reason in v.reasons))

    def test_unmatched_table_returns_no_descriptor(self):
        r = validate.validate_entry(self._entry("no_descriptor_for_me"),
                                    self.rom)
        self.assertEqual(r.status, "NO_DESCRIPTOR")

    def test_address_overflow_skipped(self):
        # Build an entry whose address+len exceeds the 4 KB ROM
        # (0x0FE0 + 8×8×2 = 0x1060 > 0x1000).
        bad = validate.PackEntry(
            kind="table", id="overflow_test", address=0x0FE0,
            data_type="uint16_be", dimensions=2, rows=8, cols=8)
        r = validate.validate_entry(bad, self.rom)
        self.assertEqual(r.status, "SKIPPED")
        self.assertIn("exceeds ROM size", r.skip_reason)

    def test_zero_dim_scalar_skipped(self):
        scalar = validate.PackEntry(
            kind="table", id="some_scalar", address=0x0100,
            data_type="uint16_be", dimensions=0)
        r = validate.validate_entry(scalar, self.rom)
        self.assertEqual(r.status, "SKIPPED")


class MainExitCodeTest(unittest.TestCase):
    def setUp(self):
        self.tdir = tempfile.TemporaryDirectory()
        self.pack_path = Path(self.tdir.name) / "synth.toml"
        self.pack_path.write_text(_PACK_TOML, encoding="utf-8")
        self.rom_path = Path(self.tdir.name) / "synth.bin"
        self.rom_path.write_bytes(_build_rom())

    def tearDown(self):
        self.tdir.cleanup()

    def test_exit_1_when_a_fail_exists(self):
        rc = validate.main(["--pack", str(self.pack_path),
                            "--rom", str(self.rom_path),
                            "--tsv"])
        self.assertEqual(rc, 1)

    def test_exit_0_when_only_pass_or_no_descriptor(self):
        # Filter to a single PASS entry — no FAILs in scope.
        rc = validate.main(["--pack", str(self.pack_path),
                            "--rom", str(self.rom_path),
                            "--only", "engine_speed",
                            "--tsv"])
        self.assertEqual(rc, 0)

    def test_exit_2_on_missing_pack(self):
        rc = validate.main(["--pack", str(Path(self.tdir.name) / "nope.toml"),
                            "--rom", str(self.rom_path)])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
