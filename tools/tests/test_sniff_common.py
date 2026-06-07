# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for tools/sniff_common.py — sniff-log parser + ISO-TP reassembler."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

_THIS = Path(__file__).resolve()
_MOD_PATH = _THIS.parent.parent / "sniff_common.py"
_spec = importlib.util.spec_from_file_location("sniff_common", _MOD_PATH)
assert _spec is not None and _spec.loader is not None
sniff_common = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = sniff_common
_spec.loader.exec_module(sniff_common)


class TestParseLog(unittest.TestCase):
    def test_parses_well_formed_lines(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "cap.log"
            log.write_text(
                "# header comment\n"
                "100 0x7E0 02 27 01 00 00 00 00 00\n"
                "115 0x7E8 06 67 01 DE AD BE EF 00\n"
                "\n"
                "130  0x7E0  06 27 02 12 34 56 78 00\n",
                encoding="utf-8",
            )
            frames = sniff_common.parse_log(log)
        self.assertEqual(len(frames), 3)
        self.assertEqual(frames[0].ms, 100)
        self.assertEqual(frames[0].can_id, 0x7E0)
        self.assertEqual(frames[0].data[:2], b"\x02\x27")
        self.assertEqual(frames[2].ms, 130)

    def test_skips_malformed_lines(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "cap.log"
            log.write_text(
                "100 0x7E0 02 27 01\n"
                "garbage line\n"
                "110 0x7E8 02 67 01\n",
                encoding="utf-8",
            )
            frames = sniff_common.parse_log(log)
        self.assertEqual(len(frames), 2)


class TestSingleFrameStrip(unittest.TestCase):
    def test_strips_pci_and_returns_payload(self) -> None:
        # SF with length=3 carrying 27 01 00
        payload = sniff_common.strip_iso_tp_sf(b"\x03\x27\x01\x00\x00\x00\x00\x00")
        self.assertEqual(payload, b"\x27\x01\x00")

    def test_rejects_first_frame(self) -> None:
        # FF starts with 0x1XYY — strip_iso_tp_sf should refuse
        self.assertIsNone(sniff_common.strip_iso_tp_sf(b"\x10\x14\x67\x01\xDE\xAD\xBE\xEF"))

    def test_rejects_empty(self) -> None:
        self.assertIsNone(sniff_common.strip_iso_tp_sf(b""))


class TestIsoTpReassembler(unittest.TestCase):
    def test_single_frame_returns_payload(self) -> None:
        r = sniff_common.IsoTpReassembler()
        out = r.feed(0x7E8, b"\x03\x67\x01\x00\x00\x00\x00\x00")
        self.assertEqual(out, b"\x67\x01\x00")

    def test_multi_frame_reassembly(self) -> None:
        # Reassemble 20-byte payload: FF declares total=0x014, carries 6
        # bytes; two CFs carry 7+7 bytes. Total 6+7+7 = 20.
        # Header: 62 F1 90 (positive RDBI response for VIN-ish DID).
        # Body: 17 ASCII bytes A..Q.
        r = sniff_common.IsoTpReassembler()
        # FF: 10 14 [6 bytes: header + first 3 ASCII bytes A B C]
        out = r.feed(0x7E8, b"\x10\x14" + b"\x62\xF1\x90\x41\x42\x43")
        self.assertIsNone(out, "FF alone must not complete")
        # CF #1: 21 [7 bytes D..J]
        out = r.feed(0x7E8, b"\x21" + b"\x44\x45\x46\x47\x48\x49\x4A")
        self.assertIsNone(out, "first CF must not complete")
        # CF #2: 22 [7 bytes K..Q]
        out = r.feed(0x7E8, b"\x22" + b"\x4B\x4C\x4D\x4E\x4F\x50\x51")
        self.assertIsNotNone(out)
        self.assertEqual(len(out), 0x14)
        self.assertEqual(out[:3], b"\x62\xF1\x90")
        # 17-byte ASCII body A..Q.
        self.assertEqual(out[3:].decode("ascii"), "ABCDEFGHIJKLMNOPQ")

    def test_cf_without_ff_is_ignored(self) -> None:
        r = sniff_common.IsoTpReassembler()
        out = r.feed(0x7E8, b"\x21\x00\x01\x02\x03\x04\x05\x06")
        self.assertIsNone(out)

    def test_out_of_sequence_cf_drops_message(self) -> None:
        r = sniff_common.IsoTpReassembler()
        r.feed(0x7E8, b"\x10\x10" + b"\x01\x02\x03\x04\x05\x06")
        # Send CF#2 instead of CF#1 — should drop the in-flight message.
        r.feed(0x7E8, b"\x22" + b"\x07\x08\x09\x0A\x0B\x0C\x0D")
        # Now a proper CF#1 should NOT reconstruct, since the message
        # was already dropped.
        out = r.feed(0x7E8, b"\x21" + b"\x07\x08\x09\x0A\x0B\x0C\x0D")
        self.assertIsNone(out)

    def test_concurrent_per_can_id_state(self) -> None:
        # Two ECUs reassembling simultaneously must not cross-contaminate.
        r = sniff_common.IsoTpReassembler()
        r.feed(0x7E8, b"\x10\x0C" + b"AAAAAA")
        r.feed(0x7E9, b"\x10\x0C" + b"BBBBBB")
        out_8 = r.feed(0x7E8, b"\x21" + b"AAAAAA\x00")
        out_9 = r.feed(0x7E9, b"\x21" + b"BBBBBB\x00")
        self.assertEqual(out_8, b"AAAAAAAAAAAA")
        self.assertEqual(out_9, b"BBBBBBBBBBBB")

    def test_in_flight_diagnostic(self) -> None:
        r = sniff_common.IsoTpReassembler()
        r.feed(0x7E8, b"\x10\x20" + b"\x01\x02\x03\x04\x05\x06")
        flight = r.in_flight()
        self.assertEqual(flight, {0x7E8: 6})

    def test_escape_ff_reassembly_for_large_payloads(self) -> None:
        # Escape FF: byte[0]=0x10, byte[1]=0x00 → 32-bit length in bytes 2-5,
        # payload starts at byte 6 (only 2 bytes of payload in the FF itself).
        # Test reassembling a 10-byte payload via escape format.
        r = sniff_common.IsoTpReassembler()
        # FF: 10 00 [00 00 00 0A] [AA BB]  — 32-bit length 10, payload = AA BB
        out = r.feed(0x7E8, b"\x10\x00\x00\x00\x00\x0A\xAA\xBB")
        self.assertIsNone(out)
        # CF#1: 21 [7 bytes]  → adds CC DD EE FF 11 22 33; payload now 9 bytes
        out = r.feed(0x7E8, b"\x21\xCC\xDD\xEE\xFF\x11\x22\x33")
        self.assertIsNone(out)
        # CF#2: 22 [1 needed byte 44 + 6 pad]
        out = r.feed(0x7E8, b"\x22\x44\x00\x00\x00\x00\x00\x00")
        self.assertEqual(out, b"\xAA\xBB\xCC\xDD\xEE\xFF\x11\x22\x33\x44")

    def test_malformed_escape_ff_with_zero_length_dropped(self) -> None:
        # Escape FF with all-zero 32-bit length is malformed; should NOT
        # crash and must not return a zero-byte "message."
        r = sniff_common.IsoTpReassembler()
        out = r.feed(0x7E8, b"\x10\x00\x00\x00\x00\x00\xAA\xBB")
        self.assertIsNone(out)
        # No in-flight state left over either.
        self.assertEqual(r.in_flight(), {})

    def test_obdx_prereassembled_ff_returns_full_payload(self) -> None:
        # Some sniff adapters (OBDX Pro VX in DVI mode) reassemble multi-
        # frame ISO-TP messages in hardware before delivering them to the
        # host. The sniff log then carries the FF PCI immediately followed
        # by the ENTIRE payload in one log line, with no subsequent CFs.
        # The reassembler must detect this (frame_data length > the 8-byte
        # raw CAN ceiling) and return the full payload immediately rather
        # than buffering the first 6 bytes and waiting for CFs that never
        # arrive.
        #
        # Concrete case from an aftermarket reflash capture: a 0x34
        # RequestDownload arrives as `10 09 34 04 33 1E 00 00 02 00 00`
        # (11 bytes total — PCI says length 9, payload is 9 bytes after
        # the 2-byte PCI).
        # The old behaviour buffered the first 6 bytes (`34 04 33 1E 00 00`)
        # and waited for 3 more via CFs — they never arrived, the 0x34
        # was silently dropped, and every subsequent 0xB6 that referenced
        # the open transfer was discarded (`current is None`).
        r = sniff_common.IsoTpReassembler()
        ff_with_full_payload = bytes([
            0x10, 0x09,                                     # standard FF, length 9
            0x34, 0x04, 0x33, 0x1E, 0x00, 0x00, 0x02, 0x00, 0x00,  # 9-byte payload
        ])
        out = r.feed(0x7E0, ff_with_full_payload)
        self.assertEqual(out, bytes([0x34, 0x04, 0x33, 0x1E, 0x00, 0x00, 0x02, 0x00, 0x00]))
        # Reassembler should have no pending state — the FF was complete.
        self.assertEqual(r.in_flight(), {})

    def test_obdx_prereassembled_escape_ff_returns_full_payload(self) -> None:
        # Same OBDX-prereassembled case but for the escape-FF format used
        # for messages longer than 4095 bytes. Verifies the >8-byte
        # frame_data handling in the escape branch too.
        r = sniff_common.IsoTpReassembler()
        body = bytes(range(256))  # arbitrary 256-byte payload
        ff_escape = bytes([0x10, 0x00, 0x00, 0x00, 0x01, 0x00]) + body  # length 256
        out = r.feed(0x7E0, ff_escape)
        self.assertEqual(out, body)
        self.assertEqual(r.in_flight(), {})


if __name__ == "__main__":
    unittest.main()
