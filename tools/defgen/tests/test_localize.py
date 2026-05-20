# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Unit tests for localize.py — focus on the v2 pattern-search relocator."""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import localize  # noqa: E402


def _make_rom(size: int = 4096, seed: int = 0) -> bytearray:
    """Deterministic 'random-looking' filler so unrelated bytes
    don't accidentally pass the relocator's classify_pair check."""
    rng = bytearray()
    x = seed or 1
    for _ in range(size):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        rng.append((x >> 16) & 0xFF)
    return rng


def _structured_table(start: int = 0x10) -> bytes:
    """Synthetic 'fuel map' — structured bytes that classify_pair
    will recognise as table-shaped (low entropy, small range)."""
    return bytes((start + i) & 0xFF for i in range(64))


class RelocateExactMatchTests(unittest.TestCase):
    """The common case: table moved without changing content."""

    def test_exact_match_at_shifted_offset(self):
        # Sibling has a table at 0x100; target has the SAME bytes at 0x200.
        table = _structured_table()
        sib = _make_rom(0x1000, seed=11)
        tgt = _make_rom(0x1000, seed=22)
        sib[0x100:0x100 + len(table)] = table
        tgt[0x200:0x200 + len(table)] = table

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint8", sample_count=16)

        self.assertIsNotNone(result)
        new_addr, conf, reason = result
        self.assertEqual(new_addr, 0x200)
        self.assertIn(conf, ("HIGH", "MED"))
        self.assertIn("relocated", reason)

    def test_no_relocation_when_table_didnt_move(self):
        """Caller already classified the original address; the
        relocator must not return pack_address as the answer
        (handled by the explicit cand_addr == pack_address skip)."""
        table = _structured_table()
        sib = _make_rom(0x1000, seed=11)
        tgt = _make_rom(0x1000, seed=22)
        sib[0x100:0x100 + len(table)] = table
        tgt[0x100:0x100 + len(table)] = table

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint8", sample_count=16)

        # Only candidate hit IS pack_address; relocator excludes it.
        self.assertIsNone(result)


class RelocateAnchorTierTests(unittest.TestCase):
    """Leading-anchor should be tried first; interior anchors are a
    fallback for uniform-prefix tables."""

    def test_interior_anchor_when_leading_is_padding(self):
        # Sibling has 32 bytes of zero padding then real content.
        padded = bytes(32) + _structured_table()[:64]
        sib = _make_rom(0x1000, seed=11)
        tgt = _make_rom(0x1000, seed=22)
        sib[0x100:0x100 + len(padded)] = padded
        # Target keeps the same shape but shifted by 0x80
        tgt[0x180:0x180 + len(padded)] = padded

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint8", sample_count=16)

        self.assertIsNotNone(result)
        new_addr, _, _ = result
        # Either the leading anchor finds 0x180 (zero-padding is filtered
        # out by _is_anchorable, so it should fall through to interior),
        # OR an interior anchor at offset 32 finds the content at 0x1A0
        # and backs up by 32 to 0x180. Both should resolve to 0x180.
        self.assertEqual(new_addr, 0x180)

    def test_no_anchor_when_table_is_uniform(self):
        """If every 16-byte window in the sibling slice is uniform,
        relocator gives up rather than match every padding region."""
        sib = _make_rom(0x1000, seed=11)
        tgt = _make_rom(0x1000, seed=22)
        sib[0x100:0x100 + 256] = bytes(256)  # all zeros — unanchorable
        tgt[0x200:0x200 + 256] = bytes(256)

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint8", sample_count=16)

        self.assertIsNone(result)


class RelocateDistanceCapTests(unittest.TestCase):
    """max_distance must reject candidates far from the original
    address even if they're a perfect content match."""

    def test_far_candidate_rejected(self):
        table = _structured_table()
        sib = _make_rom(0x10000, seed=11)
        tgt = _make_rom(0x10000, seed=22)
        sib[0x100:0x100 + len(table)] = table
        # Plant the match WAY outside the default 64 KB cap (which is
        # the size of this ROM minus a hair, so we use a tight cap).
        tgt[0x8000:0x8000 + len(table)] = table

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint8", sample_count=16,
            max_distance=0x100)

        self.assertIsNone(result)

    def test_far_candidate_accepted_with_wider_cap(self):
        table = _structured_table()
        sib = _make_rom(0x10000, seed=11)
        tgt = _make_rom(0x10000, seed=22)
        sib[0x100:0x100 + len(table)] = table
        tgt[0x8000:0x8000 + len(table)] = table

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint8", sample_count=16,
            max_distance=0x10000)

        self.assertIsNotNone(result)
        new_addr, _, _ = result
        self.assertEqual(new_addr, 0x8000)


class RelocateAlignmentTests(unittest.TestCase):
    """Candidates must be dtype-aligned."""

    def test_misaligned_match_rejected(self):
        # uint16_be table; plant a match at an odd address.
        table = _structured_table()
        sib = _make_rom(0x1000, seed=11)
        tgt = _make_rom(0x1000, seed=22)
        sib[0x100:0x100 + len(table)] = table
        # Plant ONLY at an odd address — should be rejected for uint16
        tgt[0x201:0x201 + len(table)] = table

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x100,
            dtype="uint16_be", sample_count=16)

        # No dtype-aligned candidate within range, so relocator can't help.
        self.assertIsNone(result)


class RelocateClosestWinsTests(unittest.TestCase):
    """When multiple candidates score the same confidence, the one
    closest to the original address wins."""

    def test_closer_candidate_preferred(self):
        table = _structured_table()
        sib = _make_rom(0x10000, seed=11)
        tgt = _make_rom(0x10000, seed=22)
        sib[0x1000:0x1000 + len(table)] = table
        # Plant TWO identical matches at different distances
        tgt[0x1100:0x1100 + len(table)] = table  # +0x100 from original
        tgt[0x1800:0x1800 + len(table)] = table  # +0x800 from original

        result = localize.relocate_low(
            bytes(sib), bytes(tgt), pack_address=0x1000,
            dtype="uint8", sample_count=16,
            max_distance=0x10000)

        self.assertIsNotNone(result)
        new_addr, _, _ = result
        self.assertEqual(new_addr, 0x1100)  # closer wins


class HelperTests(unittest.TestCase):
    """Small unit tests for the helpers."""

    def test_find_all_collects_all_hits(self):
        data = b"AAxxAAxxAAxx"
        self.assertEqual(localize._find_all(data, b"AA"), [0, 4, 8])

    def test_find_all_capped(self):
        data = b"AA" * 100
        hits = localize._find_all(data, b"AA", max_hits=5)
        self.assertEqual(len(hits), 5)

    def test_is_anchorable_rejects_uniform(self):
        self.assertFalse(localize._is_anchorable(bytes(16)))           # all 00
        self.assertFalse(localize._is_anchorable(b"\xff" * 16))        # all FF
        self.assertFalse(localize._is_anchorable(b"\x00\xff" * 8))     # 2-value pattern
        self.assertFalse(localize._is_anchorable(b"\x00\x01\x02" * 6)) # 3-value pattern (still <4)

    def test_is_anchorable_accepts_diverse(self):
        self.assertTrue(localize._is_anchorable(bytes(range(16))))
        self.assertTrue(localize._is_anchorable(b"\x00\x01\x02\x03" * 4))


if __name__ == "__main__":
    unittest.main()
