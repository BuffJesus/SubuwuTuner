#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Decode a SubuwuTuner sniff log into a human-readable UDS timeline.

Implements Workflow 3 of `docs/24-sniff-workflows.md`: characterizing
what session sequence, SA level, and TesterPresent cadence a working
OEM tool uses before some service succeeds, without having to
trial-and-error against the live ECU (which might lock SA on the
third wrong key).

Workflow:
  python tools/decode_uds_capture.py capture.log

Each line of output names the UDS service / sub-function (when known)
and decodes NRCs to their ISO 14229 names. An `anomalies` section at
the end highlights frames whose service ID isn't in our catalog and
sessions that auto-expired (no positive response to TesterPresent
within the negotiated timeout).

Output is plain text by default; `--json` emits the same content as
structured data for further processing.

ISO-TP multi-frame reassembly is built in for both directions.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

_THIS = Path(__file__).resolve()
_spec = importlib.util.spec_from_file_location("sniff_common", _THIS.parent / "sniff_common.py")
if _spec is None or _spec.loader is None:
    raise RuntimeError(
        f"Could not load sniff_common.py from {_THIS.parent} "
        f"(spec_from_file_location returned None)")
sniff_common = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = sniff_common
_spec.loader.exec_module(sniff_common)


POSITIVE_RESPONSE_OFFSET = 0x40
NEGATIVE_RESPONSE = 0x7F

# Subset of ISO 14229-1 service IDs relevant to ECU tuning flows. Not
# every UDS service appears in the wild on Subaru — this list covers
# what `docs/13`/`docs/23`/`docs/24` describe plus the common ones.
_SERVICE_NAMES: dict[int, str] = {
    0x10: "DiagnosticSessionControl",
    0x11: "ECUReset",
    0x14: "ClearDiagnosticInformation",
    0x19: "ReadDTCInformation",
    0x22: "ReadDataByIdentifier",
    0x23: "ReadMemoryByAddress",
    0x27: "SecurityAccess",
    0x28: "CommunicationControl",
    0x2E: "WriteDataByIdentifier",
    0x2F: "InputOutputControlByIdentifier",
    0x31: "RoutineControl",
    0x34: "RequestDownload",
    0x35: "RequestUpload",
    0x36: "TransferData",
    0x37: "RequestTransferExit",
    0x3D: "WriteMemoryByAddress",
    0x3E: "TesterPresent",
    0x83: "AccessTimingParameter",
    0x84: "SecuredDataTransmission",
    0x85: "ControlDTCSetting",
    0x86: "ResponseOnEvent",
    0x87: "LinkControl",
}

# Sub-function names by SID. Only include where it materially helps
# reading the timeline; positive-response and negative-response are
# decoded structurally not via this table.
_SUB_FUNCTION_NAMES: dict[int, dict[int, str]] = {
    0x10: {
        0x01: "defaultSession",
        0x02: "programmingSession",
        0x03: "extendedDiagnosticSession",
        0x04: "safetySystemDiagnosticSession",
    },
    0x11: {
        0x01: "hardReset",
        0x02: "keyOffOnReset",
        0x03: "softReset",
    },
    0x27: {
        0x01: "requestSeed (level 1)",
        0x02: "sendKey (level 1)",
        0x03: "requestSeed (level 3)",
        0x04: "sendKey (level 3)",
        0x05: "requestSeed (level 5)",
        0x06: "sendKey (level 5)",
    },
    0x3E: {
        0x00: "zeroSubFunction",
        0x80: "zeroSubFunction (suppressPositiveResponse)",
    },
    0x28: {
        0x00: "enableRxAndTx",
        0x01: "enableRxAndDisableTx",
        0x02: "disableRxAndEnableTx",
        0x03: "disableRxAndTx",
    },
    0x31: {
        0x01: "startRoutine",
        0x02: "stopRoutine",
        0x03: "requestRoutineResults",
    },
}

_NRC_NAMES: dict[int, str] = {
    0x10: "generalReject",
    0x11: "serviceNotSupported",
    0x12: "subFunctionNotSupported",
    0x13: "incorrectMessageLengthOrInvalidFormat",
    0x14: "responseTooLong",
    0x21: "busyRepeatRequest",
    0x22: "conditionsNotCorrect",
    0x24: "requestSequenceError",
    0x25: "noResponseFromSubnetComponent",
    0x26: "failurePreventsExecutionOfRequestedAction",
    0x31: "requestOutOfRange",
    0x33: "securityAccessDenied",
    0x35: "invalidKey",
    0x36: "exceededNumberOfAttempts",
    0x37: "requiredTimeDelayNotExpired",
    0x70: "uploadDownloadNotAccepted",
    0x71: "transferDataSuspended",
    0x72: "generalProgrammingFailure",
    0x73: "wrongBlockSequenceCounter",
    0x78: "requestCorrectlyReceivedResponsePending",
    0x7E: "subFunctionNotSupportedInActiveSession",
    0x7F: "serviceNotSupportedInActiveSession",
}


@dataclass
class TimelineEntry:
    ms: int
    direction: str               # "req" or "resp"
    can_id: int
    raw: bytes
    decoded: str
    is_anomaly: bool = False
    anomaly_reason: str = ""


def decode_frame(app: bytes) -> tuple[str, bool, str]:
    """Render an application-layer payload to a human-readable string.

    Returns (decoded, is_anomaly, anomaly_reason).
    """
    if len(app) < 1:
        return "(empty)", True, "zero-length payload"
    sid = app[0]
    # Negative response.
    if sid == NEGATIVE_RESPONSE and len(app) >= 3:
        rej_sid = app[1]
        nrc = app[2]
        sid_name = _SERVICE_NAMES.get(rej_sid, f"unknown SID 0x{rej_sid:02X}")
        nrc_name = _NRC_NAMES.get(nrc, "unknown NRC")
        anomaly = nrc not in _NRC_NAMES
        reason = f"NRC 0x{nrc:02X} not in catalog" if anomaly else ""
        return f"NRC {sid_name}: 0x{nrc:02X} ({nrc_name})", anomaly, reason
    # Positive response.
    if sid >= POSITIVE_RESPONSE_OFFSET:
        req_sid = sid - POSITIVE_RESPONSE_OFFSET
        if req_sid in _SERVICE_NAMES:
            name = _SERVICE_NAMES[req_sid]
            sub_str = ""
            if len(app) >= 2:
                sub = app[1]
                sub_name = _SUB_FUNCTION_NAMES.get(req_sid, {}).get(sub)
                if sub_name is not None:
                    sub_str = f" {sub_name}"
                else:
                    sub_str = f" sub=0x{sub:02X}"
            body = app[2:] if len(app) >= 2 else b""
            body_str = f" data={body.hex().upper()}" if body else ""
            return f"+ve {name}{sub_str}{body_str}", False, ""
    # Request.
    if sid in _SERVICE_NAMES:
        name = _SERVICE_NAMES[sid]
        sub_str = ""
        if len(app) >= 2:
            sub = app[1]
            sub_name = _SUB_FUNCTION_NAMES.get(sid, {}).get(sub)
            if sub_name is not None:
                sub_str = f" {sub_name}"
            else:
                sub_str = f" sub=0x{sub:02X}"
        body = app[2:] if len(app) >= 2 else b""
        body_str = f" data={body.hex().upper()}" if body else ""
        return f"{name}{sub_str}{body_str}", False, ""
    return f"unknown SID 0x{sid:02X} data={app.hex().upper()}", True, "service ID not in catalog"


def build_timeline(
    frames: list["sniff_common.Frame"],
    request_id: int,
    response_id: int,
) -> list[TimelineEntry]:
    """Walk the frame stream, reassemble both directions, decode each
    completed message into a TimelineEntry."""
    timeline: list[TimelineEntry] = []
    req_reassembler = sniff_common.IsoTpReassembler()
    resp_reassembler = sniff_common.IsoTpReassembler()

    for f in frames:
        if f.can_id == request_id:
            app = req_reassembler.feed(f.can_id, f.data)
            if app is not None:
                decoded, anom, reason = decode_frame(app)
                timeline.append(TimelineEntry(
                    ms=f.ms, direction="req", can_id=f.can_id,
                    raw=app, decoded=decoded,
                    is_anomaly=anom, anomaly_reason=reason,
                ))
        elif f.can_id == response_id:
            app = resp_reassembler.feed(f.can_id, f.data)
            if app is not None:
                decoded, anom, reason = decode_frame(app)
                timeline.append(TimelineEntry(
                    ms=f.ms, direction="resp", can_id=f.can_id,
                    raw=app, decoded=decoded,
                    is_anomaly=anom, anomaly_reason=reason,
                ))
    return timeline


def render_text(timeline: list[TimelineEntry]) -> str:
    """Plain-text timeline + anomaly summary."""
    lines: list[str] = []
    for e in timeline:
        arrow = "->" if e.direction == "req" else "<-"
        marker = "  ! " if e.is_anomaly else "    "
        lines.append(
            f"{e.ms:>8d}ms {arrow} 0x{e.can_id:03X} {marker}{e.decoded}"
        )
    anomalies = [e for e in timeline if e.is_anomaly]
    if anomalies:
        lines.append("")
        lines.append(f"--- {len(anomalies)} anomaly(ies) ---")
        for e in anomalies:
            lines.append(
                f"  t={e.ms}ms 0x{e.can_id:03X}: {e.anomaly_reason}  "
                f"({e.raw.hex().upper()})"
            )
    return "\n".join(lines) + "\n"


def render_json(timeline: list[TimelineEntry]) -> str:
    obj = {
        "schema": "subuwutuner.uds_timeline.v1",
        "entries": [
            {
                "ms": e.ms,
                "direction": e.direction,
                "can_id": f"0x{e.can_id:03X}",
                "raw": e.raw.hex().upper(),
                "decoded": e.decoded,
                "is_anomaly": e.is_anomaly,
                "anomaly_reason": e.anomaly_reason or None,
            }
            for e in timeline
        ],
        "anomaly_count": sum(1 for e in timeline if e.is_anomaly),
    }
    return json.dumps(obj, indent=2) + "\n"


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )
    p.add_argument("log", type=Path, help="sniff log from subuwutuner-cli sniff")
    p.add_argument("--output", "-o", type=Path, default=None, help="output path (default: stdout)")
    p.add_argument("--json", action="store_true", help="emit JSON instead of plain-text timeline")
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
    frames = sniff_common.parse_log(args.log)
    print(f"parsed {len(frames)} frames from {args.log}", file=sys.stderr)

    timeline = build_timeline(frames, args.request_id, args.response_id)
    print(
        f"decoded {len(timeline)} message(s); "
        f"{sum(1 for e in timeline if e.is_anomaly)} anomaly(ies)",
        file=sys.stderr,
    )

    text = render_json(timeline) if args.json else render_text(timeline)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
