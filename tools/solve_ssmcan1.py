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

Z3 is required for the actual solve (`pip install z3-solver`). The
`generate` and `verify` subcommands work without z3.
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

    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
