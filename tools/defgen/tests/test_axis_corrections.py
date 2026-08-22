# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for firmware-derived axis-length corrections."""

import sys
import unittest
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import axis_corrections  # noqa: E402


@dataclass
class _FakeAxis:
    address: int
    length: int


class LoadAxisCorrectionsTest(unittest.TestCase):
    def test_parses_bundled_tsv(self):
        corr = axis_corrections.load_axis_corrections(
            axis_corrections.DEFAULT_CORRECTIONS
        )
        # Header skipped; every entry is address -> positive firmware length.
        self.assertTrue(corr)
        self.assertTrue(all(v > 0 for v in corr.values()))
        # Spot-check a known correction (wastegate Y axis, 7 -> 14).
        self.assertEqual(corr[0x31314], 14)

    def test_parses_from_text(self):
        tsv = "axis_addr\tfirmware_len\tdef_len\tratio\n" "0x1000\t14\t7\t2.0\n"
        p = Path(self._tmp("corr.tsv", tsv))
        corr = axis_corrections.load_axis_corrections(p)
        self.assertEqual(corr, {0x1000: 14})

    def test_skips_blank_and_comment_lines(self):
        tsv = "0x10\t8\n\n# a note\n0x20\t9\n"
        corr = axis_corrections.load_axis_corrections(Path(self._tmp("c.tsv", tsv)))
        self.assertEqual(corr, {0x10: 8, 0x20: 9})

    def test_rejects_malformed_row(self):
        with self.assertRaises(ValueError):
            axis_corrections.load_axis_corrections(
                Path(self._tmp("bad.tsv", "0xZZ\tnope\n"))
            )

    def test_rejects_nonpositive_length(self):
        with self.assertRaises(ValueError):
            axis_corrections.load_axis_corrections(
                Path(self._tmp("bad.tsv", "0x10\t0\n"))
            )

    def _tmp(self, name: str, text: str) -> str:
        import tempfile

        d = tempfile.mkdtemp()
        p = Path(d) / name
        p.write_text(text, encoding="utf-8")
        return str(p)


class ApplyAxisCorrectionsTest(unittest.TestCase):
    def test_overrides_matching_axis_length(self):
        axes = [_FakeAxis(0x31314, 7), _FakeAxis(0x999, 5)]
        changed = axis_corrections.apply_axis_corrections(axes, {0x31314: 14})
        self.assertEqual(changed, [(0x31314, 7, 14)])
        self.assertEqual(axes[0].length, 14)  # corrected
        self.assertEqual(axes[1].length, 5)   # untouched (no correction)

    def test_is_idempotent(self):
        axes = [_FakeAxis(0x31314, 7)]
        axis_corrections.apply_axis_corrections(axes, {0x31314: 14})
        # Second pass: already at firmware length -> no change reported.
        changed = axis_corrections.apply_axis_corrections(axes, {0x31314: 14})
        self.assertEqual(changed, [])
        self.assertEqual(axes[0].length, 14)

    def test_no_corrections_is_noop(self):
        axes = [_FakeAxis(0x10, 3)]
        self.assertEqual(axis_corrections.apply_axis_corrections(axes, {}), [])
        self.assertEqual(axes[0].length, 3)


if __name__ == "__main__":
    unittest.main()
