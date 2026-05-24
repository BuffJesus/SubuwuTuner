#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Apply a constant byte offset to every address in a SubuwuTuner
TOML pack. Used to correct for ROM-extraction boundary mismatches
where the XML source was generated against a ROM image that
included (or excluded) a bootloader prefix relative to the
plaintext rom.bin the pack will actually be validated against.

Concrete case (2026-05-24): the Findings analyst-side LF79103P.xml
was extracted against a ROM image that pre-pended a 0x6000-byte
bootloader region. Our `fixtures/private/roms_plaintext_by_cid/
2017_LF79103P/rom.bin` does NOT include that prefix. Every
`address = 0x…` line in the generated pack is therefore 0x6000
too high. Applying `--offset -0x6000` produces an anchored pack
where validate.py finds 12 PASS instead of 0.

Affected fields (every `address = 0x…` line in any of these
sections):
  - [[identification]] -> cid_address
  - [[table]]          -> address
  - axis records nested under [[table]] (if any)

Other fields with hex literals (sizes, scaling factors, ID strings,
file sizes) are not touched.

Negative offsets shift addresses DOWN (subtract). Positive offsets
shift UP. Addresses that would go negative are flagged but written
as zero with a warning so the malformed pack is recognizable.

Usage:
  python tools/defgen/apply_offset.py INPUT.toml --offset -0x6000 \\
      --output OUTPUT.toml

  python tools/defgen/apply_offset.py INPUT.toml --offset -0x6000
      # writes to stdout if --output omitted
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Optional


ADDRESS_LINE = re.compile(
    r'^(?P<lead>\s*(?:address|cid_address)\s*=\s*)0x(?P<hex>[0-9A-Fa-f]+)(?P<tail>\s*(?:#.*)?)$'
)


def parse_signed_offset(s: str) -> int:
    """Accept -0x6000, 0x6000, -24576, 24576."""
    s = s.strip()
    negative = False
    if s.startswith("-"):
        negative = True
        s = s[1:]
    if s.startswith("+"):
        s = s[1:]
    if s.lower().startswith("0x"):
        v = int(s, 16)
    else:
        v = int(s, 10)
    return -v if negative else v


def shift_pack(text: str, offset: int) -> tuple[str, int, int]:
    """Apply `offset` to every address-line in `text`. Returns
    (new_text, num_shifted, num_clamped_to_zero).
    """
    shifted = 0
    clamped = 0
    out_lines = []
    for line in text.split("\n"):
        m = ADDRESS_LINE.match(line)
        if not m:
            out_lines.append(line)
            continue
        old = int(m["hex"], 16)
        new = old + offset
        if new < 0:
            clamped += 1
            new = 0
        out_lines.append(f"{m['lead']}0x{new:08X}{m['tail']}")
        shifted += 1
    return "\n".join(out_lines), shifted, clamped


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", type=Path, help="input TOML pack")
    parser.add_argument(
        "--offset",
        required=True,
        type=parse_signed_offset,
        help="byte offset to add to every address (e.g. -0x6000)",
    )
    parser.add_argument(
        "--output",
        "-o",
        type=Path,
        default=None,
        help="output pack (default: stdout)",
    )
    args = parser.parse_args(argv)

    try:
        text = args.input.read_text(encoding="utf-8")
    except OSError as e:
        print(f"apply_offset: cannot read {args.input}: {e}", file=sys.stderr)
        return 1

    new_text, shifted, clamped = shift_pack(text, args.offset)
    sign = "+" if args.offset >= 0 else "-"
    abs_off = abs(args.offset)
    print(
        f"apply_offset: shifted {shifted} addresses by {sign}0x{abs_off:X} "
        f"({sign}{abs_off})",
        file=sys.stderr,
    )
    if clamped:
        print(
            f"apply_offset: WARNING - {clamped} addresses would have gone "
            f"negative; clamped to 0x00000000",
            file=sys.stderr,
        )

    if args.output is None:
        sys.stdout.write(new_text)
    else:
        args.output.write_text(new_text, encoding="utf-8")
        print(f"apply_offset: wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
