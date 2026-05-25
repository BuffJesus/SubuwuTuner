#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Extract Subaru UDS SecurityAccess (SID 0x27) seed/key pairs from a
SubuwuTuner sniff log.

Workflow:
  1. Plug COBB AccessPort (or any authenticated tuning tool) and the OBDX
     Pro VX into the OBD-II port via a Y-cable.
  2. Start the sniffer in another shell:
       subuwutuner-cli sniff --transport obdx --device COM3 \\
           --output capture.log --filter 0x7E0,0x7E8
  3. Trigger 10-15 connect cycles from the tuning tool (disconnect +
     reconnect AP, or do anything that re-authenticates the session).
  4. Ctrl+C the sniff, then run this script:
       python tools/extract_subaru_sa.py capture.log --output pairs.json

The output JSON is a list of {seed: "HEX", key: "HEX"} pairs the eventual
algorithm-solver script consumes. Lots of pairs is good — overdetermines
the lookup tables and makes the solve robust to capture noise.

ISO-TP unwrapping is built in: this script handles the PCI byte and joins
multi-frame responses if the ECU ever fragments a seed across two CAN
frames (which it won't for 4-byte SA on a stock Subaru, but the SF/FF/CF
logic is here for safety).

Input format is the SubuwuTuner sniff log (v1):
    # subuwutuner sniff log v1
    # format: <elapsed_ms> 0x<can_id> <hex bytes...>
    523  0x7E0  02 27 01 00 00 00 00 00
    537  0x7E8  06 67 01 DE AD BE EF 00
    554  0x7E0  06 27 02 12 34 56 78 00
    571  0x7E8  02 67 02 00 00 00 00 00
"""

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


SUBARU_REQUEST_ID = 0x7E0
SUBARU_RESPONSE_ID = 0x7E8

SID_SECURITY_ACCESS = 0x27
POSITIVE_RESPONSE_OFFSET = 0x40

SA_RESPONSE_SID = SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET  # 0x67


@dataclass
class Frame:
    ms: int
    can_id: int
    data: bytes


def parse_log(path: Path) -> list[Frame]:
    """Read a SubuwuTuner sniff log into Frame objects."""
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
                can_id = int(parts[1], 16) if parts[1].lower().startswith("0x") else int(parts[1])
                data = bytes(int(h, 16) for h in parts[2].split())
            except ValueError as e:
                print(f"warn: {path}:{lineno}: parse failed ({e}) — skipping", file=sys.stderr)
                continue
            out.append(Frame(ms=ms, can_id=can_id, data=data))
    return out


def strip_iso_tp_sf(data: bytes) -> Optional[bytes]:
    """Strip the ISO-TP PCI byte from a single-frame payload.

    Returns the application-layer bytes (the part after the PCI) or
    None if the frame isn't a clean SF. We don't try to handle FF/CF
    reassembly here — Subaru SA requests/responses always fit in one
    CAN frame so anything multi-frame on the SA exchange is bogus.
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


@dataclass
class SaPair:
    seed: bytes
    key: bytes
    request_seed_ms: int
    seed_response_ms: int
    send_key_ms: int
    key_ack_ms: int = -1  # not always present (positive ack is short)
    level: int = 1        # 1 (sub-fn 0x01/0x02), 3 (0x03/0x04), 5 (0x05/0x06)


# UDS SecurityAccess sub-function pairs we recognize. Each tuple is
# (request_seed_sub, send_key_sub, level). Subaru DI engine ECUs expose
# all three on Gen-A.2 silicon (LF79 family +); Gen-A 1MB only has L1.
SA_LEVEL_PAIRS: list[tuple[int, int, int]] = [
    (0x01, 0x02, 1),  # bootloader-unlock / RMBA / RequestDownload
    (0x03, 0x04, 3),  # Gen-A.2: live RAM-write privileges
    (0x05, 0x06, 5),  # Gen-A.2: additional unlock surface
]


def extract_pairs(frames: list[Frame]) -> list[SaPair]:
    """Walk the frame stream looking for SecurityAccess exchanges at any
    supported level (L1 / L3 / L5). Emits one SaPair per completed
    `27 LL / 67 LL SEED / 27 LL+1 KEY / 67 LL+1` sequence."""
    pairs: list[SaPair] = []

    # Index sub-fns by direction for O(1) dispatch.
    req_seed_subs = {p[0]: p for p in SA_LEVEL_PAIRS}
    send_key_subs = {p[1]: p for p in SA_LEVEL_PAIRS}

    # State machine: collect the 4 events in order. If anything else
    # appears on the SA path between them we discard the partial pair
    # — better to drop a noisy capture than emit a corrupt (seed, key).
    pending_level: Optional[int] = None       # 1 / 3 / 5
    pending_req_sub: Optional[int] = None     # 0x01 / 0x03 / 0x05
    pending_key_sub: Optional[int] = None     # 0x02 / 0x04 / 0x06
    pending_seed_req_ms: Optional[int] = None
    pending_seed: Optional[bytes] = None
    pending_seed_ms: Optional[int] = None
    pending_send_key_ms: Optional[int] = None
    pending_key: Optional[bytes] = None

    def reset() -> None:
        nonlocal pending_level, pending_req_sub, pending_key_sub
        nonlocal pending_seed_req_ms, pending_seed, pending_seed_ms
        nonlocal pending_send_key_ms, pending_key
        pending_level = None
        pending_req_sub = None
        pending_key_sub = None
        pending_seed_req_ms = None
        pending_seed = None
        pending_seed_ms = None
        pending_send_key_ms = None
        pending_key = None

    for f in frames:
        if f.can_id != SUBARU_REQUEST_ID and f.can_id != SUBARU_RESPONSE_ID:
            continue
        app = strip_iso_tp_sf(f.data)
        if app is None or len(app) < 2:
            continue

        sid = app[0]
        sub = app[1]
        body = app[2:]

        # 27 LL — request seed (LL ∈ {0x01, 0x03, 0x05}). Any new
        # request-seed restarts the state machine even if we were
        # mid-collection at a different level; the tool's choice of
        # level overrides whatever we were tracking.
        if (
            f.can_id == SUBARU_REQUEST_ID
            and sid == SID_SECURITY_ACCESS
            and sub in req_seed_subs
        ):
            req_pair = req_seed_subs[sub]
            reset()
            pending_req_sub = req_pair[0]
            pending_key_sub = req_pair[1]
            pending_level = req_pair[2]
            pending_seed_req_ms = f.ms
            continue

        # 67 LL — positive seed response. Sub-fn must match the
        # outstanding request; mismatches are ignored (some other
        # tester pair we don't care about, or stale crosstalk).
        if (
            f.can_id == SUBARU_RESPONSE_ID
            and sid == SA_RESPONSE_SID
            and pending_req_sub is not None
            and sub == pending_req_sub
            and pending_seed_req_ms is not None
        ):
            if len(body) < 1:
                reset()
                continue
            pending_seed = bytes(body)
            pending_seed_ms = f.ms
            continue

        # 27 LL+1 KEY — send key.
        if (
            f.can_id == SUBARU_REQUEST_ID
            and sid == SID_SECURITY_ACCESS
            and pending_key_sub is not None
            and sub == pending_key_sub
            and pending_seed is not None
        ):
            if len(body) < 1:
                reset()
                continue
            pending_key = bytes(body)
            pending_send_key_ms = f.ms
            continue

        # 67 LL+1 — positive key ack (no payload, the unlock event).
        if (
            f.can_id == SUBARU_RESPONSE_ID
            and sid == SA_RESPONSE_SID
            and pending_key_sub is not None
            and sub == pending_key_sub
            and pending_key is not None
            and pending_seed is not None
            and pending_seed_ms is not None
            and pending_seed_req_ms is not None
            and pending_send_key_ms is not None
            and pending_level is not None
        ):
            pairs.append(
                SaPair(
                    seed=pending_seed,
                    key=pending_key,
                    request_seed_ms=pending_seed_req_ms,
                    seed_response_ms=pending_seed_ms,
                    send_key_ms=pending_send_key_ms,
                    key_ack_ms=f.ms,
                    level=pending_level,
                )
            )
            reset()
            continue

        # 7F 27 NN — SA negative response; reset state, the tool will
        # retry from scratch on the next attempt.
        if (
            f.can_id == SUBARU_RESPONSE_ID
            and sid == 0x7F
            and sub == SID_SECURITY_ACCESS
        ):
            reset()
            continue

    return pairs


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("log", type=Path, help="sniff log from subuwutuner-cli sniff")
    p.add_argument(
        "--output",
        "-o",
        type=Path,
        default=None,
        help="output JSON path (default: stdout)",
    )
    global SUBARU_REQUEST_ID, SUBARU_RESPONSE_ID
    p.add_argument(
        "--request-id",
        type=lambda s: int(s, 0),
        default=SUBARU_REQUEST_ID,
        help=f"CAN ID for SA requests (default 0x{SUBARU_REQUEST_ID:03X})",
    )
    p.add_argument(
        "--response-id",
        type=lambda s: int(s, 0),
        default=SUBARU_RESPONSE_ID,
        help=f"CAN ID for SA responses (default 0x{SUBARU_RESPONSE_ID:03X})",
    )
    p.add_argument(
        "--strip-zero-seed-polls",
        action="store_true",
        help=(
            "Drop pairs whose seed is all-zero (ISO 14229 §11.4.2.4 "
            "'already unlocked' polls — the ECU's way of saying "
            "'no challenge needed, you're already authenticated'). "
            "These are session keepalives, not algorithmic "
            "challenges. The key field in a zero-seed pair is "
            "whatever constant the tester sends back, not a "
            "derivation output, so feeding them to verify-gen-a / "
            "recover-gen-a wastes search time and pollutes the "
            "summary. Off by default to keep the extraction lossless."
        ),
    )
    args = p.parse_args()

    # Allow overriding the default Subaru pair for other modules / non-Subaru.
    SUBARU_REQUEST_ID = args.request_id
    SUBARU_RESPONSE_ID = args.response_id

    if not args.log.exists():
        print(f"error: {args.log} does not exist", file=sys.stderr)
        return 1
    frames = parse_log(args.log)
    print(f"parsed {len(frames)} frames from {args.log}", file=sys.stderr)

    pairs = extract_pairs(frames)
    # Optional: drop zero-seed keepalive polls before emission.
    if args.strip_zero_seed_polls:
        before = len(pairs)
        pairs = [p for p in pairs if any(b != 0 for b in p.seed)]
        dropped = before - len(pairs)
        if dropped > 0:
            print(
                f"  dropped {dropped} zero-seed poll(s) "
                "(--strip-zero-seed-polls)",
                file=sys.stderr,
            )
    # Per-level breakdown for the stderr summary.
    by_level: dict[int, int] = {}
    for pair in pairs:
        by_level[pair.level] = by_level.get(pair.level, 0) + 1
    breakdown = ", ".join(f"L{lvl}={n}" for lvl, n in sorted(by_level.items()))
    if breakdown:
        print(
            f"extracted {len(pairs)} SecurityAccess pair(s)  [{breakdown}]",
            file=sys.stderr,
        )
    else:
        print(f"extracted {len(pairs)} SecurityAccess pair(s)", file=sys.stderr)

    out = {
        "schema": "subuwutuner.sa.v1",
        "request_id": f"0x{SUBARU_REQUEST_ID:03X}",
        "response_id": f"0x{SUBARU_RESPONSE_ID:03X}",
        "pairs": [
            {
                "seed": pair.seed.hex().upper(),
                "key": pair.key.hex().upper(),
                "level": pair.level,
                "request_seed_ms": pair.request_seed_ms,
                "seed_response_ms": pair.seed_response_ms,
                "send_key_ms": pair.send_key_ms,
                "key_ack_ms": pair.key_ack_ms,
            }
            for pair in pairs
        ],
    }
    payload = json.dumps(out, indent=2)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
