// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Phase A read-pipeline validation per
// `D:\Subuwu\findings\PROMPT-subuwutuner-agent-test-dumping.md`.
//
// What this test does: ports the analyst-side reference SA key algorithm
// from `Findings/decompile/lf79103p/sa_feistel_v4.py` into a self-contained
// C++ helper, wires it through `Flasher::set_security_key_fn(...)`, and
// drives the full SA-then-RMBA path against MockTransport using one of the
// captured (seed, key) pairs from the user's 2017 LF79103P.
//
// Why this exists in addition to `test_subaru_security.cpp`: that file
// exercises the in-tree `ssmcan1_key_stub` against captured pairs at the
// algorithm level. This file exercises the orchestrator integration end
// to end — request_seed → key_fn → send_key → ack → RMBA — and pins the
// pluggable seam with an *independent* algorithm port. A regression in
// either the in-tree default or the analyst-side reference would now be
// caught by the cross-check in the third TEST_CASE below.
//
// L1 only. The Python reference handles L1/L3/L5 with per-level byte
// permutations, but at L1 both SEED_PERM and KEY_PERM are identity, so
// the port collapses to "Feistel-forward(K) wordswap → seed". Phase A
// validates the factory-state L1 path; L3/L5 and Fehr-active variants
// are out of scope here.

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/ecu/subaru_security.hpp"
#include "st/ecu/uds.hpp"
#include "st/flash.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace flash = st::flash;
namespace uds = st::ecu::uds;

namespace {

// 16 × 16-bit BE round keys at flash 0x074338 (L1 dispatch).
// Source: `Findings/HANDOFF-to-subuwutuner-2026-05-25-cipher-structure.md` §2,
// byte-identical between LF79102P and LF79103P factory dumps.
constexpr std::array<std::uint16_t, 16> kSaTableL1 = {
    0x794B, 0x3CAF, 0x3019, 0x8B57, 0x52A0, 0xA77C, 0x38C9, 0xB0B5,
    0x6520, 0x3B66, 0xA09D, 0x2877, 0x479F, 0xB685, 0x7568, 0x84D7,
};

// 32 × 4-bit S-box at flash 0x074378.
// Source: same handoff §1 (the "alt S-box" finding).
constexpr std::array<std::uint8_t, 32> kSBox = {
    0x05, 0x06, 0x07, 0x01, 0x09, 0x0C, 0x0D, 0x08,
    0x0A, 0x0D, 0x02, 0x0B, 0x0F, 0x04, 0x00, 0x03,
    0x0B, 0x04, 0x06, 0x00, 0x0F, 0x02, 0x0D, 0x09,
    0x05, 0x0C, 0x01, 0x0A, 0x03, 0x0D, 0x0E, 0x08,
};

constexpr std::uint16_t ror16(std::uint16_t v, unsigned n) noexcept {
    n &= 15U;
    if (n == 0) {
        return v;
    }
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(v) >> n) | (static_cast<unsigned>(v) << (16U - n)));
}

// Per-round F, mirroring sa_feistel_v4.py's `round_function_F`:
//   x = half ^ k
//   bit0_save = x & 1
//   nibbles[0..2] are the low three nibbles of x, but `i == 3 && bit0_save`
//   promotes bit 0 into bit 4 of the high nibble (SH-2A `bset #4,R2`).
//   subs = S-box at each nibble
//   packed = pack4(subs)  (lowest nibble → bits 0..3)
//   return ror16(packed, 3)
constexpr std::uint16_t fv4_F(std::uint16_t half, std::uint16_t k) noexcept {
    unsigned const x = static_cast<unsigned>(half ^ k) & 0xFFFFU;
    unsigned const bit0 = x & 1U;
    unsigned const n0 = x & 0x1FU;
    unsigned const n1 = (x >> 4) & 0x1FU;
    unsigned const n2 = (x >> 8) & 0x1FU;
    unsigned const n3 = ((x >> 12) | (bit0 ? 0x10U : 0U)) & 0x1FU;
    unsigned const packed = unsigned{kSBox[n0]} | (unsigned{kSBox[n1]} << 4) |
                            (unsigned{kSBox[n2]} << 8) | (unsigned{kSBox[n3]} << 12);
    return ror16(static_cast<std::uint16_t>(packed), 3);
}

// `sa_feistel_v4(nonce, level=1)`: ECU side. Forward direction.
//   16 rounds of true Feistel + final word-swap.
constexpr std::uint32_t fv4_forward(std::uint32_t nonce) noexcept {
    auto hi = static_cast<std::uint16_t>(nonce >> 16);
    auto lo = static_cast<std::uint16_t>(nonce & 0xFFFFU);
    for (auto const k : kSaTableL1) {
        auto const f_out = fv4_F(lo, k);
        auto const new_lo = static_cast<std::uint16_t>(hi ^ f_out);
        auto const new_hi = lo;
        hi = new_hi;
        lo = new_lo;
    }
    // Final swap: seed = (state_lo << 16) | state_hi.
    return (static_cast<std::uint32_t>(lo) << 16) | hi;
}

// Tester side: given the ECU's emitted seed, recover the internal key K.
// `seed = fv4_forward(K)` ⇒ `K = fv4_inverse(seed)`.
constexpr std::uint32_t fv4_inverse(std::uint32_t seed) noexcept {
    // Undo the final swap.
    auto hi = static_cast<std::uint16_t>(seed & 0xFFFFU);
    auto lo = static_cast<std::uint16_t>(seed >> 16);
    for (std::size_t i = kSaTableL1.size(); i-- > 0;) {
        auto const old_lo = hi;
        auto const f_out = fv4_F(old_lo, kSaTableL1[i]);
        auto const old_hi = static_cast<std::uint16_t>(lo ^ f_out);
        hi = old_hi;
        lo = old_lo;
    }
    return (static_cast<std::uint32_t>(hi) << 16) | lo;
}

constexpr std::uint32_t pack_be(std::array<std::uint8_t, 4> const &b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
}

constexpr std::array<std::uint8_t, 4> unpack_be(std::uint32_t v) noexcept {
    return {
        static_cast<std::uint8_t>((v >> 24) & 0xFFU),
        static_cast<std::uint8_t>((v >> 16) & 0xFFU),
        static_cast<std::uint8_t>((v >> 8) & 0xFFU),
        static_cast<std::uint8_t>(v & 0xFFU),
    };
}

st::Result<std::vector<std::uint8_t>>
fv4_key_fn(std::span<std::uint8_t const> seed) {
    if (seed.size() != 4) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "fv4_key_fn (Phase A port): seed must be 4 bytes");
    }
    std::array<std::uint8_t, 4> seed_arr{seed[0], seed[1], seed[2], seed[3]};
    auto const k = fv4_inverse(pack_be(seed_arr));
    auto const kb = unpack_be(k);
    return std::vector<std::uint8_t>(kb.begin(), kb.end());
}

void expect(st::transport::MockTransport &t, std::vector<std::uint8_t> req,
            std::vector<std::uint8_t> resp) {
    t.expect_send_recv(std::move(req), std::move(resp));
}

// Captured factory-state L1 (seed, key) pairs from sa_feistel_v4.py PAIRS.
// Each was observed on the wire from the user's 2017 LF79103P during a
// COBB reinstall capture and ack'd positively by the ECU.
struct Pair {
    std::uint32_t seed;
    std::uint32_t key;
    char const *note;
};
constexpr std::array<Pair, 3> kFactoryPairs = {{
    {0x4BC3CC87U, 0xA73FED09U, "cobb-reinstall L1"},
    {0x38C54C7CU, 0x64AEE90BU, "cobb-reinstall-2 L1"},
    {0xBD269DDFU, 0x60195F2DU, "cobb-reinstall-3 L1"},
}};

} // namespace

TEST_CASE("sa_feistel_v4 port: forward(K) == seed for captured factory pairs",
          "[flash][read][sa][v4]") {
    // Standalone algorithm check — no transport involved. The Python
    // reference asserts `Feistel(unperm_key(key)) == unperm_seed(seed)`,
    // which at L1 (identity perms) reduces to `fv4_forward(key) == seed`.
    for (auto const &p : kFactoryPairs) {
        CAPTURE(p.note, p.seed, p.key);
        REQUIRE(fv4_forward(p.key) == p.seed);
        REQUIRE(fv4_inverse(p.seed) == p.key);
    }
}

TEST_CASE("sa_feistel_v4 port matches in-tree ssmcan1_key_stub byte-for-byte",
          "[flash][read][sa][v4]") {
    // Cross-check: the test-local port and the in-tree default are
    // independent implementations of the same algorithm. They must agree
    // on every input — drift on either side fails this test and surfaces
    // the disagreement before it reaches a real ECU.
    using st::ecu::subaru::ssmcan1_key_stub;
    for (auto const &p : kFactoryPairs) {
        CAPTURE(p.note, p.seed);
        auto const seed_bytes = unpack_be(p.seed);
        auto const port = fv4_key_fn(std::span<std::uint8_t const>{seed_bytes});
        auto const intree = ssmcan1_key_stub(std::span<std::uint8_t const>{seed_bytes});
        REQUIRE(port.has_value());
        REQUIRE(intree.has_value());
        REQUIRE(*port == *intree);
        // And both reproduce the captured key byte-for-byte.
        auto const key_bytes = unpack_be(p.key);
        std::vector<std::uint8_t> const expected(key_bytes.begin(), key_bytes.end());
        REQUIRE(*port == expected);
    }
}

TEST_CASE("Flasher::read_full_rom drives SA+RMBA end-to-end with sa_feistel_v4 port",
          "[flash][read][sa][v4]") {
    // Integration: queue the exact wire bytes the orchestrator should send,
    // including the captured factory-state L1 (seed, key) pair, plus a
    // small RMBA chunk after SA. The Flasher uses the port we plugged in
    // via `set_security_key_fn` to derive the key from the ECU's seed.
    //
    // This is the read pipeline the Phase B / C live-ECU checks will
    // ride on. If this test passes, the orchestrator + UDS framing +
    // SA preamble + key-fn seam are all wired correctly; only the
    // transport layer remains as a hardware-side variable.
    auto const &p = kFactoryPairs[0]; // cobb-reinstall L1, the original anchor pair.
    auto const seed_bytes = unpack_be(p.seed);
    auto const key_bytes = unpack_be(p.key);

    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    // SA: requestSeed (subfn 0x01) → ECU returns seed.
    expect(t, uds::build_security_access_request_seed(0x01),
           {0x67, 0x01, seed_bytes[0], seed_bytes[1], seed_bytes[2], seed_bytes[3]});

    // SA: sendKey (subfn 0x02) with the key our port computes. ECU acks.
    std::vector<std::uint8_t> const expected_key(key_bytes.begin(), key_bytes.end());
    expect(t, uds::build_security_access_send_key(0x02, expected_key), {0x67, 0x02});

    // One 8-byte RMBA at 0x1000 — enough to exercise the chunk loop without
    // building a fixture for a real flash range.
    expect(t, uds::build_read_memory_by_address(0x1000, 8),
           {0x63, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80});

    flash::Flasher f{t};
    f.set_security_key_fn(fv4_key_fn);

    auto const r =
        f.read_full_rom(0x1000, 8, /*max_chunk=*/8, std::chrono::milliseconds{1000},
                        /*progress=*/nullptr, /*cancel=*/nullptr,
                        /*enter_diagnostic_session=*/false, /*authenticate=*/true,
                        /*security_level=*/0x01);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 8);
    REQUIRE((*r)[0] == 0x10);
    REQUIRE((*r)[7] == 0x80);
    REQUIRE(t.exhausted());
}
