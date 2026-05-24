#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Extract per-address RAM-poll statistics from a SubuwuTuner sniff log.

Implements Workflow 2 of `docs/24-sniff-workflows.md`: identifying the
live RAM addresses a tuner-tool reads when displaying its datalogger.
Cross-referencing those addresses against the definition pack reveals
which calibration tables feed which live quantity — useful both for
locating undocumented tables and for confirming the def pack's claim
about a known one.

Workflow:
  1. Y-cable + start sniff with COBB AP / EcuTek / ECUFlash showing
     the datalogger.
  2. Drive (or idle) through interesting conditions (cold start, WOT
     pull, deceleration, gear changes) so the polled values exercise
     their range.
  3. Ctrl+C the sniffer; run:
       python tools/extract_rmba_polls.py capture.log --output polls.json

Output (per address): poll count, period statistics, value range,
sample window. Standardised OBD-II PIDs (UDS `22 [DID]`) are tracked
separately from Subaru-specific RAM reads (UDS `23 [addr]`), since
they answer different questions about the tool.

Pairing recipe (per `docs/24` §Workflow 2 §"Pairing addresses to
features"): capture a baseline (idle/neutral) and a triggered
capture (LC engaged, rev limit hit, etc.), run this script over
each, and diff the value distributions per address to identify
the feature's live-state shadow.

The reassembler handles multi-frame responses for RDBI replies that
exceed CAN single-frame (e.g. VIN at 17 bytes via 0xF190).
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

_THIS = Path(__file__).resolve()
_spec = importlib.util.spec_from_file_location("sniff_common", _THIS.parent / "sniff_common.py")
assert _spec is not None and _spec.loader is not None
sniff_common = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = sniff_common
_spec.loader.exec_module(sniff_common)


SID_RDBI = 0x22                # ReadDataByIdentifier
SID_RMBA = 0x23                # ReadMemoryByAddress
POSITIVE_RESPONSE_OFFSET = 0x40


@dataclass
class _Poll:
    address: int                    # for RMBA: the memory address; for RDBI: the DID
    kind: str                       # "rmba" or "rdbi"
    size_hint: int                  # bytes requested (RMBA only; -1 for RDBI)
    request_ms: int
    response_ms: int = -1
    value: Optional[bytes] = None   # set when paired with positive response


@dataclass
class _AggregatedAddress:
    address: int
    kind: str
    poll_count: int = 0
    response_count: int = 0
    request_ms: list[int] = field(default_factory=list)
    response_values: list[bytes] = field(default_factory=list)


def _decode_rmba_request(app: bytes) -> Optional[tuple[int, int]]:
    """Parse `23 alf [addr] [size]`. Returns (address, size_bytes) or None.

    Same addressAndLengthFormatIdentifier nibble layout as `34/35`.
    """
    if len(app) < 2 or app[0] != SID_RMBA:
        return None
    alf = app[1]
    addr_bytes = alf & 0x0F
    size_bytes = (alf >> 4) & 0x0F
    if addr_bytes == 0 or addr_bytes > 4 or size_bytes == 0 or size_bytes > 4:
        return None
    expected = 2 + addr_bytes + size_bytes
    if len(app) < expected:
        return None
    address = int.from_bytes(app[2 : 2 + addr_bytes], "big")
    size = int.from_bytes(app[2 + addr_bytes : 2 + addr_bytes + size_bytes], "big")
    return address, size


def _decode_rdbi_request(app: bytes) -> Optional[int]:
    """Parse `22 [DID hi] [DID lo]`. Returns the 16-bit DID or None.

    RDBI can carry multiple DIDs but Subaru tuner-tools poll one at a
    time, so we only decode the single-DID form here. Multi-DID polls
    would need their own pairing logic.
    """
    if len(app) < 3 or app[0] != SID_RDBI:
        return None
    return (app[1] << 8) | app[2]


def extract_polls(
    frames: list["sniff_common.Frame"],
    request_id: int,
    response_id: int,
    max_value_samples: int = 32,
) -> tuple[list[_AggregatedAddress], list[str]]:
    """Walk the log and aggregate per-(kind, address) statistics."""
    warnings: list[str] = []

    req_reassembler = sniff_common.IsoTpReassembler()
    resp_reassembler = sniff_common.IsoTpReassembler()

    in_flight: dict[tuple[str, int], _Poll] = {}
    completed: list[_Poll] = []

    for f in frames:
        if f.can_id == request_id:
            app = req_reassembler.feed(f.can_id, f.data)
            if app is None or len(app) < 1:
                continue
            sid = app[0]
            if sid == SID_RMBA:
                decoded = _decode_rmba_request(app)
                if decoded is None:
                    warnings.append(f"malformed RMBA at t={f.ms}ms — skipped")
                    continue
                addr, size = decoded
                in_flight[("rmba", addr)] = _Poll(
                    address=addr, kind="rmba", size_hint=size, request_ms=f.ms
                )
            elif sid == SID_RDBI:
                did = _decode_rdbi_request(app)
                if did is None:
                    warnings.append(f"malformed RDBI at t={f.ms}ms — skipped")
                    continue
                in_flight[("rdbi", did)] = _Poll(
                    address=did, kind="rdbi", size_hint=-1, request_ms=f.ms
                )
        elif f.can_id == response_id:
            app = resp_reassembler.feed(f.can_id, f.data)
            if app is None or len(app) < 1:
                continue
            sid = app[0]
            # NRC: drop the in-flight matching the rejected service.
            # 0x78 (requestCorrectlyReceivedResponsePending) is NOT a final
            # rejection — it means "still working, wait for the real response."
            # Skip it; the real response will arrive on a subsequent frame.
            if sid == 0x7F and len(app) >= 3:
                rejected_sid = app[1]
                nrc = app[2]
                if nrc == 0x78:
                    continue
                kind = "rmba" if rejected_sid == SID_RMBA else "rdbi" if rejected_sid == SID_RDBI else None
                if kind is not None:
                    # Drop ALL in-flight of that kind — we can't tell
                    # from the NRC which specific request failed.
                    for key in [k for k in in_flight if k[0] == kind]:
                        completed.append(in_flight.pop(key))
                continue
            if sid == SID_RMBA + POSITIVE_RESPONSE_OFFSET:
                # Pair with the oldest in-flight RMBA (we don't know
                # the address from the response alone — RMBA responses
                # have no address echo).
                rmba_keys = [k for k in in_flight if k[0] == "rmba"]
                if not rmba_keys:
                    continue
                key = rmba_keys[0]
                poll = in_flight.pop(key)
                poll.response_ms = f.ms
                poll.value = bytes(app[1:])
                completed.append(poll)
            elif sid == SID_RDBI + POSITIVE_RESPONSE_OFFSET:
                if len(app) < 3:
                    continue
                did = (app[1] << 8) | app[2]
                key = ("rdbi", did)
                poll = in_flight.pop(key, None)
                if poll is None:
                    continue
                poll.response_ms = f.ms
                poll.value = bytes(app[3:])
                completed.append(poll)

    # Anything still in_flight at end of capture: emit as request-only
    # (no response) for visibility but mark as such via response_ms = -1.
    for poll in in_flight.values():
        completed.append(poll)

    # Aggregate.
    by_addr: dict[tuple[str, int], _AggregatedAddress] = {}
    for p in completed:
        key = (p.kind, p.address)
        agg = by_addr.setdefault(
            key, _AggregatedAddress(address=p.address, kind=p.kind)
        )
        agg.poll_count += 1
        agg.request_ms.append(p.request_ms)
        if p.value is not None:
            agg.response_count += 1
            if len(agg.response_values) < max_value_samples:
                agg.response_values.append(p.value)

    return sorted(by_addr.values(), key=lambda a: (a.kind, a.address)), warnings


def _periods_ms(timestamps: list[int]) -> list[int]:
    if len(timestamps) < 2:
        return []
    s = sorted(timestamps)
    return [s[i + 1] - s[i] for i in range(len(s) - 1)]


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )
    p.add_argument("log", type=Path, help="sniff log from subuwutuner-cli sniff")
    p.add_argument("--output", "-o", type=Path, default=None, help="output JSON (default: stdout)")
    p.add_argument(
        "--request-id", type=lambda s: int(s, 0), default=0x7E0,
        help="CAN ID for host→ECU requests (default 0x7E0)",
    )
    p.add_argument(
        "--response-id", type=lambda s: int(s, 0), default=0x7E8,
        help="CAN ID for ECU→host responses (default 0x7E8)",
    )
    p.add_argument(
        "--max-samples", type=int, default=8,
        help="max distinct value samples to retain per address (default 8)",
    )
    args = p.parse_args(argv)

    if not args.log.exists():
        print(f"error: {args.log} does not exist", file=sys.stderr)
        return 1
    frames = sniff_common.parse_log(args.log)
    print(f"parsed {len(frames)} frames from {args.log}", file=sys.stderr)

    aggregated, warnings = extract_polls(
        frames, args.request_id, args.response_id, max_value_samples=args.max_samples
    )
    for w in warnings:
        print(f"warn: {w}", file=sys.stderr)
    print(f"aggregated {len(aggregated)} distinct address(es)", file=sys.stderr)

    polls_json = []
    for agg in aggregated:
        periods = _periods_ms(agg.request_ms)
        polls_json.append({
            "kind": agg.kind,
            "address": f"0x{agg.address:08X}" if agg.kind == "rmba" else f"0x{agg.address:04X}",
            "poll_count": agg.poll_count,
            "response_count": agg.response_count,
            "poll_period_ms_p50": int(statistics.median(periods)) if periods else None,
            "poll_period_ms_min": min(periods) if periods else None,
            "poll_period_ms_max": max(periods) if periods else None,
            "value_samples": [v.hex().upper() for v in agg.response_values],
        })

    out = {
        "schema": "subuwutuner.poll.v1",
        "request_id": f"0x{args.request_id:03X}",
        "response_id": f"0x{args.response_id:03X}",
        "polls": polls_json,
    }
    payload = json.dumps(out, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n", encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
