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


class TestLinearityCheck(unittest.TestCase):
    """Diagnostic tests for the GF(2)-linearity check.

    Note: these tests assume the CURRENT encoded `ssmcan1_encrypt` is
    linear (it is, by inspection — pure XOR + rotate, no S-box indexing).
    If the algorithm is later refined to add nonlinear elements, the
    `test_current_algorithm_is_linear` expectation flips to NONLINEAR
    and the assertion below needs updating along with the module
    docstring's "EMPIRICAL FINDING" section.
    """

    def test_current_algorithm_is_linear(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=42)
        result = solve_ssmcan1.check_linearity(tables, num_tests=16, rng_seed=0)
        self.assertTrue(
            result.is_linear,
            msg="current ssmcan1_encrypt is expected to be linear (see module "
                "docstring); if this assertion now fails, the algorithm has "
                "been refined — update the docstring's EMPIRICAL FINDING "
                "section and flip this test's expectation.",
        )
        self.assertEqual(len(result.samples), 16)
        for _, _, _, match in result.samples:
            self.assertTrue(match)

    def test_constant_is_encrypt_of_zero_seed(self) -> None:
        tables = solve_ssmcan1.generate_tables(rng_seed=99)
        result = solve_ssmcan1.check_linearity(tables, num_tests=4, rng_seed=1)
        expected = solve_ssmcan1.ssmcan1_encrypt(b"\x00\x00\x00\x00", tables)
        self.assertEqual(result.table_only_constant, expected)

    def test_cli_linearity_check_exits_zero(self) -> None:
        # Diagnostic always exits 0, regardless of LINEAR/NONLINEAR result.
        rc = solve_ssmcan1.main(["linearity-check", "--num-tests", "4"])
        self.assertEqual(rc, 0)


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


class TestGenAFeistel(unittest.TestCase):
    """Tests for the recovered Gen-A SSMCAN1 algorithm. Mirrors the
    C++ test in tests/unit/ecu/test_subaru_security.cpp.
    """

    def test_F_hand_computed_vectors(self) -> None:
        # F(0, 0): all four S-box indices = 0 -> S[0] = 0x05 four times ->
        # y = 0x5555 -> rol16(0x5555, 13) = 0xAAAA.
        self.assertEqual(solve_ssmcan1.gen_a_F(0x0000, 0x0000), 0xAAAA)
        # F(0, 0xFFFF): all indices = 31 -> S[31] = 0x08 four times ->
        # y = 0x8888 -> rol16(0x8888, 13) = 0x1111.
        self.assertEqual(solve_ssmcan1.gen_a_F(0x0000, 0xFFFF), 0x1111)
        # F(0x1234, 0x5678): mixed indices; hand-traced to 0xF333.
        self.assertEqual(solve_ssmcan1.gen_a_F(0x1234, 0x5678), 0xF333)

    def test_F_self_cancellation(self) -> None:
        for k in (0x0000, 0xFFFF, 0xCAFE, 0xDEAD, 0x1234, 0x5678):
            self.assertEqual(solve_ssmcan1.gen_a_F(k, k), 0xAAAA)

    def test_feistel_forward_inverse_round_trip(self) -> None:
        for k in (0x00000000, 0x00000001, 0xCAFEBABE, 0xDEADBEEF,
                  0x12345678, 0x80000000, 0x000000FF, 0xA5A5A5A5,
                  0xFFFFFFFF):
            forward = solve_ssmcan1.gen_a_feistel_forward(
                k, solve_ssmcan1.GEN_A_RK_L1)
            back = solve_ssmcan1.gen_a_feistel_inverse(
                forward, solve_ssmcan1.GEN_A_RK_L1)
            self.assertEqual(back, k, msg=f"round-trip failed for {k:#010x}")

    def test_seed_to_key_round_trip(self) -> None:
        # Forward (ECU side) any internal_key -> seed; tester-side
        # seed_to_key must recover the same internal_key.
        for ik in (0x00000001, 0xCAFEBABE, 0xDEADBEEF, 0x12345678,
                   0xA5A5A5A5, 0xFFFFFFFE):
            ik_bytes = ik.to_bytes(4, "big")
            seed = solve_ssmcan1.gen_a_key_to_seed_l1(ik_bytes)
            recovered = solve_ssmcan1.gen_a_seed_to_key_l1(seed)
            self.assertEqual(recovered, ik_bytes,
                             msg=f"round-trip failed for ik={ik:#010x}")

    def test_seed_to_key_rejects_wrong_length(self) -> None:
        with self.assertRaises(ValueError):
            solve_ssmcan1.gen_a_seed_to_key_l1(b"")
        with self.assertRaises(ValueError):
            solve_ssmcan1.gen_a_seed_to_key_l1(b"\x01\x02\x03")
        with self.assertRaises(ValueError):
            solve_ssmcan1.gen_a_seed_to_key_l1(b"\x01\x02\x03\x04\x05")

    def test_seed_to_key_deterministic(self) -> None:
        # Same seed always produces the same key (the ECU's session
        # validation contract).
        seed = b"\x72\x0B\xE3\x22"  # the bytes from the user's first attempt
        k1 = solve_ssmcan1.gen_a_seed_to_key_l1(seed)
        k2 = solve_ssmcan1.gen_a_seed_to_key_l1(seed)
        self.assertEqual(k1, k2)
        self.assertEqual(len(k1), 4)

    def test_seed_to_key_matches_cpp_implementation_for_user_seed(self) -> None:
        # Pins the Python implementation against the same answer the
        # C++ produced on 2026-05-24's first real-car attempt
        # (seed 72 0B E3 22 -> key 02 C5 BA 29). If this assertion
        # ever changes value, the Python and C++ implementations have
        # diverged.
        seed = b"\x72\x0B\xE3\x22"
        expected = b"\x02\xC5\xBA\x29"
        self.assertEqual(solve_ssmcan1.gen_a_seed_to_key_l1(seed), expected)


class TestVerifyGenACommand(unittest.TestCase):
    """Exit-code + JSON-handling tests for the verify-gen-a subcommand."""

    def _write_pairs(self, pairs: list[dict]) -> Path:
        tmp = tempfile.NamedTemporaryFile(
            mode="w", suffix=".json", delete=False, encoding="utf-8")
        json.dump({"schema": "subuwutuner.sa.v1", "pairs": pairs}, tmp)
        tmp.close()
        return Path(tmp.name)

    def test_matching_pairs_exit_zero(self) -> None:
        # Generate a few synthetic pairs by running forward (ECU side)
        # on known internal_keys, then verify they all round-trip.
        pairs = []
        for ik in (0xCAFEBABE, 0xDEADBEEF, 0x12345678):
            ik_bytes = ik.to_bytes(4, "big")
            seed = solve_ssmcan1.gen_a_key_to_seed_l1(ik_bytes)
            pairs.append({"seed": seed.hex().upper(), "key": ik_bytes.hex().upper()})
        path = self._write_pairs(pairs)
        try:
            rc = solve_ssmcan1.main(["verify-gen-a", str(path)])
            self.assertEqual(rc, 0)
        finally:
            path.unlink()

    def test_all_mismatching_pairs_exit_two(self) -> None:
        # Captured keys deliberately wrong — simulates "COBB-modified
        # algorithm" scenario where the algorithm never matches.
        pairs = []
        for ik in (0xCAFEBABE, 0xDEADBEEF, 0x12345678):
            ik_bytes = ik.to_bytes(4, "big")
            seed = solve_ssmcan1.gen_a_key_to_seed_l1(ik_bytes)
            wrong_key = bytes(b ^ 0xFF for b in ik_bytes)
            pairs.append({"seed": seed.hex().upper(), "key": wrong_key.hex().upper()})
        path = self._write_pairs(pairs)
        try:
            rc = solve_ssmcan1.main(["verify-gen-a", str(path)])
            self.assertEqual(rc, 2)
        finally:
            path.unlink()

    def test_partial_match_exit_three(self) -> None:
        # 2 right + 1 wrong -> partial match, exit 3.
        pairs = []
        for i, ik in enumerate((0xCAFEBABE, 0xDEADBEEF, 0x12345678)):
            ik_bytes = ik.to_bytes(4, "big")
            seed = solve_ssmcan1.gen_a_key_to_seed_l1(ik_bytes)
            key = (bytes(b ^ 0xFF for b in ik_bytes) if i == 2 else ik_bytes)
            pairs.append({"seed": seed.hex().upper(), "key": key.hex().upper()})
        path = self._write_pairs(pairs)
        try:
            rc = solve_ssmcan1.main(["verify-gen-a", str(path)])
            self.assertEqual(rc, 3)
        finally:
            path.unlink()

    def test_empty_pairs_exit_one(self) -> None:
        path = self._write_pairs([])
        try:
            rc = solve_ssmcan1.main(["verify-gen-a", str(path)])
            self.assertEqual(rc, 1)
        finally:
            path.unlink()

    def test_malformed_json_exit_one(self) -> None:
        tmp = tempfile.NamedTemporaryFile(
            mode="w", suffix=".json", delete=False, encoding="utf-8")
        tmp.write("this is not json")
        tmp.close()
        path = Path(tmp.name)
        try:
            rc = solve_ssmcan1.main(["verify-gen-a", str(path)])
            self.assertEqual(rc, 1)
        finally:
            path.unlink()

    def test_missing_file_exit_one(self) -> None:
        rc = solve_ssmcan1.main(
            ["verify-gen-a", "/nonexistent/path/does/not/exist.json"])
        self.assertEqual(rc, 1)


class TestRecoverGenA(unittest.TestCase):
    """Aftermarket-table recovery: synthesize pairs against a modified
    table, confirm recovery finds the modification."""

    def _write_pairs_for_rk(self, rk: list[int], n: int) -> Path:
        # Generate `n` synthetic pairs by running the forward algorithm
        # against the given round-key table.
        pairs = []
        seeds = [0xCAFEBABE, 0xDEADBEEF, 0x12345678, 0xA5A5A5A5,
                 0x00000001, 0xFFFFFFFE, 0x80808080, 0x55AA55AA]
        for ik in seeds[:n]:
            ik_bytes = ik.to_bytes(4, "big")
            state = solve_ssmcan1.gen_a_feistel_forward(ik, rk)
            seed_packed = solve_ssmcan1._gen_a_wordswap32(state)
            seed = seed_packed.to_bytes(4, "big")
            pairs.append({"seed": seed.hex().upper(),
                          "key": ik_bytes.hex().upper()})
        tmp = tempfile.NamedTemporaryFile(
            mode="w", suffix=".json", delete=False, encoding="utf-8")
        json.dump({"schema": "subuwutuner.sa.v1", "pairs": pairs}, tmp)
        tmp.close()
        return Path(tmp.name)

    def test_stock_pairs_recognized(self) -> None:
        # Pairs generated from stock table - recover should report
        # "ALGORITHM CORRECT, nothing to recover" and exit 0.
        path = self._write_pairs_for_rk(solve_ssmcan1.GEN_A_RK_L1, 4)
        try:
            rc = solve_ssmcan1.main(
                ["recover-gen-a", str(path), "--min-pairs", "4"])
            self.assertEqual(rc, 0)
        finally:
            path.unlink()

    def test_xor_mask_recovered(self) -> None:
        mask = 0x1234
        fake_rk = [r ^ mask for r in solve_ssmcan1.GEN_A_RK_L1]
        path = self._write_pairs_for_rk(fake_rk, 4)
        try:
            with tempfile.NamedTemporaryFile(
                    mode="w", suffix=".json", delete=False,
                    encoding="utf-8") as out:
                out_path = Path(out.name)
            rc = solve_ssmcan1.main([
                "recover-gen-a", str(path), "--min-pairs", "4",
                "--output", str(out_path),
            ])
            self.assertEqual(rc, 0)
            # Verify the JSON sidecar contains the right table.
            with out_path.open() as f:
                payload = json.load(f)
            self.assertEqual(payload["algorithm"], "gen_a_ssmcan1")
            self.assertEqual(payload["level"], 1)
            self.assertEqual(payload["verified_against_pairs"], 4)
            recovered = [int(s, 16) for s in payload["rk_l1"]]
            self.assertEqual(recovered, fake_rk)
            out_path.unlink()
        finally:
            path.unlink()

    def test_single_substitution_recovered(self) -> None:
        # Replace one position with an arbitrary value, confirm
        # single-sub search finds it. Note: XOR-mask shape would NOT
        # find this (no single C produces a one-position-only change),
        # so the test exercises the deeper search.
        fake_rk = list(solve_ssmcan1.GEN_A_RK_L1)
        fake_rk[5] = 0xBEEF  # position 5 replaced
        path = self._write_pairs_for_rk(fake_rk, 4)
        try:
            rc = solve_ssmcan1.main(
                ["recover-gen-a", str(path), "--min-pairs", "4"])
            self.assertEqual(rc, 0)
        finally:
            path.unlink()

    def test_too_few_pairs_exit_one(self) -> None:
        path = self._write_pairs_for_rk(solve_ssmcan1.GEN_A_RK_L1, 1)
        try:
            rc = solve_ssmcan1.main(
                ["recover-gen-a", str(path), "--min-pairs", "2"])
            self.assertEqual(rc, 1)
        finally:
            path.unlink()

    def test_missing_file_exit_one(self) -> None:
        rc = solve_ssmcan1.main(
            ["recover-gen-a", "/nonexistent/file.json"])
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
