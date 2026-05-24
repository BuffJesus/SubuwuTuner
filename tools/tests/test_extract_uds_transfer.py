# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Tests for tools/extract_uds_transfer.py — round-trip a synthetic
sniff log through the extractor and verify the recovered manifest +
payload bytes."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

_THIS = Path(__file__).resolve()
_MOD_PATH = _THIS.parent.parent / "extract_uds_transfer.py"
_spec = importlib.util.spec_from_file_location("extract_uds_transfer", _MOD_PATH)
assert _spec is not None and _spec.loader is not None
extract_uds_transfer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = extract_uds_transfer
_spec.loader.exec_module(extract_uds_transfer)


def _line(ms: int, can_id: int, *bytes_: int) -> str:
    hex_part = " ".join(f"{b:02X}" for b in bytes_)
    return f"{ms} 0x{can_id:03X} {hex_part}\n"


def _synthetic_flash_log(d: Path) -> Path:
    """Hand-craft a minimal flash sequence:
      34 00 44 [4B addr=0x00040000] [4B size=0x14]   RequestDownload (20 bytes to 0x40000)
      36 01 [6 bytes via SF wait - actually MF]
      ...
      37                                              RequestTransferExit
    """
    log = d / "flash.log"
    lines: list[str] = ["# synthetic flash capture\n"]
    # RequestDownload, packaged as ISO-TP SF (length 11):
    #   34 00 44 00 04 00 00 00 00 00 14  → 11 bytes payload
    lines.append(_line(
        100, 0x7E0,
        0x0B, 0x34, 0x00, 0x44, 0x00, 0x04, 0x00, 0x00,  # SF len=11 + first 7 of payload
    ))
    # The above only carries 7 of 11 payload bytes — need to use FF for 11 bytes.
    # Reset and use FF + CF instead:
    lines = ["# synthetic flash capture\n"]
    # RequestDownload via FF + CF (11-byte payload):
    # FF: 10 0B + first 6 bytes of payload (34 00 44 00 04 00)
    lines.append(_line(100, 0x7E0, 0x10, 0x0B, 0x34, 0x00, 0x44, 0x00, 0x04, 0x00))
    # CF#1: 21 + remaining 5 bytes (00 00 00 00 14) + 2 pad bytes
    lines.append(_line(105, 0x7E0, 0x21, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00))
    # Positive response from ECU: 74 20 14 (RequestDownload positive: maxNumberOfBlockLength)
    lines.append(_line(110, 0x7E8, 0x03, 0x74, 0x20, 0x14, 0x00, 0x00, 0x00, 0x00))
    # TransferData block #1: 36 01 [up to 4 bytes — total 6 byte SF: 06 36 01 AA BB CC DD]
    lines.append(_line(120, 0x7E0, 0x06, 0x36, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0x00))
    # Positive ack: 76 01
    lines.append(_line(125, 0x7E8, 0x02, 0x76, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00))
    # TransferData block #2: 36 02 [4 more bytes EE FF 11 22]
    lines.append(_line(130, 0x7E0, 0x06, 0x36, 0x02, 0xEE, 0xFF, 0x11, 0x22, 0x00))
    lines.append(_line(135, 0x7E8, 0x02, 0x76, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00))
    # TransferData block #3: 36 03 [final 4 bytes 33 44 55 66 — wait need 8 total but already have 8]
    # Actually let's say 4 blocks × 4 bytes = 16 bytes total. Need one more block.
    lines.append(_line(140, 0x7E0, 0x06, 0x36, 0x03, 0x33, 0x44, 0x55, 0x66, 0x00))
    lines.append(_line(145, 0x7E8, 0x02, 0x76, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00))
    lines.append(_line(150, 0x7E0, 0x06, 0x36, 0x04, 0x77, 0x88, 0x99, 0xAA, 0x00))
    lines.append(_line(155, 0x7E8, 0x02, 0x76, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00))
    # RequestTransferExit
    lines.append(_line(160, 0x7E0, 0x01, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))
    lines.append(_line(165, 0x7E8, 0x01, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))
    log.write_text("".join(lines), encoding="utf-8")
    return log


class TestExtractTransfer(unittest.TestCase):
    def test_round_trip_recovers_payload(self) -> None:
        # NB: the payload .bin lives inside the temp dir, so the
        # read_bytes() assertion must happen INSIDE the `with` block —
        # the dir is gone by the time we exit.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            log = _synthetic_flash_log(d)
            output = d / "manifest.json"
            payload_dir = d / "payloads"
            rc = extract_uds_transfer.main([
                str(log), "--output", str(output),
                "--payload-dir", str(payload_dir),
            ])
            self.assertEqual(rc, 0)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema"], "subuwutuner.flash.v1")
            self.assertEqual(len(manifest["transfers"]), 1)
            t = manifest["transfers"][0]
            self.assertEqual(t["address"], "0x00040000")
            self.assertEqual(t["declared_size"], 0x14)
            self.assertEqual(t["received_size"], 16)  # 4 blocks * 4 bytes each
            self.assertEqual(t["format_byte"], "0x00")
            self.assertEqual(t["addr_len_format"], "0x44")
            self.assertEqual(t["block_count"], 4)
            recovered = Path(t["payload_path"]).read_bytes()
            self.assertEqual(
                recovered,
                b"\xAA\xBB\xCC\xDD\xEE\xFF\x11\x22\x33\x44\x55\x66\x77\x88\x99\xAA",
            )

    def test_decode_request_download_alf(self) -> None:
        # 34 00 44 [4B addr] [4B size]
        app = bytes([0x34, 0x00, 0x44, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14])
        decoded = extract_uds_transfer._decode_request_download(app)
        self.assertEqual(decoded, (0x00, 0x44, 0x00040000, 0x14))

    def test_decode_request_download_rejects_short(self) -> None:
        self.assertIsNone(extract_uds_transfer._decode_request_download(b"\x34\x00"))


if __name__ == "__main__":
    unittest.main()
