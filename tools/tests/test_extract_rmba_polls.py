# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for tools/extract_rmba_polls.py — synthetic RMBA/RDBI poll
stream → aggregated per-address stats."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

_THIS = Path(__file__).resolve()
_MOD_PATH = _THIS.parent.parent / "extract_rmba_polls.py"
_spec = importlib.util.spec_from_file_location("extract_rmba_polls", _MOD_PATH)
assert _spec is not None and _spec.loader is not None
extract_rmba_polls = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = extract_rmba_polls
_spec.loader.exec_module(extract_rmba_polls)


def _line(ms: int, can_id: int, *bytes_: int) -> str:
    hex_part = " ".join(f"{b:02X}" for b in bytes_)
    return f"{ms} 0x{can_id:03X} {hex_part}\n"


def _rmba_request(ms: int, addr: int, size: int = 4) -> str:
    """SF: `23 alf [4B addr] [1B size]` — 7 data bytes (SF max). alf=0x14
    means size_bytes=1 (high nibble) + addr_bytes=4 (low nibble)."""
    return _line(
        ms, 0x7E0,
        0x07, 0x23, 0x14,
        (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
        (addr >> 8) & 0xFF, addr & 0xFF,
        size,
    )


def _rmba_response(ms: int, value: int) -> str:
    """SF: `63 [4B value]` — 5 bytes."""
    return _line(
        ms, 0x7E8,
        0x05, 0x63,
        (value >> 24) & 0xFF, (value >> 16) & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF, 0x00,
    )


def _rdbi_request(ms: int, did: int) -> str:
    """SF: `22 [2B DID]` — 3 bytes."""
    return _line(ms, 0x7E0, 0x03, 0x22, (did >> 8) & 0xFF, did & 0xFF, 0x00, 0x00, 0x00, 0x00)


def _rdbi_response(ms: int, did: int, value: int) -> str:
    """SF: `62 [2B DID] [4B value]` — 7 bytes."""
    return _line(
        ms, 0x7E8,
        0x07, 0x62, (did >> 8) & 0xFF, did & 0xFF,
        (value >> 24) & 0xFF, (value >> 16) & 0xFF,
        (value >> 8) & 0xFF, value & 0xFF,
    )


class TestExtractPolls(unittest.TestCase):
    def test_rmba_aggregation(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "polls.log"
            lines: list[str] = []
            # Poll addr 0xFFFF1234 every 100ms, 5 times, with varying values
            for i, t in enumerate([100, 200, 300, 400, 500]):
                lines.append(_rmba_request(t, 0xFFFF1234))
                lines.append(_rmba_response(t + 5, 0x1000 + i * 0x10))
            # Poll a second address less frequently
            lines.append(_rmba_request(150, 0xFFFF5678))
            lines.append(_rmba_response(155, 0xDEADBEEF))
            log.write_text("".join(lines), encoding="utf-8")

            output = Path(d) / "polls.json"
            rc = extract_rmba_polls.main([str(log), "--output", str(output)])
            self.assertEqual(rc, 0)
            polls = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(polls["schema"], "subuwutuner.poll.v1")
        by_addr = {p["address"]: p for p in polls["polls"]}
        self.assertIn("0xFFFF1234", by_addr)
        self.assertIn("0xFFFF5678", by_addr)
        p1 = by_addr["0xFFFF1234"]
        self.assertEqual(p1["poll_count"], 5)
        self.assertEqual(p1["response_count"], 5)
        self.assertEqual(p1["poll_period_ms_p50"], 100)
        self.assertEqual(len(p1["value_samples"]), 5)
        # Distinct values captured: 1000, 1010, 1020, 1030, 1040
        self.assertIn("00001000", p1["value_samples"])
        self.assertIn("00001040", p1["value_samples"])

    def test_rdbi_aggregation_and_pairing_by_did(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "polls.log"
            lines = [
                _rdbi_request(100, 0xF190),
                _rdbi_response(110, 0xF190, 0xAABBCCDD),
                _rdbi_request(200, 0xF188),
                _rdbi_response(210, 0xF188, 0x11223344),
                _rdbi_request(300, 0xF190),
                _rdbi_response(310, 0xF190, 0xAABBCCDD),
            ]
            log.write_text("".join(lines), encoding="utf-8")
            output = Path(d) / "polls.json"
            self.assertEqual(
                extract_rmba_polls.main([str(log), "--output", str(output)]),
                0,
            )
            polls = json.loads(output.read_text(encoding="utf-8"))
        by_addr = {(p["kind"], p["address"]): p for p in polls["polls"]}
        self.assertEqual(by_addr[("rdbi", "0xF190")]["poll_count"], 2)
        self.assertEqual(by_addr[("rdbi", "0xF188")]["poll_count"], 1)

    def test_nrc_drops_in_flight_of_matching_kind(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "polls.log"
            lines = [
                _rmba_request(100, 0xFFFF1234),
                # NRC 0x33 for SID 0x23
                _line(110, 0x7E8, 0x03, 0x7F, 0x23, 0x33, 0x00, 0x00, 0x00, 0x00),
                _rmba_request(200, 0xFFFF1234),
                _rmba_response(210, 0x42),
            ]
            log.write_text("".join(lines), encoding="utf-8")
            output = Path(d) / "polls.json"
            self.assertEqual(
                extract_rmba_polls.main([str(log), "--output", str(output)]),
                0,
            )
            polls = json.loads(output.read_text(encoding="utf-8"))
        by_addr = {p["address"]: p for p in polls["polls"]}
        agg = by_addr["0xFFFF1234"]
        self.assertEqual(agg["poll_count"], 2)
        self.assertEqual(agg["response_count"], 1)  # only the 2nd got a positive response

    def test_decode_rmba_request(self) -> None:
        # 23 13 [3B addr] [1B size]
        app = bytes([0x23, 0x13, 0xAA, 0xBB, 0xCC, 0x08])
        self.assertEqual(
            extract_rmba_polls._decode_rmba_request(app), (0xAABBCC, 0x08)
        )

    def test_decode_rdbi_request(self) -> None:
        self.assertEqual(extract_rmba_polls._decode_rdbi_request(b"\x22\xF1\x90"), 0xF190)
        self.assertIsNone(extract_rmba_polls._decode_rdbi_request(b"\x22\xF1"))

    def test_nrc_0x78_response_pending_does_not_drop_in_flight(self) -> None:
        # NRC 0x78 (requestCorrectlyReceivedResponsePending) is "still
        # working, wait" — not a final rejection. Real response arrives
        # on a subsequent frame and must still be paired with the
        # in-flight request.
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "polls.log"
            lines = [
                _rmba_request(100, 0xFFFF1234),
                # NRC 0x78 from ECU: "wait, still working"
                _line(110, 0x7E8, 0x03, 0x7F, 0x23, 0x78, 0x00, 0x00, 0x00, 0x00),
                # Then the real positive response with the value
                _rmba_response(150, 0xCAFEBABE),
            ]
            log.write_text("".join(lines), encoding="utf-8")
            output = Path(d) / "polls.json"
            self.assertEqual(
                extract_rmba_polls.main([str(log), "--output", str(output)]),
                0,
            )
            polls = json.loads(output.read_text(encoding="utf-8"))
        by_addr = {p["address"]: p for p in polls["polls"]}
        agg = by_addr["0xFFFF1234"]
        # The 0x78 must NOT have dropped the in-flight; the value should
        # still be paired with the original request.
        self.assertEqual(agg["poll_count"], 1)
        self.assertEqual(agg["response_count"], 1)
        self.assertIn("CAFEBABE", agg["value_samples"])


if __name__ == "__main__":
    unittest.main()
