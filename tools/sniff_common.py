# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Shared sniff-log primitives for tools/extract_*.py and tools/decode_*.py.

A SubuwuTuner sniff log (v1) is one frame per line:

    <elapsed_ms> 0x<can_id> <hex bytes...>

Lines beginning with `#` are comments. Multiple sniff-log producers (the
`subuwutuner-cli sniff` subcommand, hand-edited captures, fixtures from
unit tests) emit the same format.

This module provides:
- `Frame` dataclass + `parse_log(path)` reader
- `IsoTpReassembler` — full SF / FF / CF reassembly keyed by CAN ID
- `strip_iso_tp_sf` — convenience for single-frame-only flows (e.g. SA)

The standalone `extract_subaru_sa.py` script predates this module and
duplicates the small subset it needs. Don't bother retro-refactoring
it; the duplication is small and that script's flow is SF-only so the
full reassembler buys nothing.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class Frame:
    ms: int
    can_id: int
    data: bytes


def parse_log(path: Path) -> list[Frame]:
    """Read a SubuwuTuner sniff log into Frame objects. Malformed lines are
    skipped with a stderr warning rather than aborting — captures can be
    noisy and one bad line shouldn't lose the whole capture."""
    out: list[Frame] = []
    with path.open("r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(maxsplit=2)
            if len(parts) < 3:
                print(f"warn: {path}:{lineno}: skipping malformed line", file=sys.stderr)
                continue
            try:
                ms = int(parts[0])
                can_id = (
                    int(parts[1], 16) if parts[1].lower().startswith("0x") else int(parts[1])
                )
                data = bytes(int(h, 16) for h in parts[2].split())
            except ValueError as e:
                print(f"warn: {path}:{lineno}: parse failed ({e}) — skipping", file=sys.stderr)
                continue
            out.append(Frame(ms=ms, can_id=can_id, data=data))
    return out


def strip_iso_tp_sf(data: bytes) -> Optional[bytes]:
    """Strip the ISO-TP PCI byte from a single-frame payload.

    Returns the application-layer bytes or None if the frame isn't a
    valid single frame (FF/CF/FC return None — the caller is expected
    to use `IsoTpReassembler` if they need multi-frame support).
    """
    if len(data) < 1:
        return None
    pci_hi = data[0] & 0xF0
    if pci_hi != 0x00:
        return None
    length = data[0] & 0x0F
    if 1 + length > len(data):
        return None
    return data[1 : 1 + length]


# ──────────────────────────────────────────────────────────────────────
# ISO-TP reassembly
# ──────────────────────────────────────────────────────────────────────

_PCI_SF = 0x00
_PCI_FF = 0x10
_PCI_CF = 0x20
_PCI_FC = 0x30


@dataclass
class _PendingMessage:
    expected_length: int
    payload: bytearray = field(default_factory=bytearray)
    next_seq: int = 1  # CF sequence wraps 0..15, starts at 1 (FF was implicit 0)


class IsoTpReassembler:
    """Stateful per-CAN-ID ISO-TP reassembler.

    `feed(can_id, frame_data)` returns:
      * `bytes` — reassembled payload (when this frame completes a message)
      * `None`  — frame was consumed but the message isn't done
      * `None`  — frame was FC or malformed (silently dropped — sniffer mode
                  doesn't care about flow control, and corruption on the
                  wire is expected)

    Single-frame messages return their payload immediately on the same
    `feed()` call. Multi-frame messages return on the final CF.

    State is kept per `can_id` so concurrent reassembly across multiple
    ECUs (e.g., a Y-cable capture seeing 0x7E8, 0x7E9, …) is safe.
    """

    def __init__(self) -> None:
        self._pending: dict[int, _PendingMessage] = {}

    def feed(self, can_id: int, frame_data: bytes) -> Optional[bytes]:
        if len(frame_data) < 1:
            return None
        pci = frame_data[0] & 0xF0

        if pci == _PCI_SF:
            length = frame_data[0] & 0x0F
            if length == 0 or 1 + length > len(frame_data):
                return None
            # SF resets any in-flight reassembly for this ID — the OEM
            # tool wouldn't normally interleave but corruption can do it.
            self._pending.pop(can_id, None)
            return bytes(frame_data[1 : 1 + length])

        if pci == _PCI_FF:
            if len(frame_data) < 2:
                return None
            # ISO 15765-2 FF has two formats:
            #   Standard: byte[0]=0x10..0x1F + byte[1] gives a 12-bit length
            #             (0x001..0xFFF). Payload starts at byte[2], FF carries
            #             up to 6 bytes.
            #   Escape:   byte[0]=0x10, byte[1]=0x00 → real 32-bit length is in
            #             bytes[2..5]. Payload starts at byte[6], FF carries only
            #             up to 2 bytes. Used for messages > 4095 bytes (large
            #             flash blocks in some tuner-tool transfers).
            standard_length = ((frame_data[0] & 0x0F) << 8) | frame_data[1]
            if standard_length == 0:
                # Escape FF.
                if len(frame_data) < 6:
                    return None
                total_length = int.from_bytes(frame_data[2:6], "big")
                first_chunk = bytes(frame_data[6:8])  # FF carries up to 2 bytes here
            else:
                total_length = standard_length
                first_chunk = bytes(frame_data[2:8])  # up to 6 bytes
            if total_length == 0:
                # Malformed even after escape — drop silently.
                return None
            self._pending[can_id] = _PendingMessage(
                expected_length=total_length,
                payload=bytearray(first_chunk[: min(total_length, len(first_chunk))]),
                next_seq=1,
            )
            # FF alone doesn't complete (any real message that fits in 6
            # bytes uses SF, not FF). But guard against weird ECUs anyway.
            pend = self._pending[can_id]
            if len(pend.payload) >= pend.expected_length:
                done = bytes(pend.payload[: pend.expected_length])
                del self._pending[can_id]
                return done
            return None

        if pci == _PCI_CF:
            pend = self._pending.get(can_id)
            if pend is None:
                # CF without preceding FF — dropped frame or corrupt
                # capture. Ignore silently.
                return None
            seq = frame_data[0] & 0x0F
            if seq != (pend.next_seq & 0x0F):
                # Out-of-sequence CF — gap in the capture. Drop the
                # in-flight message rather than emit a corrupted blob.
                del self._pending[can_id]
                return None
            pend.next_seq = (pend.next_seq + 1) & 0x0F
            remaining = pend.expected_length - len(pend.payload)
            chunk = bytes(frame_data[1 : 1 + min(7, remaining)])
            pend.payload.extend(chunk)
            if len(pend.payload) >= pend.expected_length:
                done = bytes(pend.payload[: pend.expected_length])
                del self._pending[can_id]
                return done
            return None

        # FC or unknown PCI — silently ignored in sniffer mode.
        return None

    def in_flight(self) -> dict[int, int]:
        """Diagnostic: returns {can_id: bytes_collected_so_far}. Useful
        for spotting truncated captures (an ID that ended mid-message)."""
        return {cid: len(p.payload) for cid, p in self._pending.items()}
