# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Unit tests for bulk_regen.py — focus on the [pack]-field preservation
pass added 2026-05-20 after the rom_size_bytes regression (commits
0008695 + 8ea6d71)."""

from __future__ import annotations

import sys
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import bulk_regen  # noqa: E402


def _pack_with(rom_size_bytes: str, transmission: str = '"mt"',
               years: str = "[9]") -> str:
    return textwrap.dedent(f"""\
        [pack]
        schema_version = 1
        id             = "test"
        display_name   = "TEST"
        platform       = "subaru.legacy"
        transmission   = {transmission}
        years          = {years}
        endianness     = "big"
        rom_size_bytes = {rom_size_bytes}
        license        = "Apache-2.0"

        [[identification]]
        name        = "TEST"
        cid_address = 0x00002000
    """)


class ExtractPackFieldTests(unittest.TestCase):

    def test_extract_rom_size_bytes(self):
        text = _pack_with("1048576")
        result = bulk_regen._extract_pack_field(text, "rom_size_bytes")
        self.assertIsNotNone(result)
        full, val = result
        self.assertEqual(val, "1048576")

    def test_extract_returns_none_for_missing_key(self):
        text = _pack_with("1048576")
        self.assertIsNone(
            bulk_regen._extract_pack_field(text, "no_such_field"))

    def test_extract_only_searches_pack_section(self):
        """A `rom_size_bytes` lookup must NOT match a like-named key in
        any other section that happens to have one."""
        text = _pack_with("1048576") + textwrap.dedent("""

            [[table]]
            id             = "fake"
            address        = 0x1000
            rom_size_bytes = 9999
        """)
        result = bulk_regen._extract_pack_field(text, "rom_size_bytes")
        self.assertIsNotNone(result)
        full, val = result
        self.assertEqual(val, "1048576")  # NOT 9999


class ReplacePackFieldTests(unittest.TestCase):

    def test_replace_preserves_surrounding_text(self):
        text = _pack_with("0")
        out = bulk_regen._replace_pack_field(text, "rom_size_bytes", "1048576")
        self.assertIn("rom_size_bytes = 1048576", out)
        self.assertNotIn("rom_size_bytes = 0", out)
        # Other fields untouched
        self.assertIn('transmission   = "mt"', out)
        self.assertIn("years          = [9]", out)

    def test_replace_noop_on_missing_section(self):
        text = "no pack section here at all\n"
        self.assertEqual(
            bulk_regen._replace_pack_field(text, "rom_size_bytes", "1048576"),
            text)


class FieldPreservationTests(unittest.TestCase):
    """The core regression-prevention rule: when defgen emits an empty
    /default value for a [pack] field but the existing pack had a
    manually-patched non-empty value, keep the existing value."""

    def test_rom_size_bytes_preserved_when_regen_emits_zero(self):
        existing = _pack_with("1048576")
        regenerated = _pack_with("0")  # defgen default for missing <filesize>
        out, preservations = bulk_regen._apply_field_preservation(
            existing, regenerated)
        self.assertIn("rom_size_bytes = 1048576", out)
        self.assertEqual(len(preservations), 1)
        self.assertEqual(preservations[0][0], "rom_size_bytes")

    def test_transmission_preserved_when_regen_emits_empty(self):
        existing = _pack_with("1048576", transmission='"at"')
        regenerated = _pack_with("1048576", transmission='""')
        out, preservations = bulk_regen._apply_field_preservation(
            existing, regenerated)
        self.assertIn('transmission   = "at"', out)
        self.assertEqual([p[0] for p in preservations], ["transmission"])

    def test_years_preserved_when_regen_emits_empty_list(self):
        existing = _pack_with("1048576", years="[9, 10]")
        regenerated = _pack_with("1048576", years="[]")
        out, preservations = bulk_regen._apply_field_preservation(
            existing, regenerated)
        self.assertIn("years          = [9, 10]", out)
        self.assertEqual([p[0] for p in preservations], ["years"])

    def test_no_preservation_when_regen_has_real_value(self):
        """If defgen's regen produces a non-empty value (i.e. the XML
        DID supply one), trust it — don't overlay the existing."""
        existing = _pack_with("1048576")
        regenerated = _pack_with("2097152")  # defgen says 2MB
        out, preservations = bulk_regen._apply_field_preservation(
            existing, regenerated)
        # Should keep the regenerated 2097152, NOT preserve the existing
        self.assertIn("rom_size_bytes = 2097152", out)
        self.assertNotIn("rom_size_bytes = 1048576", out)
        self.assertEqual(preservations, [])

    def test_no_preservation_when_existing_also_empty(self):
        existing = _pack_with("0")
        regenerated = _pack_with("0")
        out, preservations = bulk_regen._apply_field_preservation(
            existing, regenerated)
        self.assertEqual(out, regenerated)
        self.assertEqual(preservations, [])

    def test_multiple_fields_preserved_simultaneously(self):
        existing = _pack_with("1048576", transmission='"at"', years="[2009]")
        regenerated = _pack_with("0", transmission='""', years="[]")
        out, preservations = bulk_regen._apply_field_preservation(
            existing, regenerated)
        self.assertIn("rom_size_bytes = 1048576", out)
        self.assertIn('transmission   = "at"', out)
        self.assertIn("years          = [2009]", out)
        self.assertEqual(len(preservations), 3)
        names = {p[0] for p in preservations}
        self.assertEqual(names, {"rom_size_bytes", "transmission", "years"})


if __name__ == "__main__":
    unittest.main()
