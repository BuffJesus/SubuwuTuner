# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Round-trip tests for tools/solve_ssmcan1.py.

Run with:
    python -m unittest discover -s tools/tests

The verify-path tests run without external dependencies. The Z3 tests
skip themselves cleanly when z3-solver is not installed.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

# Import the script as a module by file path (tools/ isn't a package).
# Register in sys.modules first so dataclasses can resolve cls.__module__
# during class construction (otherwise the @dataclass decorator crashes).
_THIS = Path(__file__).resolve()
_SOLVER_PATH = _THIS.parent.parent / "solve_ssmcan1.py"
_spec = importlib.util.spec_from_file_location("solve_ssmcan1", _SOLVER_PATH)
assert _spec is not None and _spec.loader is not None
solve_ssmcan1 = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = solve_ssmcan1
_spec.loader.exec_module(solve_ssmcan1)


def _z3_available() -> bool:
    return importlib.util.find_spec("z3") is not None


class TestAlgorithm(unittest.TestCase):
    def test_encrypt_is_deterministic(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=42)
        seed = b"\xDE\xAD\xBE\xEF"
        k1 = solve_ssmcan1.ssmcan1_encrypt(seed, tables)
        k2 = solve_ssmcan1.ssmcan1_encrypt(seed, tables)
        self.assertEqual(k1, k2)
        self.assertEqual(len(k1), solve_ssmcan1.KEY_BYTES)

    def test_encrypt_different_seeds_diverge(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=42)
        # Very low collision probability — if this fails, the algorithm
        # is collapsing its input space, which would break uniqueness.
        keys = {
            solve_ssmcan1.ssmcan1_encrypt(s.to_bytes(4, "big"), tables)
            for s in range(256)
        }
        self.assertEqual(len(keys), 256, "two seeds collapsed to the same key")

    def test_encrypt_rejects_wrong_seed_length(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=1)
        with self.assertRaises(ValueError):
            solve_ssmcan1.ssmcan1_encrypt(b"\x00\x00\x00", tables)
        with self.assertRaises(ValueError):
            solve_ssmcan1.ssmcan1_encrypt(b"\x00\x00\x00\x00\x00", tables)


class TestGenerator(unittest.TestCase):
    def test_seeded_tables_are_reproducible(self) -> None:
        a = solve_ssmcan1.generate_tables(rng_seed=12345)
        b = solve_ssmcan1.generate_tables(rng_seed=12345)
        self.assertEqual(a.ikb, b.ikb)
        self.assertEqual(a.kpt, b.kpt)

    def test_seeded_pairs_are_reproducible(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=1)
        p1 = solve_ssmcan1.generate_pairs(tables, num_pairs=8, rng_seed=99)
        p2 = solve_ssmcan1.generate_pairs(tables, num_pairs=8, rng_seed=99)
        self.assertEqual([(p.seed, p.key) for p in p1], [(p.seed, p.key) for p in p2])

    def test_table_sizes(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=7)
        self.assertEqual(len(tables.ikb), 32)
        self.assertEqual(len(tables.kpt), 32)
        self.assertEqual(len(tables.ikb_u16), 16)
        self.assertEqual(len(tables.kpt_u8), 32)


class TestVerifier(unittest.TestCase):
    def test_correct_tables_pass(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=2)
        pairs = solve_ssmcan1.generate_pairs(tables, num_pairs=16, rng_seed=3)
        report = solve_ssmcan1.verify_pairs(pairs, tables)
        self.assertEqual(report.passed, 16)
        self.assertEqual(report.failed, 0)
        self.assertTrue(report.all_passed)

    def test_wrong_tables_fail(self) -> None:
        tables_a = solve_ssmcan1.generate_tables(rng_seed=10)
        tables_b = solve_ssmcan1.generate_tables(rng_seed=11)
        pairs = solve_ssmcan1.generate_pairs(tables_a, num_pairs=8, rng_seed=12)
        report = solve_ssmcan1.verify_pairs(pairs, tables_b)
        # Vanishingly unlikely that two random table-sets happen to
        # agree on any of 8 random seeds.
        self.assertEqual(report.passed, 0)
        self.assertEqual(report.failed, 8)
        self.assertFalse(report.all_passed)
        self.assertTrue(len(report.failed_examples) > 0)


class TestPairsJson(unittest.TestCase):
    def test_dump_then_load_roundtrip(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=5)
        pairs = solve_ssmcan1.generate_pairs(tables, num_pairs=4, rng_seed=6)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "pairs.json"
            solve_ssmcan1.dump_pairs_json(pairs, p)
            loaded = solve_ssmcan1.load_pairs(p)
        self.assertEqual(loaded.schema, "subuwutuner.sa.synthetic.v1")
        self.assertEqual(
            [(x.seed, x.key) for x in loaded.pairs],
            [(x.seed, x.key) for x in pairs],
        )

    def test_load_tolerates_extractor_schema(self) -> None:
        # Matches what tools/extract_subaru_sa.py emits (with extra timing
        # fields we don't care about here).
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "pairs.json"
            p.write_text(
                json.dumps({
                    "schema": "subuwutuner.sa.v1",
                    "request_id": "0x7E0",
                    "response_id": "0x7E8",
                    "pairs": [
                        {
                            "seed": "DEADBEEF",
                            "key": "12345678",
                            "request_seed_ms": 100,
                            "seed_response_ms": 110,
                            "send_key_ms": 120,
                            "key_ack_ms": 130,
                        },
                    ],
                }),
                encoding="utf-8",
            )
            loaded = solve_ssmcan1.load_pairs(p)
        self.assertEqual(len(loaded.pairs), 1)
        self.assertEqual(loaded.pairs[0].seed, b"\xDE\xAD\xBE\xEF")
        self.assertEqual(loaded.pairs[0].key, b"\x12\x34\x56\x78")


class TestEmitter(unittest.TestCase):
    def test_emitted_cpp_has_expected_structure(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=20)
        cpp = solve_ssmcan1.emit_cpp(
            tables, num_pairs=16, solve_status="unique", fork_namespace="myfork"
        )
        # Header / provenance
        self.assertIn("AUTO-GENERATED", cpp)
        self.assertIn("docs/23-security-access.md", cpp)
        # Namespace customisation
        self.assertIn("namespace myfork", cpp)
        # Table literals — verify by re-parsing a couple of entries
        self.assertIn("kSsmcan1IndexKeyBase", cpp)
        self.assertIn("kSsmcan1KeyPartsTable", cpp)
        self.assertIn(f"0x{tables.ikb_u16[0]:04X}", cpp)
        self.assertIn(f"0x{tables.kpt_u8[0]:02X}", cpp)
        # Function signature compatible with st::ecu::SecurityKeyFn
        self.assertIn("ssmcan1_real(std::span<std::uint8_t const> seed)", cpp)
        self.assertIn("::st::Result<std::vector<std::uint8_t>>", cpp)
        # Algorithm shape — should reference the documented operations
        self.assertIn("state >> 3", cpp)         # ror32
        self.assertIn("state << 29", cpp)        # ror32 complement
        self.assertIn("(low << 16) | high", cpp) # final byte swap


@unittest.skipUnless(_z3_available(), "z3-solver not installed; skipping symbolic-solve tests")
class TestZ3Solver(unittest.TestCase):
    def test_round_trip_recovers_encryption(self) -> None:
        """Generate pairs, solve, verify the recovered tables reproduce
        the originals on the original pairs. (Tables themselves may or
        may not be the unique original — see ambiguity test — but the
        encryption function on the captured seeds must match.)"""
        tables = solve_ssmcan1.generate_tables(rng_seed=100)
        pairs = solve_ssmcan1.generate_pairs(tables, num_pairs=24, rng_seed=101)
        result = solve_ssmcan1.solve_with_z3(pairs, timeout_ms=30_000)
        self.assertIn(result.status, ("unique", "ambiguous"),
                      msg=f"unexpected status: {result.status}, notes: {result.notes}")
        self.assertIsNotNone(result.tables)
        # Self-verify: recovered tables must reproduce every captured pair.
        report = solve_ssmcan1.verify_pairs(pairs, result.tables)
        self.assertEqual(report.passed, len(pairs))

    def test_ambiguity_detected_with_too_few_pairs(self) -> None:
        """With just 1-2 pairs the system is wildly under-determined;
        Z3 should report ambiguity rather than claiming uniqueness."""
        tables = solve_ssmcan1.generate_tables(rng_seed=200)
        pairs = solve_ssmcan1.generate_pairs(tables, num_pairs=1, rng_seed=201)
        result = solve_ssmcan1.solve_with_z3(pairs, timeout_ms=30_000)
        # Either ambiguous (the expected case) or unique-by-luck. The
        # critical thing is the harness reports ambiguity SOMETIMES;
        # we make the seed deterministic so this is reproducible.
        self.assertEqual(result.status, "ambiguous",
                         msg=f"expected ambiguous, got {result.status}")
        self.assertIsNotNone(result.alternate_tables)
        # Both candidates must reproduce the captured pair(s).
        for candidate in (result.tables, result.alternate_tables):
            report = solve_ssmcan1.verify_pairs(pairs, candidate)
            self.assertEqual(report.passed, len(pairs))


if __name__ == "__main__":
    unittest.main()
