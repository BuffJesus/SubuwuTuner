#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Regenerate the synthetic ROM under fixtures/demo.stune/.

Run from the repo root:
    python scripts/gen-demo-fixture.py

Produces fixtures/demo.stune/source.bin and working.bin (initially identical),
both matching the layout described by fixtures/demo-pack/.
"""

from __future__ import annotations

import os
import struct
from pathlib import Path

ROM_SIZE = 1024


def build_rom() -> bytes:
    data = bytearray(ROM_SIZE)

    # Calibration ID at 0x0008 (matches fixtures/demo-pack/pack.toml).
    data[0x08:0x10] = b"AS80U   "

    # Some embedded strings the ASCII scanner will surface in `rom-info`.
    data[0x18:0x18 + 13] = b"DEMO_2019_BIN"
    data[0x80:0x80 + 24] = b"(c) Synthetic Motors LLC"

    # RPM axis (uint16_be) at 0x0040: 1000, 2000, 3000, 4000 rpm.
    data[0x40:0x48] = struct.pack(">HHHH", 1000, 2000, 3000, 4000)

    # Load axis (uint16_be * 0.001 → 1.0, 2.0, 3.0 g/rev) at 0x0050.
    data[0x50:0x56] = struct.pack(">HHH", 1000, 2000, 3000)

    # Fuel table at 0x0100 — uniform 14.7 AFR (raw 147 across 12 cells).
    data[0x100:0x10C] = bytes([147] * 12)

    # Boost target at 0x0200 — realistic-looking ramp:
    #   load 1.0 g/rev:  50  60  70  70  kPa  (raw 200 220 240 240)
    #   load 2.0 g/rev:  60  70  75  75
    #   load 3.0 g/rev:  70  75  77.5 77.5
    data[0x200:0x20C] = bytes([
        200, 220, 240, 240,
        220, 240, 250, 250,
        240, 250, 255, 255,
    ])

    # EGR duty at 0x0300 — zero everywhere.
    data[0x300:0x30C] = bytes([0] * 12)

    return bytes(data)


def main() -> None:
    out_dir = Path("fixtures/demo.stune")
    out_dir.mkdir(parents=True, exist_ok=True)
    payload = build_rom()
    (out_dir / "source.bin").write_bytes(payload)
    (out_dir / "working.bin").write_bytes(payload)
    print(f"Wrote {len(payload)} bytes to {out_dir}/source.bin and working.bin")


if __name__ == "__main__":
    main()
