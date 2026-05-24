#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Extract UDS RequestDownload / TransferData / RequestTransferExit
sequences from a SubuwuTuner sniff log.

Implements Workflow 1 of `docs/24-sniff-workflows.md`: recovering the
calibration bytes a tuner tool (COBB AP, EcuTek, ECUFlash via Tactrix)
writes to the ECU during a flash, without ever issuing an RMBA
ourselves. Particularly valuable when the ROM-read path is gated by
SecurityAccess we can't compute yet.

Workflow:
  1. Y-cable the OBDX VX and the active tuning tool into the OBD-II
     port. Start `subuwutuner-cli sniff` in listen-only mode.
  2. Perform a flash through the active tool.
  3. Ctrl+C the sniffer; run:
       python tools/extract_uds_transfer.py capture.log \\
           --output transfers.json --payload-dir payloads/

Output:
  - transfers.json — per-transfer metadata (address, declared size,
    received size, format byte, block count, payload SHA-256, filename)
  - payloads/transfer-N-0xADDRESS.bin — reassembled payload per `34/37`
    cycle, byte-for-byte as it appeared on the wire

CAVEATS — read these before treating the payload as "the tune":
  - dataFormatIdentifier (the byte after `34`) is the OEM-spec
    declaration of compression/encryption applied to the transferred
    bytes. Value 0x00 means the ECU writes the wire bytes verbatim.
    Non-zero values mean the bytes on the wire are NOT the bytes
    that hit flash.
  - Tuner tools often wrap their calibration in an outer envelope
    (COBB-format, EcuTek's container) even when 0x00 is declared.
    Compare against a known pre-flash baseline before trusting.
  - blockSequenceCounter wraps 0xFF → 0x00 per ISO 14229 §14.4.2.

ISO-TP multi-frame reassembly is built in (TransferData blocks rarely
fit in a CAN single frame).
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# Sibling module — load by absolute path so this script runs the same
# whether invoked from the repo root or from inside tools/.
_THIS = Path(__file__).resolve()
_spec = importlib.util.spec_from_file_location("sniff_common", _THIS.parent / "sniff_common.py")
assert _spec is not None and _spec.loader is not None
sniff_common = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = sniff_common
_spec.loader.exec_module(sniff_common)


# UDS service IDs we care about for transfer extraction.
SID_REQUEST_DOWNLOAD = 0x34
SID_REQUEST_UPLOAD = 0x35
SID_TRANSFER_DATA = 0x36
SID_REQUEST_TRANSFER_EXIT = 0x37
POSITIVE_RESPONSE_OFFSET = 0x40


@dataclass
class Transfer:
    direction: str         # "download" (host → ECU) or "upload" (ECU → host)
    address: int           # decoded from RequestDownload/Upload addressAndLengthFormat
    declared_size: int     # what `34` said the transfer would be
    received_size: int = 0  # what we actually reassembled from `36` blocks
    format_byte: int = 0   # dataFormatIdentifier
    addr_len_format: int = 0  # addressAndLengthFormatIdentifier
    block_count: int = 0
    block_seq_wraps: int = 0
    payload: bytearray = field(default_factory=bytearray)
    request_ms: int = -1
    exit_ms: int = -1


def _decode_request_download(app: bytes) -> Optional[tuple[int, int, int, int]]:
    """Parse a `34 fmt alf [addr] [size]` payload. Returns (format_byte,
    addr_len_format, address, size) or None if the frame is malformed.

    The addressAndLengthFormatIdentifier nibble layout:
      - high nibble = size-byte count
      - low nibble  = address-byte count
    Each up to 4 bytes per ISO 14229.
    """
    if len(app) < 3:
        return None
    sid = app[0]
    if sid not in (SID_REQUEST_DOWNLOAD, SID_REQUEST_UPLOAD):
        return None
    format_byte = app[1]
    alf = app[2]
    addr_bytes = alf & 0x0F
    size_bytes = (alf >> 4) & 0x0F
    if addr_bytes == 0 or addr_bytes > 4 or size_bytes == 0 or size_bytes > 4:
        return None
    expected = 3 + addr_bytes + size_bytes
    if len(app) < expected:
        return None
    address = int.from_bytes(app[3 : 3 + addr_bytes], "big")
    size = int.from_bytes(app[3 + addr_bytes : 3 + addr_bytes + size_bytes], "big")
    return format_byte, alf, address, size


def extract_transfers(
    frames: list["sniff_common.Frame"],
    request_id: int,
    response_id: int,
) -> tuple[list[Transfer], list[str]]:
    """Walk a sniff log frame stream and pair RequestDownload/Upload with
    their subsequent TransferData payload. Returns (transfers, warnings)."""
    warnings: list[str] = []
    transfers: list[Transfer] = []

    req_reassembler = sniff_common.IsoTpReassembler()
    resp_reassembler = sniff_common.IsoTpReassembler()

    current: Optional[Transfer] = None
    last_seq: Optional[int] = None  # tracks blockSequenceCounter wrap

    for f in frames:
        if f.can_id == request_id:
            app = req_reassembler.feed(f.can_id, f.data)
            if app is None or len(app) < 1:
                continue
            sid = app[0]
            if sid in (SID_REQUEST_DOWNLOAD, SID_REQUEST_UPLOAD):
                decoded = _decode_request_download(app)
                if decoded is None:
                    warnings.append(
                        f"malformed {hex(sid)} at t={f.ms}ms — skipped"
                    )
                    continue
                fmt, alf, addr, size = decoded
                if current is not None:
                    warnings.append(
                        f"new {hex(sid)} at t={f.ms}ms while transfer to "
                        f"0x{current.address:08X} still open ({current.received_size}/"
                        f"{current.declared_size} bytes received) — closing previous"
                    )
                    transfers.append(current)
                current = Transfer(
                    direction="download" if sid == SID_REQUEST_DOWNLOAD else "upload",
                    address=addr,
                    declared_size=size,
                    format_byte=fmt,
                    addr_len_format=alf,
                    request_ms=f.ms,
                )
                last_seq = None
            elif sid == SID_TRANSFER_DATA and current is not None:
                if len(app) < 2:
                    continue
                seq = app[1]
                data = app[2:]
                if last_seq is not None:
                    # ISO 14229 §14.4.2: counter wraps 0xFF → 0x00.
                    expected_seq = (last_seq + 1) & 0xFF
                    if seq != expected_seq:
                        warnings.append(
                            f"block-seq gap at t={f.ms}ms: expected 0x{expected_seq:02X}, "
                            f"got 0x{seq:02X} (counter wrap counted regardless)"
                        )
                    if last_seq == 0xFF and seq == 0x00:
                        current.block_seq_wraps += 1
                last_seq = seq
                current.payload.extend(data)
                current.received_size += len(data)
                current.block_count += 1
            elif sid == SID_REQUEST_TRANSFER_EXIT and current is not None:
                current.exit_ms = f.ms
                transfers.append(current)
                current = None
                last_seq = None
        elif f.can_id == response_id:
            # We don't strictly need responses for payload extraction, but
            # walking them keeps the reassembler state consistent for any
            # future enhancement (e.g., validating positive-ack timing).
            resp_reassembler.feed(f.can_id, f.data)

    if current is not None:
        warnings.append(
            f"capture ended mid-transfer (addr 0x{current.address:08X}, "
            f"{current.received_size}/{current.declared_size} bytes received) — "
            f"emitting partial"
        )
        transfers.append(current)

    return transfers, warnings


def write_payload(transfer: Transfer, payload_dir: Path, idx: int) -> tuple[Path, str]:
    """Write a transfer's reassembled bytes to disk. Returns (path, sha256-hex)."""
    payload_dir.mkdir(parents=True, exist_ok=True)
    filename = f"transfer-{idx:03d}-0x{transfer.address:08X}.bin"
    path = payload_dir / filename
    data = bytes(transfer.payload)
    path.write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()
    return path, digest


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )
    p.add_argument("log", type=Path, help="sniff log from subuwutuner-cli sniff")
    p.add_argument(
        "--output", "-o", type=Path, required=True,
        help="output JSON path (transfer manifest)",
    )
    p.add_argument(
        "--payload-dir", type=Path, default=None,
        help="directory to write per-transfer .bin payloads "
             "(default: <output stem>.payloads/)",
    )
    p.add_argument(
        "--request-id", type=lambda s: int(s, 0), default=0x7E0,
        help="CAN ID for host→ECU requests (default 0x7E0)",
    )
    p.add_argument(
        "--response-id", type=lambda s: int(s, 0), default=0x7E8,
        help="CAN ID for ECU→host responses (default 0x7E8)",
    )
    args = p.parse_args(argv)

    if not args.log.exists():
        print(f"error: {args.log} does not exist", file=sys.stderr)
        return 1

    payload_dir = args.payload_dir or (args.output.parent / (args.output.stem + ".payloads"))

    frames = sniff_common.parse_log(args.log)
    print(f"parsed {len(frames)} frames from {args.log}", file=sys.stderr)

    transfers, warnings = extract_transfers(frames, args.request_id, args.response_id)
    for w in warnings:
        print(f"warn: {w}", file=sys.stderr)
    print(f"extracted {len(transfers)} transfer(s)", file=sys.stderr)

    manifest = {
        "schema": "subuwutuner.flash.v1",
        "request_id": f"0x{args.request_id:03X}",
        "response_id": f"0x{args.response_id:03X}",
        "transfers": [],
    }
    for idx, t in enumerate(transfers):
        path, digest = write_payload(t, payload_dir, idx)
        manifest["transfers"].append({
            "index": idx,
            "direction": t.direction,
            "address": f"0x{t.address:08X}",
            "declared_size": t.declared_size,
            "received_size": t.received_size,
            "format_byte": f"0x{t.format_byte:02X}",
            "addr_len_format": f"0x{t.addr_len_format:02X}",
            "block_count": t.block_count,
            "block_seq_wraps": t.block_seq_wraps,
            "payload_sha256": digest,
            "payload_path": str(path),
            "request_ms": t.request_ms,
            "exit_ms": t.exit_ms,
        })

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
