# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for tools/defgen/apply_offset.py.

Run with:
    python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path


_THIS = Path(__file__).resolve()
_APPLY_OFFSET = _THIS.parent.parent / "defgen" / "apply_offset.py"
_spec = importlib.util.spec_from_file_location("apply_offset", _APPLY_OFFSET)
assert _spec is not None and _spec.loader is not None
apply_offset = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = apply_offset
_spec.loader.exec_module(apply_offset)


SAMPLE_PACK = """\
[pack]
id = "test"
endianness = "big"

[[identification]]
name        = "TEST"
cid_address = 0x00000000
cid_length  = 8

[[table]]
id      = "foo"
address = 0x00078000
data_type = "uint16_be"

[[table]]
id      = "bar"
address = 0x000FFFFF
data_type = "uint8"
"""


class TestParseSignedOffset(unittest.TestCase):
    def test_negative_hex(self) -> None:
        self.assertEqual(apply_offset.parse_signed_offset("-0x6000"), -24576)

    def test_positive_hex(self) -> None:
        self.assertEqual(apply_offset.parse_signed_offset("0x6000"), 24576)
        self.assertEqual(apply_offset.parse_signed_offset("+0x6000"), 24576)

    def test_decimal(self) -> None:
        self.assertEqual(apply_offset.parse_signed_offset("-24576"), -24576)
        self.assertEqual(apply_offset.parse_signed_offset("100"), 100)

    def test_zero(self) -> None:
        self.assertEqual(apply_offset.parse_signed_offset("0"), 0)
        self.assertEqual(apply_offset.parse_signed_offset("0x0"), 0)


class TestShiftPack(unittest.TestCase):
    def test_negative_offset_shifts_addresses_down(self) -> None:
        text, shifted, clamped = apply_offset.shift_pack(SAMPLE_PACK, -0x6000)
        self.assertEqual(shifted, 3)
        self.assertEqual(clamped, 1)
        # cid_address 0x0 - 0x6000 clamps to 0
        self.assertIn("cid_address = 0x00000000", text)
        # 0x78000 - 0x6000 = 0x72000
        self.assertIn("address = 0x00072000", text)
        # 0xFFFFF - 0x6000 = 0xF9FFF
        self.assertIn("address = 0x000F9FFF", text)

    def test_positive_offset_shifts_addresses_up(self) -> None:
        text, shifted, clamped = apply_offset.shift_pack(SAMPLE_PACK, 0x1000)
        self.assertEqual(shifted, 3)
        self.assertEqual(clamped, 0)
        self.assertIn("cid_address = 0x00001000", text)
        self.assertIn("address = 0x00079000", text)

    def test_zero_offset_is_no_op(self) -> None:
        text, shifted, clamped = apply_offset.shift_pack(SAMPLE_PACK, 0)
        self.assertEqual(shifted, 3)
        self.assertEqual(clamped, 0)
        # Format may differ (8-digit zero-padded) so just verify the
        # numeric value via re-parse: original hex values are preserved.
        for original_hex in ("0x00078000", "0x000FFFFF"):
            old = int(original_hex, 16)
            # All originals must appear somewhere in the new text in
            # the new uppercase 8-hex format.
            self.assertIn(f"0x{old:08X}", text)

    def test_non_address_lines_untouched(self) -> None:
        text, _shifted, _clamped = apply_offset.shift_pack(SAMPLE_PACK, -0x6000)
        # cid_length is NOT an address field; must be unchanged.
        self.assertIn("cid_length  = 8", text)
        self.assertIn('id      = "foo"', text)
        self.assertIn('data_type = "uint16_be"', text)
        self.assertIn('endianness = "big"', text)

    def test_clamp_warning_count(self) -> None:
        # Two address lines below 0x6000 -> both get clamped.
        pack = (
            "[[table]]\n"
            'id = "low"\n'
            "address = 0x00000100\n"
            "[[table]]\n"
            'id = "mid"\n'
            "address = 0x00005FFF\n"
            "[[table]]\n"
            'id = "high"\n'
            "address = 0x00010000\n"
        )
        _text, shifted, clamped = apply_offset.shift_pack(pack, -0x6000)
        self.assertEqual(shifted, 3)
        self.assertEqual(clamped, 2)

    def test_offset_preserves_line_count(self) -> None:
        # Round-trip should not add or remove lines.
        text, _, _ = apply_offset.shift_pack(SAMPLE_PACK, -0x6000)
        self.assertEqual(text.count("\n"), SAMPLE_PACK.count("\n"))


class TestCli(unittest.TestCase):
    def test_full_run_to_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            in_path = tdp / "in.toml"
            out_path = tdp / "out.toml"
            in_path.write_text(SAMPLE_PACK, encoding="utf-8")

            captured = io.StringIO()
            with redirect_stderr(captured):
                rc = apply_offset.main(
                    [str(in_path), "--offset", "-0x6000", "--output", str(out_path)]
                )
            self.assertEqual(rc, 0)
            stderr = captured.getvalue()
            self.assertIn("shifted 3 addresses by -0x6000", stderr)
            self.assertIn("WARNING", stderr)  # cid_address clamp
            self.assertIn(f"wrote {out_path}", stderr)

            out_text = out_path.read_text(encoding="utf-8")
            self.assertIn("address = 0x00072000", out_text)

    def test_missing_input_exits_one(self) -> None:
        captured = io.StringIO()
        with redirect_stderr(captured):
            rc = apply_offset.main(
                ["/nonexistent/path.toml", "--offset", "-0x6000"]
            )
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
