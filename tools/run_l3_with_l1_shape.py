#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Re-run L3 SA recovery using L1-shape byte order (identity shuffle).

Hypothesis: the L3 wire format on LF79103P is the same identity byte
order as L1 (s0,s1,s2,s3 -> packed BE; key state -> wire MSB-first),
NOT the (s1,s2,s0,s3) / (k3,k1,k2,k0) shuffle the analyst spec
prescribes. If true, three of four L3 pairs may verify against a
table the previous recovery harness missed because it was feeding
the Feistel inverse a mis-permuted 32-bit state.

This is a one-shot diagnostic, not a tool. Reads zero files; emits
human-readable result table to stdout.
"""
from __future__ import annotations

import sys
from pathlib import Path

# Reuse the verified primitives from solve_ssmcan1.py.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from solve_ssmcan1 import (  # type: ignore[import-untyped]
    GEN_A_RK_L1,
    GEN_A_RK_L35,
    _gen_a_wordswap32,
    gen_a_feistel_inverse,
)


# Captured L3 pairs (seed_hex, key_hex, label) — all 2026-05-24, byte
# verified against raw CAN logs on Desktop.
L3_PAIRS = [
    ("9FE376C9", "749DB819", "bus-check ms=67185"),
    ("ABCF96C2", "E47D4B22", "bus-check ms=76153"),
    ("61593479", "8EB11B73", "uninstall1 ms=52287"),
    ("23FF1830", "444E56A5", "reinstall ms=166349"),
]

# Captured L1 pair (sanity reference).
L1_PAIRS = [
    ("4BC3CC87", "A73FED09", "reinstall ms=170205"),
]


def derive_key_l1_shape(seed_bytes: bytes, rk: list[int]) -> bytes:
    """Run the Feistel-inverse path with L1 (identity) shuffle.

    seed_bytes: 4 wire seed bytes (positions [2..5] of the `67 LL` response).
    rk:         16 x uint16 round-key table.
    returns:    4 wire key bytes (would be at positions [2..5] of `27 LL+1`).
    """
    seed_packed = int.from_bytes(seed_bytes, "big")
    state_post_rounds = _gen_a_wordswap32(seed_packed)
    internal_key = gen_a_feistel_inverse(state_post_rounds, rk)
    return internal_key.to_bytes(4, "big")


def try_table(seed_hex: str, key_hex: str, rk: list[int]) -> bool:
    """Check whether `rk` + L1-shape shuffle reproduces the captured key."""
    seed = bytes.fromhex(seed_hex)
    expected_key = bytes.fromhex(key_hex)
    got = derive_key_l1_shape(seed, rk)
    return got == expected_key


def run_direct_checks() -> None:
    print("=" * 78)
    print("Direct check: L1-shape shuffle, stock tables")
    print("=" * 78)
    print(f"{'pair':<28} {'seed':>10} {'key':>10}  "
          f"{'RK_L1':>6}  {'RK_L35':>7}")
    print("-" * 78)

    # L1 control row.
    for seed_h, key_h, label in L1_PAIRS:
        a = "MATCH" if try_table(seed_h, key_h, GEN_A_RK_L1) else "no"
        b = "MATCH" if try_table(seed_h, key_h, GEN_A_RK_L35) else "no"
        print(f"  L1 {label:<24} {seed_h:>10} {key_h:>10}  "
              f"{a:>6}  {b:>7}")

    print("-" * 78)

    # L3 pairs treated as L1-shape.
    for seed_h, key_h, label in L3_PAIRS:
        a = "MATCH" if try_table(seed_h, key_h, GEN_A_RK_L1) else "no"
        b = "MATCH" if try_table(seed_h, key_h, GEN_A_RK_L35) else "no"
        print(f"  L3 {label:<24} {seed_h:>10} {key_h:>10}  "
              f"{a:>6}  {b:>7}")
    print()


def run_xor_mask_search(base_label: str, base_rk: list[int]) -> None:
    """Brute force a 16-bit XOR mask applied uniformly to every entry of
    `base_rk`. Search space 2**16; print any mask that fits >=2 of the
    L3 pairs (false-positive prob per pair = 2**-32, so 2 fits is
    overwhelmingly real)."""
    print("=" * 78)
    print(f"XOR-mask search over {base_label} "
          f"(L1-shape shuffle, 65,536 candidates)")
    print("=" * 78)

    hits: list[tuple[int, list[bool]]] = []
    for mask in range(0x10000):
        rk = [k ^ mask for k in base_rk]
        results = [try_table(s, k, rk) for (s, k, _) in L3_PAIRS]
        n_hit = sum(results)
        if n_hit >= 2:
            hits.append((mask, results))

    if not hits:
        print(f"  no XOR-mask of {base_label} fits >= 2 L3 pairs")
        print()
        return

    print(f"  found {len(hits)} mask(s) fitting >= 2 pairs:")
    for mask, results in hits[:20]:
        marks = "".join("Y" if r else "." for r in results)
        print(f"    mask=0x{mask:04X}  pairs=[{marks}]  "
              f"({sum(results)}/4)")
    if len(hits) > 20:
        print(f"    ... and {len(hits) - 20} more")
    print()


def run_single_substitution_search(base_label: str,
                                   base_rk: list[int]) -> None:
    """One entry of the 16-entry table replaced. 16 * 65,536 = ~1M."""
    print("=" * 78)
    print(f"Single-substitution search over {base_label} "
          f"(L1-shape shuffle, ~1M candidates)")
    print("=" * 78)

    hits: list[tuple[int, int, list[bool]]] = []
    for pos in range(16):
        for new_val in range(0x10000):
            rk = list(base_rk)
            rk[pos] = new_val
            results = [try_table(s, k, rk) for (s, k, _) in L3_PAIRS]
            if sum(results) >= 2:
                hits.append((pos, new_val, results))

    if not hits:
        print(f"  no single-position substitution of {base_label} "
              f"fits >= 2 L3 pairs")
        print()
        return

    print(f"  found {len(hits)} substitution(s) fitting >= 2 pairs:")
    for pos, val, results in hits[:20]:
        marks = "".join("Y" if r else "." for r in results)
        print(f"    pos={pos:2d}  val=0x{val:04X}  pairs=[{marks}]  "
              f"({sum(results)}/4)")
    if len(hits) > 20:
        print(f"    ... and {len(hits) - 20} more")
    print()


def main() -> int:
    run_direct_checks()
    run_xor_mask_search("RK_L1", GEN_A_RK_L1)
    run_xor_mask_search("RK_L35", GEN_A_RK_L35)
    run_single_substitution_search("RK_L1", GEN_A_RK_L1)
    run_single_substitution_search("RK_L35", GEN_A_RK_L35)
    return 0


if __name__ == "__main__":
    sys.exit(main())
