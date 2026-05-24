#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""
Solve the Subaru SSMCAN1 SecurityAccess key tables from captured
(seed, key) pairs.

Workflow (real captures):
  1. Capture a sniff log while COBB AccessPort (or any tool with a working
     key function) authenticates against the ECU via Y-cable:
       subuwutuner-cli sniff --transport obdx --device COM3 \\
           --output capture.log --filter 0x7E0,0x7E8
  2. Extract (seed, key) pairs:
       python tools/extract_subaru_sa.py capture.log --output pairs.json
  3. Solve for the algorithm tables:
       python tools/solve_ssmcan1.py solve pairs.json --output ssmcan1.cpp
  4. Drop the emitted C++ into a downstream fork (NOT upstream — the
     algorithm contamination is exactly what the plug-in architecture
     in docs/23-security-access.md avoids), and wire via
     `Flasher::set_security_key_fn(...)`.

Workflow (synthetic / scaffold validation, no hardware needed):
  python tools/solve_ssmcan1.py generate --pairs 32 \\
      --output synthetic-pairs.json --tables-out synthetic-tables.json
  python tools/solve_ssmcan1.py verify synthetic-pairs.json \\
      --tables synthetic-tables.json
  python tools/solve_ssmcan1.py solve synthetic-pairs.json \\
      --output recovered.cpp --report

The synthetic flow proves the end-to-end harness (algorithm + generator
+ solver + emitter) is self-consistent. It does NOT validate that the
algorithm structure encoded below matches the real ECU's algorithm —
that's what the first real capture is for.

---

ALGORITHM STRUCTURE (clean-room from public documentation)

The SSMCAN1 algorithm is publicly described as a 16-round XOR cipher
parameterized by two lookup tables (64 bytes total) with a 3-bit
barrel-roll per round and a final top/bottom byte swap on the 4-byte
output. The PRECISE per-round operation order is not unambiguously
documented in the references that are license-compatible to read; the
operation order encoded in `ssmcan1_encrypt` below is ONE plausible
interpretation of that structural description.

If a real-capture solve returns UNSAT, the operation order is the
most likely thing to refine. The scaffold's synthetic round-trip
test validates only that the solver harness works against whatever
structure is encoded — it does not validate the structure itself.
This separation is intentional: get the tooling right first, then
iterate the structure once real data is available.

EMPIRICAL FINDING (2026-05-23): the algorithm as currently encoded
is provably GF(2)-linear in (seed, tables) — i.e.,
`encrypt(s, T) == encrypt(s, 0) XOR encrypt(0, T)` for every seed.
Verify with:

    python tools/solve_ssmcan1.py linearity-check

Consequence: the 512-bit table space maps to AT MOST 32 bits of
effective constant offset. ~480 bits of table parameter are
mathematically invisible — multiple table-sets produce the IDENTICAL
encryption function across all 2^32 possible seeds. The `solve`
subcommand will therefore correctly report "ambiguous" on synthetic
captures from this encoding regardless of how many pairs are
provided (verified at 200 pairs, rng-seed=1234: still ambiguous;
recovered tables differ from ground truth in 30/32 IKB bytes and
32/32 KPT bytes).

Against a real Subaru capture, `solve` will most likely return
UNSAT — real SSMCAN1 must contain nonlinear elements the current
encoding lacks. That UNSAT is itself the useful signal: it confirms
which structural assumption is wrong and tells us to refine.

The most likely missing structural element: `KeyPartsTable` is
probably INDEXED by state bits (S-box style) rather than XOR'd in
as a positional constant. S-box indexing makes table content
observable in the output, which is what lets pair captures pin the
values. This refinement is gated on real-capture data — guessing
further without it would compound speculation.

Z3 is required for the actual solve (`pip install z3-solver`). The
`generate`, `verify`, and `linearity-check` subcommands work
without z3.
"""

from __future__ import annotations

import argparse
import json
import secrets
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# Algorithm constants (per public structural description).
NUM_ROUNDS = 16
IKB_ENTRIES = 16        # IndexKeyBase: 16 entries × 2 bytes = 32 bytes
IKB_ENTRY_BYTES = 2
KPT_ENTRIES = 32        # KeyPartsTable: 32 entries × 1 byte = 32 bytes
KPT_ENTRY_BYTES = 1
SEED_BYTES = 4
KEY_BYTES = 4
TABLE_BYTES_TOTAL = (IKB_ENTRIES * IKB_ENTRY_BYTES) + (KPT_ENTRIES * KPT_ENTRY_BYTES)  # 64


@dataclass
class Tables:
    """The 64 bytes of algorithm constants."""
    ikb: bytes  # 32 bytes (16 × u16, big-endian)
    kpt: bytes  # 32 bytes (32 × u8)

    def __post_init__(self) -> None:
        if len(self.ikb) != IKB_ENTRIES * IKB_ENTRY_BYTES:
            raise ValueError(f"ikb must be {IKB_ENTRIES * IKB_ENTRY_BYTES} bytes, got {len(self.ikb)}")
        if len(self.kpt) != KPT_ENTRIES * KPT_ENTRY_BYTES:
            raise ValueError(f"kpt must be {KPT_ENTRIES * KPT_ENTRY_BYTES} bytes, got {len(self.kpt)}")

    @property
    def ikb_u16(self) -> list[int]:
        return [int.from_bytes(self.ikb[i * 2:i * 2 + 2], "big") for i in range(IKB_ENTRIES)]

    @property
    def kpt_u8(self) -> list[int]:
        return list(self.kpt)


# ──────────────────────────────────────────────────────────────────────
# Algorithm
# ──────────────────────────────────────────────────────────────────────

def _rotr32(x: int, n: int) -> int:
    n &= 31
    return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF


def ssmcan1_encrypt(seed: bytes, tables: Tables) -> bytes:
    """Compute the 4-byte SSMCAN1 key for a 4-byte seed under `tables`.

    Structure (one plausible interpretation of public docs — see module
    docstring): 16 rounds. Per round r:
      * state ^= IndexKeyBase[r] << 16          (high half mixed)
      * state ^= (KeyPartsTable[r*2] << 8) | KeyPartsTable[r*2+1]
                                                 (low half mixed)
      * state = ror32(state, 3)                  (barrel-roll right 3 bits)
    Final: top/bottom 16-bit swap.

    Same input + same tables → same output (pure, deterministic).
    """
    if len(seed) != SEED_BYTES:
        raise ValueError(f"seed must be {SEED_BYTES} bytes")

    state = int.from_bytes(seed, "big")
    ikb = tables.ikb_u16
    kpt = tables.kpt_u8

    for r in range(NUM_ROUNDS):
        state ^= (ikb[r] & 0xFFFF) << 16
        state ^= ((kpt[r * 2] & 0xFF) << 8) | (kpt[r * 2 + 1] & 0xFF)
        state = _rotr32(state, 3)

    high = (state >> 16) & 0xFFFF
    low = state & 0xFFFF
    state = (low << 16) | high

    return state.to_bytes(KEY_BYTES, "big")


# ──────────────────────────────────────────────────────────────────────
# Synthetic generator
# ──────────────────────────────────────────────────────────────────────

def generate_tables(rng_seed: Optional[int] = None) -> Tables:
    """Produce a random Tables. Deterministic when `rng_seed` is set."""
    if rng_seed is None:
        ikb = secrets.token_bytes(IKB_ENTRIES * IKB_ENTRY_BYTES)
        kpt = secrets.token_bytes(KPT_ENTRIES * KPT_ENTRY_BYTES)
    else:
        import random
        rng = random.Random(rng_seed)
        ikb = bytes(rng.randint(0, 255) for _ in range(IKB_ENTRIES * IKB_ENTRY_BYTES))
        kpt = bytes(rng.randint(0, 255) for _ in range(KPT_ENTRIES * KPT_ENTRY_BYTES))
    return Tables(ikb=ikb, kpt=kpt)


@dataclass
class GeneratedPair:
    seed: bytes
    key: bytes


def generate_pairs(
    tables: Tables, num_pairs: int, rng_seed: Optional[int] = None
) -> list[GeneratedPair]:
    """Generate `num_pairs` (seed, key) pairs under `tables`. Pseudo-random
    seeds; if `rng_seed` is set the seed sequence is deterministic."""
    if rng_seed is None:
        seeds = [secrets.token_bytes(SEED_BYTES) for _ in range(num_pairs)]
    else:
        import random
        rng = random.Random(rng_seed)
        seeds = [bytes(rng.randint(0, 255) for _ in range(SEED_BYTES)) for _ in range(num_pairs)]
    return [GeneratedPair(seed=s, key=ssmcan1_encrypt(s, tables)) for s in seeds]


# ──────────────────────────────────────────────────────────────────────
# Pairs JSON I/O (matches tools/extract_subaru_sa.py output)
# ──────────────────────────────────────────────────────────────────────

@dataclass
class LoadedPairs:
    schema: str
    pairs: list[GeneratedPair] = field(default_factory=list)


def load_pairs(path: Path) -> LoadedPairs:
    """Read a pairs.json file produced by extract_subaru_sa.py (or the
    `generate` subcommand here). Tolerates both the full extractor output
    (with timing fields) and the slim generated form."""
    data = json.loads(path.read_text(encoding="utf-8"))
    schema = data.get("schema", "<unknown>")
    if schema not in ("subuwutuner.sa.v1", "subuwutuner.sa.synthetic.v1"):
        print(
            f"warn: {path}: unrecognized schema {schema!r}; assuming compatible",
            file=sys.stderr,
        )
    raw_pairs = data.get("pairs", [])
    out: list[GeneratedPair] = []
    for i, p in enumerate(raw_pairs):
        try:
            seed = bytes.fromhex(p["seed"])
            key = bytes.fromhex(p["key"])
        except (KeyError, ValueError) as e:
            print(f"warn: {path}: pair[{i}] skipped ({e})", file=sys.stderr)
            continue
        if len(seed) != SEED_BYTES or len(key) != KEY_BYTES:
            print(
                f"warn: {path}: pair[{i}] has wrong sizes "
                f"(seed={len(seed)}, key={len(key)}); skipped",
                file=sys.stderr,
            )
            continue
        out.append(GeneratedPair(seed=seed, key=key))
    return LoadedPairs(schema=schema, pairs=out)


def dump_pairs_json(pairs: list[GeneratedPair], path: Path) -> None:
    obj = {
        "schema": "subuwutuner.sa.synthetic.v1",
        "pairs": [{"seed": p.seed.hex().upper(), "key": p.key.hex().upper()} for p in pairs],
    }
    path.write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")


def dump_tables_json(tables: Tables, path: Path) -> None:
    obj = {
        "schema": "subuwutuner.ssmcan1.tables.v1",
        "ikb_hex": tables.ikb.hex().upper(),
        "kpt_hex": tables.kpt.hex().upper(),
    }
    path.write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")


def load_tables_json(path: Path) -> Tables:
    obj = json.loads(path.read_text(encoding="utf-8"))
    return Tables(ikb=bytes.fromhex(obj["ikb_hex"]), kpt=bytes.fromhex(obj["kpt_hex"]))


# ──────────────────────────────────────────────────────────────────────
# Verifier
# ──────────────────────────────────────────────────────────────────────

@dataclass
class VerifyReport:
    total: int
    passed: int
    failed_examples: list[tuple[bytes, bytes, bytes]] = field(default_factory=list)
    # (seed, expected_key, computed_key) for up to N failures

    @property
    def all_passed(self) -> bool:
        return self.failed == 0

    @property
    def failed(self) -> int:
        return self.total - self.passed


def verify_pairs(
    pairs: list[GeneratedPair], tables: Tables, max_failure_examples: int = 3
) -> VerifyReport:
    """Check that `tables` produce the observed key for every (seed, key) pair."""
    passed = 0
    failures: list[tuple[bytes, bytes, bytes]] = []
    for p in pairs:
        computed = ssmcan1_encrypt(p.seed, tables)
        if computed == p.key:
            passed += 1
        elif len(failures) < max_failure_examples:
            failures.append((p.seed, p.key, computed))
    return VerifyReport(total=len(pairs), passed=passed, failed_examples=failures)


# ──────────────────────────────────────────────────────────────────────
# Linearity diagnostic
# ──────────────────────────────────────────────────────────────────────

@dataclass
class LinearityResult:
    is_linear: bool
    table_only_constant: bytes               # encrypt(0, tables)
    samples: list[tuple[bytes, bytes, bytes, bool]] = field(default_factory=list)
    # (seed, full_encrypt, predicted_via_linearity, match)


def check_linearity(
    tables: Tables, num_tests: int = 16, rng_seed: int = 0
) -> LinearityResult:
    """Empirically test whether ssmcan1_encrypt is GF(2)-linear in
    (seed, tables): does `encrypt(s, T) == encrypt(s, 0) XOR encrypt(0, T)`
    hold for every seed?

    A LINEAR result is a structural defect for a seed/key algorithm — it
    means the 512-bit table space maps onto at most 32 bits of observable
    signal, so the solver can't recover unique tables from any number of
    (seed, key) pairs. A NONLINEAR result means table content is
    observable in the output, which is what the solver needs.

    See module docstring "EMPIRICAL FINDING" for context.
    """
    import random
    rng = random.Random(rng_seed)

    zero_tables = Tables(
        ikb=bytes(IKB_ENTRIES * IKB_ENTRY_BYTES),
        kpt=bytes(KPT_ENTRIES * KPT_ENTRY_BYTES),
    )
    constant = ssmcan1_encrypt(b"\x00" * SEED_BYTES, tables)

    samples: list[tuple[bytes, bytes, bytes, bool]] = []
    is_linear = True
    for _ in range(num_tests):
        s = bytes(rng.randint(0, 255) for _ in range(SEED_BYTES))
        full = ssmcan1_encrypt(s, tables)
        seed_only = ssmcan1_encrypt(s, zero_tables)
        predicted = bytes(a ^ b for a, b in zip(seed_only, constant))
        match = full == predicted
        if not match:
            is_linear = False
        samples.append((s, full, predicted, match))

    return LinearityResult(
        is_linear=is_linear, table_only_constant=constant, samples=samples
    )


# ──────────────────────────────────────────────────────────────────────
# Z3 solver (lazy import — only needed for `solve`)
# ──────────────────────────────────────────────────────────────────────

@dataclass
class SolveResult:
    tables: Optional[Tables]
    status: str                    # "unique", "ambiguous", "unsat", "z3-missing"
    notes: list[str] = field(default_factory=list)
    alternate_tables: Optional[Tables] = None  # set when status == "ambiguous"


def solve_with_z3(pairs: list[GeneratedPair], timeout_ms: int = 60_000) -> SolveResult:
    """Symbolically solve for (IndexKeyBase, KeyPartsTable) that satisfy
    every observed (seed, key) pair. Returns:
      * status='unique'    — single satisfying table-set
      * status='ambiguous' — >1 satisfying set; need more pairs
      * status='unsat'     — no table-set fits these pairs under the
                             encoded algorithm. The structure assumption
                             is wrong, or the capture is corrupt.
      * status='z3-missing'— z3-solver package not installed
    """
    try:
        import z3
    except ImportError:
        return SolveResult(
            tables=None,
            status="z3-missing",
            notes=[
                "z3-solver is not installed. Install with:",
                "    pip install z3-solver",
                "Then re-run this command.",
            ],
        )

    solver = z3.Solver()
    solver.set("timeout", timeout_ms)

    ikb_bv = [z3.BitVec(f"ikb_{i}", 16) for i in range(IKB_ENTRIES)]
    kpt_bv = [z3.BitVec(f"kpt_{i}", 8) for i in range(KPT_ENTRIES)]

    def encrypt_z3(seed_bv: "z3.BitVecRef") -> "z3.BitVecRef":
        state = seed_bv
        for r in range(NUM_ROUNDS):
            # state ^= IndexKeyBase[r] << 16  (high half)
            high_mix = z3.Concat(ikb_bv[r], z3.BitVecVal(0, 16))
            state = state ^ high_mix
            # state ^= (KeyPartsTable[r*2] << 8) | KeyPartsTable[r*2+1]  (low half)
            low_mix = z3.Concat(
                z3.BitVecVal(0, 16), kpt_bv[r * 2], kpt_bv[r * 2 + 1]
            )
            state = state ^ low_mix
            # state = ror32(state, 3)
            state = z3.RotateRight(state, 3)
        # top/bottom 16-bit swap
        high = z3.Extract(31, 16, state)
        low = z3.Extract(15, 0, state)
        return z3.Concat(low, high)

    for p in pairs:
        seed_v = int.from_bytes(p.seed, "big")
        key_v = int.from_bytes(p.key, "big")
        solver.add(encrypt_z3(z3.BitVecVal(seed_v, 32)) == z3.BitVecVal(key_v, 32))

    check = solver.check()
    if check == z3.unsat:
        return SolveResult(
            tables=None,
            status="unsat",
            notes=[
                "No table-set satisfies the captured pairs under the encoded",
                "algorithm structure. Most likely cause: the per-round operation",
                "order in `ssmcan1_encrypt` does not match real Subaru SSMCAN1.",
                "See module docstring 'ALGORITHM STRUCTURE' section.",
                "Second possibility: capture contains corrupt pairs (e.g., bytes",
                "from a different SA level or a different ECU). Try re-running",
                "extract_subaru_sa.py with a tighter --filter.",
            ],
        )
    if check != z3.sat:
        return SolveResult(
            tables=None,
            status=f"unknown ({check})",
            notes=["Solver returned unknown — likely timed out. Try --timeout."],
        )

    model = solver.model()
    ikb_bytes = b"".join(
        int(model.eval(ikb_bv[i], model_completion=True).as_long()).to_bytes(2, "big")
        for i in range(IKB_ENTRIES)
    )
    kpt_bytes = bytes(
        int(model.eval(kpt_bv[i], model_completion=True).as_long())
        for i in range(KPT_ENTRIES)
    )
    primary = Tables(ikb=ikb_bytes, kpt=kpt_bytes)

    # Ambiguity check: is there any OTHER table-set that satisfies the pairs?
    # model_completion=True is REQUIRED — if Z3 leaves a variable unassigned
    # (which is legal for under-determined systems), bare .eval() returns the
    # symbolic expression and .as_long() throws AttributeError.
    differ = z3.Or(
        [
            ikb_bv[i] != model.eval(ikb_bv[i], model_completion=True).as_long()
            for i in range(IKB_ENTRIES)
        ]
        + [
            kpt_bv[i] != model.eval(kpt_bv[i], model_completion=True).as_long()
            for i in range(KPT_ENTRIES)
        ]
    )
    solver.add(differ)
    second = solver.check()
    if second == z3.sat:
        m2 = solver.model()
        ikb2 = b"".join(
            int(m2.eval(ikb_bv[i], model_completion=True).as_long()).to_bytes(2, "big")
            for i in range(IKB_ENTRIES)
        )
        kpt2 = bytes(
            int(m2.eval(kpt_bv[i], model_completion=True).as_long())
            for i in range(KPT_ENTRIES)
        )
        alt = Tables(ikb=ikb2, kpt=kpt2)
        return SolveResult(
            tables=primary,
            status="ambiguous",
            alternate_tables=alt,
            notes=[
                f"Found at least 2 distinct table-sets that satisfy all {len(pairs)} pair(s).",
                "The system is under-determined. Capture more pairs (16+ recommended)",
                "and re-run. Both candidates are emitted; either may be correct, but",
                "you cannot tell from these pairs alone.",
            ],
        )

    return SolveResult(tables=primary, status="unique", notes=[])


# ──────────────────────────────────────────────────────────────────────
# C++ emitter
# ──────────────────────────────────────────────────────────────────────

CPP_TEMPLATE = """\
// SPDX-License-Identifier: <fork-author-choose>
//
// AUTO-GENERATED by tools/solve_ssmcan1.py — do NOT commit upstream.
// This file embeds Subaru SSMCAN1 key-derivation tables recovered
// from a (seed, key) capture. Upstream SubuwuTuner ships only stubs
// for license reasons; see docs/23-security-access.md.
//
// To use in a downstream fork:
//   #include "ssmcan1_real.hpp"
//   flasher.set_security_key_fn({fork_namespace}::ssmcan1_real);
//
// Pairs used in the solve: {num_pairs}
// Solver status: {solve_status}

#include "st/core/result.hpp"
#include "st/ecu/security_key.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {fork_namespace} {{

// IndexKeyBase[16] — 16 entries × 2 bytes
inline constexpr std::array<std::uint16_t, {ikb_count}> kSsmcan1IndexKeyBase = {{
{ikb_literals}
}};

// KeyPartsTable[32] — 32 entries × 1 byte
inline constexpr std::array<std::uint8_t, {kpt_count}> kSsmcan1KeyPartsTable = {{
{kpt_literals}
}};

[[nodiscard]] inline ::st::Result<std::vector<std::uint8_t>>
ssmcan1_real(std::span<std::uint8_t const> seed) {{
    if (seed.size() != {seed_bytes}) {{
        return ::st::failure(::st::ErrorCode::InvalidArgument,
                             "ssmcan1_real: seed must be {seed_bytes} bytes");
    }}

    std::uint32_t state = (static_cast<std::uint32_t>(seed[0]) << 24)
                        | (static_cast<std::uint32_t>(seed[1]) << 16)
                        | (static_cast<std::uint32_t>(seed[2]) << 8)
                        |  static_cast<std::uint32_t>(seed[3]);

    for (std::size_t r = 0; r < {num_rounds}; ++r) {{
        state ^= static_cast<std::uint32_t>(kSsmcan1IndexKeyBase[r]) << 16;
        state ^= (static_cast<std::uint32_t>(kSsmcan1KeyPartsTable[r * 2]) << 8)
               |  static_cast<std::uint32_t>(kSsmcan1KeyPartsTable[r * 2 + 1]);
        // ror32 by 3
        state = (state >> 3) | (state << 29);
    }}

    // top/bottom 16-bit swap
    std::uint32_t const high = (state >> 16) & 0xFFFFu;
    std::uint32_t const low = state & 0xFFFFu;
    state = (low << 16) | high;

    return std::vector<std::uint8_t>{{
        static_cast<std::uint8_t>((state >> 24) & 0xFFu),
        static_cast<std::uint8_t>((state >> 16) & 0xFFu),
        static_cast<std::uint8_t>((state >> 8)  & 0xFFu),
        static_cast<std::uint8_t>( state        & 0xFFu),
    }};
}}

}} // namespace {fork_namespace}
"""


def emit_cpp(
    tables: Tables,
    num_pairs: int,
    solve_status: str,
    fork_namespace: str = "yourfork",
) -> str:
    """Render a self-contained C++ source file embedding `tables` and the
    matching algorithm function. Drop into a downstream fork and wire via
    `Flasher::set_security_key_fn(...)`."""

    def chunk(seq: list[str], per_line: int) -> str:
        lines = []
        for i in range(0, len(seq), per_line):
            lines.append("    " + ", ".join(seq[i:i + per_line]) + ",")
        return "\n".join(lines)

    ikb_literals = chunk([f"0x{v:04X}" for v in tables.ikb_u16], per_line=8)
    kpt_literals = chunk([f"0x{v:02X}" for v in tables.kpt_u8], per_line=8)

    return CPP_TEMPLATE.format(
        fork_namespace=fork_namespace,
        num_pairs=num_pairs,
        solve_status=solve_status,
        ikb_count=IKB_ENTRIES,
        kpt_count=KPT_ENTRIES,
        seed_bytes=SEED_BYTES,
        num_rounds=NUM_ROUNDS,
        ikb_literals=ikb_literals,
        kpt_literals=kpt_literals,
    )


# ──────────────────────────────────────────────────────────────────────
# CLI subcommands
# ──────────────────────────────────────────────────────────────────────

def cmd_generate(args: argparse.Namespace) -> int:
    tables = generate_tables(rng_seed=args.rng_seed)
    pairs = generate_pairs(tables, args.pairs, rng_seed=args.rng_seed)
    dump_pairs_json(pairs, args.output)
    print(f"wrote {len(pairs)} synthetic pair(s) to {args.output}", file=sys.stderr)
    if args.tables_out:
        dump_tables_json(tables, args.tables_out)
        print(f"wrote ground-truth tables to {args.tables_out}", file=sys.stderr)
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    loaded = load_pairs(args.pairs)
    if not loaded.pairs:
        print("error: no pairs to verify", file=sys.stderr)
        return 1
    tables = load_tables_json(args.tables)
    report = verify_pairs(loaded.pairs, tables)
    print(f"{report.passed}/{report.total} pair(s) match", file=sys.stderr)
    if report.failed:
        print(f"first {len(report.failed_examples)} failure(s):", file=sys.stderr)
        for seed, expected, computed in report.failed_examples:
            print(
                f"  seed={seed.hex().upper()}  expected={expected.hex().upper()}  "
                f"computed={computed.hex().upper()}",
                file=sys.stderr,
            )
        return 1
    return 0


def cmd_solve(args: argparse.Namespace) -> int:
    loaded = load_pairs(args.pairs)
    if not loaded.pairs:
        print("error: no pairs to solve against", file=sys.stderr)
        return 1
    print(f"solving against {len(loaded.pairs)} pair(s)...", file=sys.stderr)
    result = solve_with_z3(loaded.pairs, timeout_ms=args.timeout * 1000)

    print(f"status: {result.status}", file=sys.stderr)
    for note in result.notes:
        print(f"  {note}", file=sys.stderr)

    if result.tables is None:
        return 2 if result.status in ("unsat", "z3-missing") else 1

    # Sanity-check: verify the recovered tables reproduce every pair.
    verify = verify_pairs(loaded.pairs, result.tables)
    if verify.failed:
        print(
            f"error: recovered tables failed self-verification "
            f"({verify.passed}/{verify.total}) — this is a solver bug",
            file=sys.stderr,
        )
        return 1
    print(f"self-verify: {verify.passed}/{verify.total} pair(s) reproduced", file=sys.stderr)

    cpp = emit_cpp(
        result.tables,
        num_pairs=len(loaded.pairs),
        solve_status=result.status,
        fork_namespace=args.namespace,
    )
    if args.output:
        args.output.write_text(cpp, encoding="utf-8")
        print(f"wrote C++ source to {args.output}", file=sys.stderr)
    else:
        print(cpp)

    if result.status == "ambiguous" and result.alternate_tables and args.output:
        alt_path = args.output.with_suffix(".alternate" + args.output.suffix)
        alt_cpp = emit_cpp(
            result.alternate_tables,
            num_pairs=len(loaded.pairs),
            solve_status="ambiguous-alternate",
            fork_namespace=args.namespace,
        )
        alt_path.write_text(alt_cpp, encoding="utf-8")
        print(f"wrote alternate (ambiguous) candidate to {alt_path}", file=sys.stderr)

    if args.report:
        print("\n# Tables (hex)", file=sys.stderr)
        print(f"IndexKeyBase: {result.tables.ikb.hex().upper()}", file=sys.stderr)
        print(f"KeyPartsTable: {result.tables.kpt.hex().upper()}", file=sys.stderr)

    return 0


def cmd_linearity_check(args: argparse.Namespace) -> int:
    """Diagnostic: is the encoded algorithm GF(2)-linear in (seed, tables)?
    See `check_linearity` and the module docstring's "EMPIRICAL FINDING"
    section for what the answer means."""
    tables = generate_tables(rng_seed=args.tables_seed)
    result = check_linearity(
        tables, num_tests=args.num_tests, rng_seed=args.seed_rng
    )

    print(
        f"encrypt(0, T) = {result.table_only_constant.hex().upper()}"
        f"  (table-only contribution)",
        file=sys.stderr,
    )
    print(
        "Linearity test: encrypt(s, T) == encrypt(s, 0) XOR encrypt(0, T) ?",
        file=sys.stderr,
    )
    print(file=sys.stderr)
    for seed, full, predicted, match in result.samples:
        marker = "OK" if match else "MISMATCH"
        print(
            f"  seed={seed.hex().upper()}  full={full.hex().upper()}  "
            f"pred={predicted.hex().upper()}  {marker}",
            file=sys.stderr,
        )
    print(file=sys.stderr)

    if result.is_linear:
        print("RESULT: LINEAR - algorithm is GF(2)-linear in (seed, tables).", file=sys.stderr)
        print(
            "        Table parameters have at most 32 bits of observable signal",
            file=sys.stderr,
        )
        print(
            "        regardless of how many (seed, key) pairs are captured.",
            file=sys.stderr,
        )
        print(
            "        `solve` will report 'ambiguous' on synthetic captures and",
            file=sys.stderr,
        )
        print(
            "        is likely to return UNSAT on real Subaru data.",
            file=sys.stderr,
        )
    else:
        print("RESULT: NONLINEAR - table content is observable in the output.", file=sys.stderr)
        print(
            "        `solve` can in principle recover unique tables given",
            file=sys.stderr,
        )
        print(
            "        enough pairs.",
            file=sys.stderr,
        )

    # Exit 0 either way — this is a diagnostic, not a pass/fail test.
    # Both outcomes are informative for the algorithm-refinement loop.
    return 0


# =============================================================================
# Gen-A SSMCAN1 — recovered Feistel + S-box (analyst-mode RE, 2026-05-24)
# =============================================================================
#
# Python mirror of the C++ implementation in src/ecu/src/subaru_security.cpp.
# Constants are byte-verified against all 8 A-series ROMs sampled (LF75 ..
# LF9L, model years 2015-2021). See docs/23-security-access.md § "Algorithm
# structure recovered (2026-05-24)" for the algorithm description and
# docs/17 §7 for the distribution-axis reasoning.
#
# Used by the `verify-gen-a` subcommand below to round-trip a captured
# (seed, key) pair against the recovered algorithm — for example, pairs
# extracted from a Y-cable sniff of COBB AccessPort authenticating to the
# user's car. If every captured pair matches what our algorithm computes,
# the algorithm + constants are correct for that car. If pairs mismatch,
# the captured keys are ground truth to derive whatever's different
# (e.g. COBB-modified round-key constants).

GEN_A_SBOX = bytes([
    0x05, 0x06, 0x07, 0x01, 0x09, 0x0c, 0x0d, 0x08,
    0x0a, 0x0d, 0x02, 0x0b, 0x0f, 0x04, 0x00, 0x03,
    0x0b, 0x04, 0x06, 0x00, 0x0f, 0x02, 0x0d, 0x09,
    0x05, 0x0c, 0x01, 0x0a, 0x03, 0x0d, 0x0e, 0x08,
])

GEN_A_RK_L1: list[int] = [
    0x78b1, 0x4625, 0x201c, 0x9ea5,
    0xad6b, 0x35f4, 0xfd21, 0x5e71,
    0xb046, 0x7f4a, 0x4b75, 0x93f9,
    0x1895, 0x8961, 0x3ecc, 0x862b,
]

# Level-3 + Level-5 share the same round-key table on Gen-A.2.
# Byte-verified across all LF7x/LF9x ROMs in
# fixtures/private/findings_algorithms/generation-A-seed-to-key.md §
# "Constants - level 3 / level 5".
GEN_A_RK_L35: list[int] = [
    0x794b, 0x3caf, 0x3019, 0x8b57,
    0x52a0, 0xa77c, 0x38c9, 0xb0b5,
    0x6520, 0x3b66, 0xa09d, 0x2877,
    0x479f, 0xb685, 0x7568, 0x84d7,
]


def _stock_rk_for_level(level: int) -> list[int]:
    if level == 1:
        return GEN_A_RK_L1
    if level in (3, 5):
        return GEN_A_RK_L35
    raise ValueError(f"unsupported SA level: {level}")


def _gen_a_rol16(v: int, n: int) -> int:
    n &= 15
    v &= 0xFFFF
    if n == 0:
        return v
    return ((v << n) | (v >> (16 - n))) & 0xFFFF


def gen_a_F(x: int, k: int) -> int:
    """Per-round F function: x XOR k, four overlapping 5-bit S-box lookups,
    16-bit rotate-left by 13. Bit 0 of x promotes to bit 4 of the
    high-nibble index (the asymmetric construction)."""
    x = (x ^ k) & 0xFFFF
    i3 = ((x & 0x0001) << 4) | (x >> 12)
    i2 = (x >> 8) & 0x1F
    i1 = (x >> 4) & 0x1F
    i0 = x & 0x1F
    y = ((GEN_A_SBOX[i3] << 12) | (GEN_A_SBOX[i2] << 8)
         | (GEN_A_SBOX[i1] << 4) | GEN_A_SBOX[i0])
    return _gen_a_rol16(y, 13)


def gen_a_feistel_forward(state: int, rk: list[int]) -> int:
    """ECU's direction: internal_key -> post-rounds state."""
    state &= 0xFFFFFFFF
    for k in rk:
        L = state & 0xFFFF
        H = (state >> 16) & 0xFFFF
        new_L = (H ^ gen_a_F(L, k)) & 0xFFFF
        new_H = L
        state = ((new_H << 16) | new_L) & 0xFFFFFFFF
    return state


def gen_a_feistel_inverse(state: int, rk: list[int]) -> int:
    """Tester's direction: post-rounds state -> internal_key."""
    state &= 0xFFFFFFFF
    for k in reversed(rk):
        L_new = state & 0xFFFF
        H_new = (state >> 16) & 0xFFFF
        L_old = H_new
        H_old = (L_new ^ gen_a_F(L_old, k)) & 0xFFFF
        state = ((H_old << 16) | L_old) & 0xFFFFFFFF
    return state


def _gen_a_wordswap32(v: int) -> int:
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


def gen_a_seed_to_key_l1(seed_bytes: bytes) -> bytes:
    """Compute the L1 (bootloader-unlock) key from the 4 wire seed bytes.

    `seed_bytes` is exactly the 4 bytes at positions [2..5] of the
    `67 01 ...` requestSeed response. Returns the 4 bytes to place at
    positions [2..5] of the `27 02 ...` sendKey request.
    """
    return gen_a_seed_to_key(seed_bytes, level=1)


# -----------------------------------------------------------------------------
# Per-level byte shuffles (analyst-side reference C, fixtures/private/
# findings_algorithms/ + ../reference-implementations/seed_to_key_gen_a.c).
# L1 is the identity case. L3/L5 reorder bytes on both the seed-in and
# key-out paths so a tester can't naively cross-replay between levels.
# -----------------------------------------------------------------------------

def _l1_wire_seed_to_packed(wire: bytes) -> int:
    return int.from_bytes(wire, "big")


def _l1_state_to_wire_key(state: int) -> bytes:
    return state.to_bytes(4, "big")


def _l3_wire_seed_to_packed(wire: bytes) -> int:
    # The analyst-side spec / reference C describes L3 as a 3-byte seed:
    # s1=wire[0], s2=wire[1], s0=wire[2], s3 forced to 0. That makes the
    # inverse lossy and the synthetic round-trip impossible for most
    # internal_keys (the forward direction's low-byte-of-seed_packed
    # would have to happen to be 0 for round-trip to hold).
    #
    # Empirically on a 2017 LF79103P, the ECU's `67 03 ...` response
    # carries a 4-byte payload after SID/sub (PCI=06 in observed
    # captures), and wire[3] is nonzero. We treat ALL FOUR wire bytes
    # as seed material with the spec's same per-byte permutation
    # (s1, s2, s0, then wire[3] as s3 rather than forced 0). This
    # preserves round-trip on synthetic pairs and matches the actually-
    # observed Gen-A.2 wire shape. If a future capture from an older
    # Gen-A 1MB ECU shows s3=0 in wire[3], swap this back to forcing 0.
    s1, s2, s0, s3 = wire[0], wire[1], wire[2], wire[3]
    return (s0 << 24) | (s1 << 16) | (s2 << 8) | s3


def _l3_state_to_wire_key(state: int) -> bytes:
    # Reference C: wire[0]=k3, wire[1]=k1, wire[2]=k2, wire[3]=k0 where
    # k0..k3 are state bytes MSB->LSB.
    k0 = (state >> 24) & 0xFF
    k1 = (state >> 16) & 0xFF
    k2 = (state >> 8) & 0xFF
    k3 = state & 0xFF
    return bytes([k3, k1, k2, k0])


def _l5_wire_seed_to_packed(wire: bytes) -> int:
    # Reference C: s3=wire[0], s1=wire[1], s0=wire[2], s2=wire[3].
    s3, s1, s0, s2 = wire[0], wire[1], wire[2], wire[3]
    return (s0 << 24) | (s1 << 16) | (s2 << 8) | s3


def _l5_state_to_wire_key(state: int) -> bytes:
    # Reference C: wire[0]=k1, wire[1]=k3, wire[2]=k0, wire[3]=k2.
    k0 = (state >> 24) & 0xFF
    k1 = (state >> 16) & 0xFF
    k2 = (state >> 8) & 0xFF
    k3 = state & 0xFF
    return bytes([k1, k3, k0, k2])


def _shuffles_for_level(level: int) -> "tuple[Callable[[bytes], int], Callable[[int], bytes]]":
    if level == 1:
        return _l1_wire_seed_to_packed, _l1_state_to_wire_key
    if level == 3:
        return _l3_wire_seed_to_packed, _l3_state_to_wire_key
    if level == 5:
        return _l5_wire_seed_to_packed, _l5_state_to_wire_key
    raise ValueError(f"unsupported SA level: {level}")


def gen_a_seed_to_key(seed_bytes: bytes, level: int) -> bytes:
    """Compute the wire-key bytes from the wire-seed bytes for any
    supported SA level (1 / 3 / 5). All levels share the Feistel +
    S-box primitive and the wordswap step — they differ only in the
    per-level byte shuffle (in/out) and in the round-key table
    selection (L1 uses RK_L1; L3 and L5 share RK_L35).

    `seed_bytes` is the 4 bytes at positions [2..5] of the `67 LL`
    requestSeed response. Returns the 4 bytes to place at
    positions [2..5] of the `27 LL+1` sendKey request.
    """
    if len(seed_bytes) != 4:
        raise ValueError(
            f"Gen-A L{level} seed must be 4 bytes, got {len(seed_bytes)}")
    rk = _stock_rk_for_level(level)
    seed_to_packed, state_to_wire = _shuffles_for_level(level)

    seed_packed = seed_to_packed(seed_bytes)
    state_post_rounds = _gen_a_wordswap32(seed_packed)
    internal_key = gen_a_feistel_inverse(state_post_rounds, rk)
    return state_to_wire(internal_key)


def gen_a_key_to_seed(key_bytes: bytes, level: int) -> bytes:
    """Inverse of gen_a_seed_to_key — the ECU-side direction. Useful
    for synthesizing test pairs at any level.
    """
    if len(key_bytes) != 4:
        raise ValueError(
            f"Gen-A L{level} key must be 4 bytes, got {len(key_bytes)}")
    rk = _stock_rk_for_level(level)
    seed_to_packed, state_to_wire = _shuffles_for_level(level)
    # Run the per-level key-out shuffle in reverse to recover the
    # internal state bytes from the wire key.
    if level == 1:
        internal_key = int.from_bytes(key_bytes, "big")
    elif level == 3:
        # wire[0]=k3 wire[1]=k1 wire[2]=k2 wire[3]=k0 -> state bytes (k0,k1,k2,k3)
        k3, k1, k2, k0 = key_bytes[0], key_bytes[1], key_bytes[2], key_bytes[3]
        internal_key = (k0 << 24) | (k1 << 16) | (k2 << 8) | k3
    elif level == 5:
        # wire[0]=k1 wire[1]=k3 wire[2]=k0 wire[3]=k2
        k1, k3, k0, k2 = key_bytes[0], key_bytes[1], key_bytes[2], key_bytes[3]
        internal_key = (k0 << 24) | (k1 << 16) | (k2 << 8) | k3
    else:
        raise ValueError(f"unsupported SA level: {level}")
    post_rounds = gen_a_feistel_forward(internal_key, rk)
    seed_packed = _gen_a_wordswap32(post_rounds)
    # Run per-level seed-in shuffle in reverse to recover wire bytes.
    if level == 1:
        return seed_packed.to_bytes(4, "big")
    if level == 3:
        s0 = (seed_packed >> 24) & 0xFF
        s1 = (seed_packed >> 16) & 0xFF
        s2 = (seed_packed >> 8) & 0xFF
        s3 = seed_packed & 0xFF
        # Per the analyst-side spec L3 wire is "(s1, s2, s0)" with wire[3]
        # forced to 0 — but the observed LF79103P wire carries a real s3
        # byte at wire[3]. We emit the actual s3 here to keep symmetry
        # with _l3_wire_seed_to_packed above and synthetic round-trip.
        return bytes([s1, s2, s0, s3])
    if level == 5:
        s0 = (seed_packed >> 24) & 0xFF
        s1 = (seed_packed >> 16) & 0xFF
        s2 = (seed_packed >> 8) & 0xFF
        s3 = seed_packed & 0xFF
        return bytes([s3, s1, s0, s2])
    raise ValueError(f"unsupported SA level: {level}")  # unreachable


def gen_a_key_to_seed_l1(key_bytes: bytes) -> bytes:
    """Inverse of gen_a_seed_to_key_l1 — the ECU-side direction.

    Given a 4-byte internal_key, returns what the ECU would have emitted
    on the wire as the L1 seed. Used by tests and by the verify-gen-a
    summary when the captured key needs to be checked against a
    forward-computed seed too.
    """
    if len(key_bytes) != 4:
        raise ValueError(
            f"Gen-A L1 key must be 4 bytes, got {len(key_bytes)}")
    internal_key = int.from_bytes(key_bytes, "big")
    post_rounds = gen_a_feistel_forward(internal_key, GEN_A_RK_L1)
    seed_packed = _gen_a_wordswap32(post_rounds)
    return seed_packed.to_bytes(4, "big")


def cmd_verify_gen_a(args: argparse.Namespace) -> int:
    """Round-trip a captured pairs.json against the recovered Gen-A
    algorithm at every captured level (1/3/5). Auto-dispatches per pair
    using the `level` field in the JSON; defaults to L1 for older
    captures without the field.

    Used as a one-shot sanity check before burning real-car attempts:
    if every captured pair matches, the algorithm + stock constants are
    correct for that level on this car; if every captured pair
    mismatches, the level's constants have been altered by an
    aftermarket tune (use recover-gen-a to derive the replacement)."""
    try:
        text = args.pairs.read_text(encoding="utf-8")
        data = json.loads(text)
    except OSError as e:
        print(f"verify-gen-a: cannot read {args.pairs}: {e}", file=sys.stderr)
        return 1
    except json.JSONDecodeError as e:
        print(f"verify-gen-a: invalid JSON in {args.pairs}: {e}", file=sys.stderr)
        return 1

    pairs = data.get("pairs") if isinstance(data, dict) else None
    if not pairs:
        print(f"verify-gen-a: no pairs in {args.pairs}", file=sys.stderr)
        return 1

    # Per-level tallies for the per-level result line.
    matches_by_level: dict[int, int] = {}
    mismatches_by_level: dict[int, int] = {}
    malformed = 0
    print(
        f"Verifying {len(pairs)} captured pair(s) against Gen-A SSMCAN1 "
        f"(L1/L3/L5)",
        file=sys.stderr,
    )
    print(file=sys.stderr)

    for i, p in enumerate(pairs):
        try:
            seed = bytes.fromhex(p["seed"])
            captured_key = bytes.fromhex(p["key"])
        except (KeyError, TypeError, ValueError) as e:
            print(f"  [{i:2d}] malformed pair: {e}", file=sys.stderr)
            malformed += 1
            continue
        if len(seed) != 4 or len(captured_key) != 4:
            print(
                f"  [{i:2d}] wrong byte count: seed={len(seed)} key={len(captured_key)} "
                f"(both must be 4)",
                file=sys.stderr,
            )
            malformed += 1
            continue
        level = int(p.get("level", 1))
        if level not in (1, 3, 5):
            print(f"  [{i:2d}] unsupported level {level} (must be 1/3/5)",
                  file=sys.stderr)
            malformed += 1
            continue
        try:
            computed_key = gen_a_seed_to_key(seed, level=level)
        except ValueError as e:
            print(f"  [{i:2d}] algorithm error: {e}", file=sys.stderr)
            malformed += 1
            continue
        match = computed_key == captured_key
        marker = "MATCH" if match else "MISMATCH"
        print(
            f"  [{i:2d}] L{level}  seed={seed.hex().upper()}  "
            f"captured={captured_key.hex().upper()}  "
            f"computed={computed_key.hex().upper()}  {marker}",
            file=sys.stderr,
        )
        if match:
            matches_by_level[level] = matches_by_level.get(level, 0) + 1
        else:
            mismatches_by_level[level] = mismatches_by_level.get(level, 0) + 1

    print(file=sys.stderr)
    levels_seen = sorted(set(matches_by_level) | set(mismatches_by_level))
    total = sum(matches_by_level.values()) + sum(mismatches_by_level.values()) + malformed
    matches = sum(matches_by_level.values())
    mismatches = sum(mismatches_by_level.values())
    print(
        f"Summary: {matches}/{total} match, {mismatches}/{total} mismatch, "
        f"{malformed}/{total} malformed.",
        file=sys.stderr,
    )
    for level in levels_seen:
        m = matches_by_level.get(level, 0)
        mm = mismatches_by_level.get(level, 0)
        verdict = (
            "STOCK" if m > 0 and mm == 0
            else "ALTERED" if m == 0 and mm > 0
            else "MIXED"
        )
        print(f"  L{level}: {m}/{m+mm} match  -> {verdict}", file=sys.stderr)

    # Exit code rolls up across all levels: 0 if every pair matched (all
    # levels stock), 2 if every pair mismatched (all levels altered), 3
    # if mixed (some levels stock, some altered, or noise).
    if matches == total and total > 0:
        print(
            "RESULT: ALGORITHM CORRECT at every captured level - stock constants verified.",
            file=sys.stderr,
        )
        return 0
    if matches == 0 and mismatches > 0:
        print(
            "RESULT: ALGORITHM MISMATCH at every captured level - constants altered. "
            "Run recover-gen-a per level to derive the replacement tables.",
            file=sys.stderr,
        )
        return 2
    print(
        f"RESULT: MIXED - some levels match stock, others don't. "
        "Inspect the per-level summary above.",
        file=sys.stderr,
    )
    return 3


# =============================================================================
# Gen-A SSMCAN1 aftermarket-table recovery
# =============================================================================
#
# When verify-gen-a reports MISMATCH (every captured pair fails round-trip
# against stock RK_L1), the algorithm is correct but the ECU's installed
# round-key table has been replaced by an aftermarket tuning install. The
# recovery search per docs/25-ish + the analyst-side
# fixtures/private/findings_algorithms/sa-aftermarket-alteration.md §3
# tries candidate replacement tables in priority order:
#
#   1. Known-list   — vendor-specific tables we've previously recovered.
#                     Currently empty; populated as recoveries land.
#   2. XOR-mask     — RK[i] = stock_RK[i] XOR C for a 16-bit constant C.
#                     65,536 candidates. The most common alteration shape.
#   3. Single-sub   — exactly one of the 16 table entries replaced with
#                     an arbitrary 16-bit value. 16 × 65,536 ≈ 1M cands.
#
# Each candidate is verified against EVERY captured pair. N pairs give
# 32·N bits of constraint on a 256-bit unknown; for N=4 the false-
# positive rate is 2^-128 (statistically zero). The script enforces
# N ≥ 2 minimum and recommends N ≥ 4.
#
# Shapes 4 (byte-swap permutations) and 5 (arbitrary 256-bit) are NOT
# attempted by default — too large or no usable structure. Add to the
# generator only if recoveries against shapes 1-3 keep failing on a
# given pack.

# Known-list of previously recovered aftermarket tables. Populated as
# vendor recoveries land; each entry is a 16-tuple matching the layout
# of GEN_A_RK_L1. Provenance + vendor-attribution kept in comments per
# the docs/15 clean-room posture (recorded as facts, not as expression).
GEN_A_RK_L1_KNOWN_AFTERMARKET: list[tuple[str, list[int]]] = [
    # Format: ("provenance label", [u16; 16])
    # ex: ("VendorX OTS v3.2 — recovered 2026-06-15 from car-A capture", [0x1234, ...])
]


def gen_a_verify_table_against_pairs(rk: list[int],
                                     pairs: list[tuple[bytes, bytes]],
                                     level: int = 1) -> bool:
    """Return True iff `rk` (16 × u16) round-trips EVERY (seed, key) pair.

    All pairs must be at the same SA level (the caller filters by level
    before invoking). Wire->packed and state->wire shuffles are per-level
    per docs/15 + the analyst-side spec.
    """
    seed_to_packed, state_to_wire = _shuffles_for_level(level)
    for seed_bytes, expected_key_bytes in pairs:
        seed_packed = seed_to_packed(seed_bytes)
        state_post_rounds = _gen_a_wordswap32(seed_packed)
        internal_key = gen_a_feistel_inverse(state_post_rounds, rk)
        derived_key_bytes = state_to_wire(internal_key)
        if derived_key_bytes != expected_key_bytes:
            return False
    return True


def gen_a_recovery_candidates(
    stock_rk: list[int],
) -> "Iterable[tuple[str, list[int]]]":
    """Yield (label, rk) candidates in priority order for the recovery
    search. Caller terminates on the first one that verifies all pairs.
    Uses generators throughout — the 1M+ single-sub candidates are
    streamed, not materialized."""
    # Shape 1: known list.
    for label, rk in GEN_A_RK_L1_KNOWN_AFTERMARKET:
        yield f"known-list: {label}", list(rk)

    # Shape 2: XOR-mask. 65,536 candidates. ~seconds in Python.
    for c in range(1, 0x10000):  # c=0 is the stock table; skip
        yield f"xor-mask: stock ^ 0x{c:04x}", [s ^ c for s in stock_rk]

    # Shape 3: single-position substitution. 16 × 65,536 = ~1M
    # candidates. ~tens of seconds to a minute in Python; if this
    # becomes the routine path it's straightforward to vectorize.
    for pos in range(16):
        for val in range(0x10000):
            if val == stock_rk[pos]:  # skip the stock entry
                continue
            sub = list(stock_rk)
            sub[pos] = val
            yield f"single-sub: pos={pos} val=0x{val:04x}", sub


def cmd_recover_gen_a(args: argparse.Namespace) -> int:
    """Recover the modified round-key table for one SA level from captured
    (seed, key) pairs. Run per-level when verify-gen-a reports ALTERED at
    that level. Defaults to --level 1; pass --level 3 or --level 5 to
    recover a non-L1 table (L3 and L5 both use RK_L35 as the stock base).

    Pairs in the JSON are filtered by `level` field: only pairs at the
    requested level participate in the search. Older pairs.json files
    without a `level` field default to L1.
    """
    try:
        text = args.pairs.read_text(encoding="utf-8")
        data = json.loads(text)
    except OSError as e:
        print(f"recover-gen-a: cannot read {args.pairs}: {e}", file=sys.stderr)
        return 1
    except json.JSONDecodeError as e:
        print(f"recover-gen-a: invalid JSON in {args.pairs}: {e}",
              file=sys.stderr)
        return 1

    raw_pairs = data.get("pairs") if isinstance(data, dict) else None
    if not raw_pairs:
        print(f"recover-gen-a: no pairs in {args.pairs}", file=sys.stderr)
        return 1

    target_level = int(args.level)
    if target_level not in (1, 3, 5):
        print(f"recover-gen-a: --level must be 1, 3, or 5 (got {target_level})",
              file=sys.stderr)
        return 1
    try:
        stock_rk = _stock_rk_for_level(target_level)
    except ValueError as e:
        print(f"recover-gen-a: {e}", file=sys.stderr)
        return 1

    pairs: list[tuple[bytes, bytes]] = []
    skipped_wrong_level = 0
    for i, p in enumerate(raw_pairs):
        pair_level = int(p.get("level", 1))
        if pair_level != target_level:
            skipped_wrong_level += 1
            continue
        try:
            seed = bytes.fromhex(p["seed"])
            key = bytes.fromhex(p["key"])
        except (KeyError, TypeError, ValueError):
            print(f"  [{i}] malformed pair, skipping", file=sys.stderr)
            continue
        if len(seed) != 4 or len(key) != 4:
            print(f"  [{i}] wrong byte count, skipping", file=sys.stderr)
            continue
        pairs.append((seed, key))

    if skipped_wrong_level > 0:
        print(
            f"  (skipped {skipped_wrong_level} pair(s) at other levels)",
            file=sys.stderr,
        )

    if len(pairs) < args.min_pairs:
        print(
            f"recover-gen-a: need at least {args.min_pairs} valid pairs at "
            f"level {target_level}, got {len(pairs)}. Capture more L{target_level} "
            f"SA exchanges and retry.",
            file=sys.stderr,
        )
        return 1

    print(
        f"Loaded {len(pairs)} valid L{target_level} (seed, key) pair(s).",
        file=sys.stderr,
    )

    # Sanity check: are the pairs consistent with stock? If so, the
    # algorithm isn't altered and recovery is unnecessary.
    if gen_a_verify_table_against_pairs(stock_rk, pairs, level=target_level):
        rk_name = "RK_L1" if target_level == 1 else "RK_L35"
        print(
            f"RESULT: ALGORITHM CORRECT - every pair verifies against stock "
            f"{rk_name}. No table replacement detected; nothing to recover.",
            file=sys.stderr,
        )
        return 0

    print(
        f"Stock RK fails on at least one pair - searching for replacement.",
        file=sys.stderr,
    )
    print(file=sys.stderr)

    # Search loop with periodic progress to stderr.
    import time
    start = time.time()
    last_progress = start
    examined = 0
    for label, candidate in gen_a_recovery_candidates(stock_rk):
        examined += 1
        if gen_a_verify_table_against_pairs(candidate, pairs, level=target_level):
            elapsed = time.time() - start
            print(
                f"RESULT: RECOVERED L{target_level} after examining {examined:,} "
                f"candidate(s) in {elapsed:.1f}s.",
                file=sys.stderr,
            )
            print(f"  shape: {label}", file=sys.stderr)
            rk_name = "rk_l1" if target_level == 1 else "rk_l35"
            print(f"  {rk_name} (16 x u16, big-endian on the wire):",
                  file=sys.stderr)
            for i in range(0, 16, 4):
                row = " ".join(f"0x{v:04x}" for v in candidate[i:i + 4])
                print(f"    [{i:>2}-{i+3:>2}] {row}", file=sys.stderr)
            # Emit a JSON sidecar the user can re-load / share.
            if args.output is not None:
                payload = {
                    "schema": "subuwutuner.sa_recovery.v1",
                    "algorithm": "gen_a_ssmcan1",
                    "level": target_level,
                    "shape": label,
                    rk_name: [f"0x{v:04x}" for v in candidate],
                    "verified_against_pairs": len(pairs),
                    "examined_candidates": examined,
                    "elapsed_seconds": round(elapsed, 2),
                }
                args.output.write_text(json.dumps(payload, indent=2) + "\n",
                                       encoding="utf-8")
                print(f"  wrote {args.output}", file=sys.stderr)
            return 0
        now = time.time()
        if now - last_progress > 5.0:
            print(f"  ...examined {examined:,} candidates "
                  f"({now - start:.0f}s elapsed)",
                  file=sys.stderr)
            last_progress = now

    elapsed = time.time() - start
    print(
        f"RESULT: NO RECOVERY after examining {examined:,} candidates "
        f"in {elapsed:.1f}s.",
        file=sys.stderr,
    )
    print(
        "  Tried known-list + XOR-mask (65k) + single-sub (1M). The vendor "
        "may have applied a deeper transformation (shape 4/5) or modified "
        "the S-box (shape D). Capture more pairs (4+ recommended) and "
        "consider a wider search.",
        file=sys.stderr,
    )
    return 2


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )
    sub = p.add_subparsers(dest="subcommand", required=True)

    p_gen = sub.add_parser(
        "generate",
        help="produce synthetic pairs.json + ground-truth tables for harness testing",
    )
    p_gen.add_argument("--pairs", type=int, default=32, help="how many pairs to generate")
    p_gen.add_argument("--output", "-o", type=Path, required=True, help="pairs.json output")
    p_gen.add_argument(
        "--tables-out", type=Path, default=None,
        help="ground-truth tables JSON (for verify); omit to leave unwritten",
    )
    p_gen.add_argument(
        "--rng-seed", type=int, default=None,
        help="seed RNG for reproducibility; default uses os.urandom",
    )
    p_gen.set_defaults(func=cmd_generate)

    p_ver = sub.add_parser(
        "verify",
        help="check that supplied tables reproduce every pair (sanity / round-trip)",
    )
    p_ver.add_argument("pairs", type=Path, help="pairs.json (from extract_subaru_sa.py or generate)")
    p_ver.add_argument("--tables", type=Path, required=True, help="tables JSON to verify against")
    p_ver.set_defaults(func=cmd_verify)

    p_solve = sub.add_parser(
        "solve",
        help="recover tables from pairs.json (requires z3-solver)",
    )
    p_solve.add_argument("pairs", type=Path, help="pairs.json (from extract_subaru_sa.py)")
    p_solve.add_argument(
        "--output", "-o", type=Path, default=None,
        help="emit recovered tables as C++ source; default stdout",
    )
    p_solve.add_argument(
        "--namespace", type=str, default="yourfork",
        help="C++ namespace for the emitted function (default 'yourfork')",
    )
    p_solve.add_argument(
        "--timeout", type=int, default=60,
        help="z3 solve timeout in seconds (default 60)",
    )
    p_solve.add_argument(
        "--report", action="store_true",
        help="print recovered table hex to stderr",
    )
    p_solve.set_defaults(func=cmd_solve)

    p_lin = sub.add_parser(
        "linearity-check",
        help="diagnostic: is the encoded algorithm GF(2)-linear in (seed, tables)?",
    )
    p_lin.add_argument(
        "--num-tests", type=int, default=16,
        help="number of random test seeds to evaluate (default 16)",
    )
    p_lin.add_argument(
        "--tables-seed", type=int, default=42,
        help="RNG seed for the test tables (default 42, reproducible)",
    )
    p_lin.add_argument(
        "--seed-rng", type=int, default=0,
        help="RNG seed for the test seeds (default 0, reproducible)",
    )
    p_lin.set_defaults(func=cmd_linearity_check)

    p_vga = sub.add_parser(
        "verify-gen-a",
        help=(
            "verify a captured pairs.json against the recovered Gen-A L1 "
            "algorithm (analyst-mode RE, 2026-05-24)"
        ),
    )
    p_vga.add_argument(
        "pairs",
        type=Path,
        help="pairs.json (from extract_subaru_sa.py)",
    )
    p_vga.set_defaults(func=cmd_verify_gen_a)

    p_rec = sub.add_parser(
        "recover-gen-a",
        help=(
            "search for the replacement round-key table when verify-gen-a "
            "reports MISMATCH (aftermarket-locked ECU). One level per run."
        ),
    )
    p_rec.add_argument(
        "pairs",
        type=Path,
        help="pairs.json (from extract_subaru_sa.py); 4+ pairs recommended",
    )
    p_rec.add_argument(
        "--level",
        type=int,
        default=1,
        choices=(1, 3, 5),
        help="SA level to recover (default 1). L3 and L5 both use RK_L35 "
             "as the stock base but apply different per-level byte shuffles; "
             "the pairs JSON's level field is used to filter input pairs.",
    )
    p_rec.add_argument(
        "--min-pairs",
        type=int,
        default=2,
        help="minimum (seed, key) pairs required for the search (default 2; "
             "4 gives a 2^-128 false-positive rate)",
    )
    p_rec.add_argument(
        "--output",
        "-o",
        type=Path,
        default=None,
        help="write a subuwutuner.sa_recovery.v1 JSON sidecar with the "
             "recovered table (paste back to the maintainer / load into the "
             "Flasher's set_security_key_fn)",
    )
    p_rec.set_defaults(func=cmd_recover_gen_a)

    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
