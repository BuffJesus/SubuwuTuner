// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unit tests for the Gen-A SSMCAN1 SecurityAccess implementation
// (src/ecu/src/subaru_security.cpp). Hand-computed F-function vectors,
// round-trip property tests for the Feistel construction, and
// wire-byte handling for the level-1 seed/key path.

#include "st/ecu/subaru_security.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace st;

namespace {

// Hand-computed reference values for the per-round F function. Verified
// against the spec at fixtures/private/findings_algorithms/
// generation-A-seed-to-key.md (algorithm description, not constants).
//
// F(x, k): x XOR k, four overlapping 5-bit S-box lookups, rotate-left 13.
// The high-nibble index has bit 0 of x promoted to bit 4 (asymmetric).
//
// All three vectors below were derived by hand-stepping the algorithm
// against the documented S-box; no source implementation consulted.
struct FVector {
    std::uint16_t x;
    std::uint16_t k;
    std::uint16_t expected;
};

constexpr std::array<FVector, 3> kFVectors = {{
    // F(0, 0): all four S-box indices = 0 → S[0] = 0x05 four times →
    // y = 0x5555 → rol16(0x5555, 13) = 0xAAAA.
    {0x0000U, 0x0000U, 0xAAAAU},

    // F(0, 0xFFFF): x = 0xFFFF, all indices = 31, S[31] = 0x08 four times →
    // y = 0x8888 → rol16(0x8888, 13) = 0x1111. Exercises the
    // bit-0-promotes-to-bit-4 path on i_3.
    {0x0000U, 0xFFFFU, 0x1111U},

    // F(0x1234, 0x5678): mixed indices.
    // x = 0x444C, i_3 = 4, i_2 = 4, i_1 = 4, i_0 = 12 → S[4]=9, S[12]=0x0f.
    // y = 0x999F → rol16(0x999F, 13) = 0xF333.
    {0x1234U, 0x5678U, 0xF333U},
}};

} // namespace

TEST_CASE("Gen-A F function matches hand-computed vectors", "[ecu][sa][gen_a]") {
    using ecu::subaru::internal::test_only_F;
    for (auto const &v : kFVectors) {
        CAPTURE(v.x, v.k);
        REQUIRE(test_only_F(v.x, v.k) == v.expected);
    }
}

TEST_CASE("Gen-A F function: k masks x by XOR (self-cancellation)",
          "[ecu][sa][gen_a]") {
    using ecu::subaru::internal::test_only_F;
    // For any k, F(k, k) computes against x=0, so the result is fixed.
    REQUIRE(test_only_F(0x0000U, 0x0000U) == 0xAAAAU);
    REQUIRE(test_only_F(0xFFFFU, 0xFFFFU) == 0xAAAAU);
    REQUIRE(test_only_F(0xCAFEU, 0xCAFEU) == 0xAAAAU);
    REQUIRE(test_only_F(0xDEADU, 0xDEADU) == 0xAAAAU);
}

TEST_CASE("Gen-A Feistel forward and inverse are mutual inverses",
          "[ecu][sa][gen_a]") {
    using ecu::subaru::internal::test_only_feistel_forward;
    using ecu::subaru::internal::test_only_feistel_inverse;
    using ecu::subaru::internal::test_only_round_keys_l1;
    auto const rk = test_only_round_keys_l1();

    // Spot inputs spanning the 32-bit state space.
    constexpr std::array<std::uint32_t, 8> inputs = {
        0x00000001U, 0xCAFEBABEU, 0xDEADBEEFU, 0x12345678U,
        0x80000000U, 0x00000080U, 0xFFFFFFFFU, 0xA5A5A5A5U,
    };
    for (auto const k : inputs) {
        CAPTURE(k);
        auto const forward = test_only_feistel_forward(k, rk);
        auto const back = test_only_feistel_inverse(forward, rk);
        REQUIRE(back == k);
    }
}

TEST_CASE("Gen-A Feistel zero is not a fixed point (round-trip still holds)",
          "[ecu][sa][gen_a]") {
    // The spec's "zero is a no-op fixed point" phrasing referred to ECU
    // policy (the ECU refuses an internal_key of 0 by re-rolling), not
    // to a mathematical property of the Feistel itself. With our recovered
    // S-box and round-key constants F(0, k) is non-zero for k = K_L1[0],
    // so the Feistel does NOT fix 0 — confirm and pin the round-trip
    // property instead.
    using ecu::subaru::internal::test_only_feistel_forward;
    using ecu::subaru::internal::test_only_feistel_inverse;
    using ecu::subaru::internal::test_only_round_keys_l1;
    auto const rk = test_only_round_keys_l1();
    auto const forward_of_zero = test_only_feistel_forward(0U, rk);
    REQUIRE(forward_of_zero != 0U);
    REQUIRE(test_only_feistel_inverse(forward_of_zero, rk) == 0U);
}

TEST_CASE("ssmcan1_key_stub rejects wrong seed length",
          "[ecu][sa][gen_a]") {
    using ecu::subaru::ssmcan1_key_stub;
    {
        auto const r = ssmcan1_key_stub({});
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code() == ErrorCode::InvalidArgument);
    }
    {
        std::vector<std::uint8_t> const three{1, 2, 3};
        auto const r = ssmcan1_key_stub(three);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code() == ErrorCode::InvalidArgument);
    }
    {
        std::vector<std::uint8_t> const five{1, 2, 3, 4, 5};
        auto const r = ssmcan1_key_stub(five);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("ssmcan1_key_stub accepts any 4-byte seed",
          "[ecu][sa][gen_a]") {
    using ecu::subaru::ssmcan1_key_stub;
    // The function never rejects a 4-byte input — the ECU is the
    // authority on whether the key matches. Spot-check several seeds.
    constexpr std::array<std::array<std::uint8_t, 4>, 6> seeds = {{
        {0xDE, 0xAD, 0xBE, 0xEF},
        {0x00, 0x00, 0x00, 0x01},
        {0xFF, 0xFF, 0xFF, 0xFF},
        {0xCA, 0xFE, 0xBA, 0xBE},
        {0x12, 0x34, 0x56, 0x78},
        {0xA5, 0xA5, 0xA5, 0xA5},
    }};
    for (auto const &s : seeds) {
        CAPTURE(s[0], s[1], s[2], s[3]);
        auto const r = ssmcan1_key_stub(std::span<std::uint8_t const>{s});
        REQUIRE(r.has_value());
        REQUIRE(r->size() == 4);
    }
}

TEST_CASE("ssmcan1_key_stub end-to-end: forward(K) → seed → key == K",
          "[ecu][sa][gen_a]") {
    // The ECU side runs feistel_forward(internal_key, L1_rk) → wordswap →
    // seed_bytes. The tester runs ssmcan1_key_stub(seed_bytes) and the
    // recovered key bytes must equal internal_key's BE representation.
    using ecu::subaru::ssmcan1_key_stub;
    using ecu::subaru::internal::test_only_feistel_forward;
    using ecu::subaru::internal::test_only_round_keys_l1;
    auto const rk = test_only_round_keys_l1();

    auto const wordswap = [](std::uint32_t v) noexcept {
        return (v >> 16) | (v << 16);
    };
    auto const to_bytes = [](std::uint32_t v) {
        return std::array<std::uint8_t, 4>{
            static_cast<std::uint8_t>((v >> 24) & 0xFFU),
            static_cast<std::uint8_t>((v >> 16) & 0xFFU),
            static_cast<std::uint8_t>((v >> 8) & 0xFFU),
            static_cast<std::uint8_t>(v & 0xFFU),
        };
    };

    constexpr std::array<std::uint32_t, 6> internal_keys = {
        0x00000001U, 0xCAFEBABEU, 0xDEADBEEFU, 0x12345678U,
        0xA5A5A5A5U, 0xFFFFFFFEU,
    };
    for (auto const ik : internal_keys) {
        CAPTURE(ik);
        auto const post_rounds = test_only_feistel_forward(ik, rk);
        auto const seed_packed = wordswap(post_rounds);
        auto const seed_wire = to_bytes(seed_packed);

        auto const key = ssmcan1_key_stub(std::span<std::uint8_t const>{seed_wire});
        REQUIRE(key.has_value());
        auto const expected_key_bytes = to_bytes(ik);
        REQUIRE(std::vector<std::uint8_t>(expected_key_bytes.begin(),
                                           expected_key_bytes.end()) == *key);
    }
}

TEST_CASE("ssmcan1_key_stub: degenerate-seed cases produce deterministic keys",
          "[ecu][sa][gen_a]") {
    // The Feistel is non-fixed at 0, so a zero seed doesn't loop back to
    // a zero key. What matters operationally is that the result is
    // deterministic — the same seed always produces the same key, which
    // is the ECU's session-validation contract.
    using ecu::subaru::ssmcan1_key_stub;
    std::array<std::uint8_t, 4> const zero{0x00, 0x00, 0x00, 0x00};
    auto const r1 = ssmcan1_key_stub(std::span<std::uint8_t const>{zero});
    auto const r2 = ssmcan1_key_stub(std::span<std::uint8_t const>{zero});
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1->size() == 4);
    REQUIRE(*r1 == *r2);
    // And the result is non-trivial (not just the zero seed echoed back).
    REQUIRE(*r1 != std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("ssmk1_key_stub remains NotImplemented",
          "[ecu][sa][gen_a]") {
    // Sanity check that the K-Line algorithm stub continues to surface
    // a clear error message. Implementation deferred (no K-Line capture
    // rig in hand).
    using ecu::subaru::ssmk1_key_stub;
    std::array<std::uint8_t, 4> const seed{0xDE, 0xAD, 0xBE, 0xEF};
    auto const r = ssmk1_key_stub(std::span<std::uint8_t const>{seed});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == ErrorCode::NotImplemented);
}

TEST_CASE("cy1_aes_key_stub remains NotImplemented",
          "[ecu][sa][gen_a]") {
    // Sanity check that Gen-B (AES) still routes through the stub.
    // The algorithm + master keys are known analyst-side; in-tree
    // implementation deferred pending an AES primitive choice.
    using ecu::subaru::cy1_aes_key_stub;
    std::array<std::uint8_t, 16> const seed{
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
        0x12, 0x34, 0x56, 0x78, 0xA5, 0xA5, 0xA5, 0xA5,
    };
    auto const r = cy1_aes_key_stub(std::span<std::uint8_t const>{seed});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == ErrorCode::NotImplemented);
}
