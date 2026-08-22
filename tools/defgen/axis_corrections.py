# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Firmware-derived axis-length corrections for generated packs.

The RomRaider source XML under-declares `sizex`/`sizey` on ~15 axis
classes for the VA WRX (wastegate, boost, AVCS ignition, requested
torque, injectors, EGR, ...): a per-class arithmetic bug halves the
axis length, so the generated def renders (and lets you edit) only half
of the most-tuned tables. The firmware-true lengths were recovered by
walking the interpolation descriptors against plaintext ROMs
(findings/def-corrections-2026-08-21 / atlas-descriptor-resolve).

This module carries those corrections as *facts* (an axis at flash
address A is N entries long) and overrides the length of any axis whose
address matches. Correcting the axis length automatically fixes every
table that references it, because a table's grid shape is derived from
its axis lengths.

The correction data lives in `corrections/axis_length_corrections.tsv`
(columns: axis_addr, firmware_len, def_len, ratio). It is applied at
generation time via `defgen --axis-corrections <tsv>`; addresses that do
not appear in a given pack are simply ignored.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable, Protocol

DEFAULT_CORRECTIONS = (
    Path(__file__).resolve().parent / "corrections" / "axis_length_corrections.tsv"
)


class _HasAddressLength(Protocol):
    address: int
    length: int


def load_axis_corrections(path: Path) -> dict[int, int]:
    """Parse the corrections TSV into {axis_address: firmware_length}.

    Skips the header row and any blank / comment (`#`) lines. Raises
    ValueError on a malformed data row so a typo fails loudly rather
    than silently dropping a correction.
    """
    corrections: dict[int, int] = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        # Tolerate the header row (non-hex first column).
        if lineno == 1 and not cols[0].lower().startswith("0x"):
            continue
        if len(cols) < 2:
            raise ValueError(
                f"{path}:{lineno}: expected at least 2 tab-separated columns, "
                f"got {len(cols)}"
            )
        try:
            addr = int(cols[0], 16)
            firmware_len = int(cols[1])
        except ValueError as exc:
            raise ValueError(f"{path}:{lineno}: malformed row {cols!r}") from exc
        if firmware_len <= 0:
            raise ValueError(
                f"{path}:{lineno}: firmware_len must be positive, got {firmware_len}"
            )
        corrections[addr] = firmware_len
    return corrections


def apply_axis_corrections(
    axes: Iterable[_HasAddressLength], corrections: dict[int, int]
) -> list[tuple[int, int, int]]:
    """Override the length of any axis whose address is in `corrections`.

    Mutates each matching axis in place. Returns one (address, old_length,
    new_length) tuple per axis actually changed (skips axes already at the
    firmware length, so re-running is a no-op).
    """
    changed: list[tuple[int, int, int]] = []
    for axis in axes:
        new_len = corrections.get(axis.address)
        if new_len is None or axis.length == new_len:
            continue
        changed.append((axis.address, axis.length, new_len))
        axis.length = new_len
    return changed
