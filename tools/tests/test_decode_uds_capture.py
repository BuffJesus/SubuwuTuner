# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for tools/decode_uds_capture.py — service / NRC / anomaly decoding."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

_THIS = Path(__file__).resolve()
_MOD_PATH = _THIS.parent.parent / "decode_uds_capture.py"
_spec = importlib.util.spec_from_file_location("decode_uds_capture", _MOD_PATH)
assert _spec is not None and _spec.loader is not None
decode_uds_capture = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = decode_uds_capture
_spec.loader.exec_module(decode_uds_capture)


def _line(ms: int, can_id: int, *bytes_: int) -> str:
    hex_part = " ".join(f"{b:02X}" for b in bytes_)
    return f"{ms} 0x{can_id:03X} {hex_part}\n"


class TestDecodeFrame(unittest.TestCase):
    def test_decodes_dsc_request_with_sub_function(self) -> None:
        decoded, anom, _ = decode_uds_capture.decode_frame(b"\x10\x03")
        self.assertIn("DiagnosticSessionControl", decoded)
        self.assertIn("extendedDiagnosticSession", decoded)
        self.assertFalse(anom)

    def test_decodes_positive_response(self) -> None:
        # +ve to SecurityAccess requestSeed: 67 01 DE AD BE EF
        decoded, anom, _ = decode_uds_capture.decode_frame(b"\x67\x01\xDE\xAD\xBE\xEF")
        self.assertIn("+ve", decoded)
        self.assertIn("SecurityAccess", decoded)
        self.assertIn("requestSeed", decoded)
        self.assertIn("DEADBEEF", decoded)
        self.assertFalse(anom)

    def test_decodes_known_nrc(self) -> None:
        decoded, anom, _ = decode_uds_capture.decode_frame(b"\x7F\x27\x36")
        self.assertIn("NRC SecurityAccess", decoded)
        self.assertIn("exceededNumberOfAttempts", decoded)
        # Known NRC = not an anomaly.
        self.assertFalse(anom)

    def test_unknown_nrc_flagged_as_anomaly(self) -> None:
        # NRC 0xAA isn't in the catalog.
        decoded, anom, reason = decode_uds_capture.decode_frame(b"\x7F\x27\xAA")
        self.assertTrue(anom)
        self.assertIn("0xAA", reason)

    def test_unknown_sid_flagged_as_anomaly(self) -> None:
        decoded, anom, reason = decode_uds_capture.decode_frame(b"\xBE\x00\x01")
        self.assertTrue(anom)
        self.assertIn("not in catalog", reason)


class TestBuildTimeline(unittest.TestCase):
    def test_text_renderer_includes_arrows_and_decoded_services(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "cap.log"
            log.write_text(
                _line(100, 0x7E0, 0x02, 0x10, 0x03, 0, 0, 0, 0, 0)
                + _line(110, 0x7E8, 0x02, 0x50, 0x03, 0, 0, 0, 0, 0)
                + _line(120, 0x7E0, 0x02, 0x27, 0x01, 0, 0, 0, 0, 0)
                + _line(130, 0x7E8, 0x06, 0x67, 0x01, 0xDE, 0xAD, 0xBE, 0xEF, 0)
                + _line(140, 0x7E0, 0x06, 0x27, 0x02, 0xCA, 0xFE, 0xBA, 0xBE, 0)
                + _line(150, 0x7E8, 0x03, 0x7F, 0x27, 0x35, 0, 0, 0, 0),
                encoding="utf-8",
            )
            out_path = Path(d) / "decoded.txt"
            rc = decode_uds_capture.main([str(log), "--output", str(out_path)])
            self.assertEqual(rc, 0)
            text = out_path.read_text(encoding="utf-8")
        self.assertIn("->", text)
        self.assertIn("<-", text)
        self.assertIn("DiagnosticSessionControl", text)
        self.assertIn("extendedDiagnosticSession", text)
        self.assertIn("SecurityAccess", text)
        self.assertIn("requestSeed", text)
        self.assertIn("invalidKey", text)

    def test_json_output_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "cap.log"
            log.write_text(_line(100, 0x7E0, 0x02, 0x10, 0x03, 0, 0, 0, 0, 0), encoding="utf-8")
            out_path = Path(d) / "decoded.json"
            rc = decode_uds_capture.main(
                [str(log), "--output", str(out_path), "--json"]
            )
            self.assertEqual(rc, 0)
            data = json.loads(out_path.read_text(encoding="utf-8"))
        self.assertEqual(data["schema"], "subuwutuner.uds_timeline.v1")
        self.assertEqual(len(data["entries"]), 1)
        self.assertEqual(data["entries"][0]["direction"], "req")
        self.assertIn("extendedDiagnosticSession", data["entries"][0]["decoded"])

    def test_anomaly_section_appears_in_text_output(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "cap.log"
            # Unknown SID 0xBE — should fire the anomaly path.
            log.write_text(_line(100, 0x7E0, 0x02, 0xBE, 0x01, 0, 0, 0, 0, 0), encoding="utf-8")
            out_path = Path(d) / "decoded.txt"
            self.assertEqual(
                decode_uds_capture.main([str(log), "--output", str(out_path)]),
                0,
            )
            text = out_path.read_text(encoding="utf-8")
        self.assertIn("anomaly", text)
        self.assertIn("0xBE", text)


if __name__ == "__main__":
    unittest.main()
