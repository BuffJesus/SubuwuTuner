// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/subaru_security.hpp"

#include "st/core/error.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::subaru {

namespace {

// -----------------------------------------------------------------------------
// Gen-A SSMCAN1 — 16-round Feistel on 32-bit blocks with a 5-bit S-box.
//
// Recovered analyst-side from the plaintext flash images of every available
// A-series CID (LF75 .. LF9L, model years 2015-2021). The S-box, the level-1
// round-key table, and the level-3/5 round-key table are byte-identical
// across all sampled CIDs. Per-CID flash offsets are catalogued under
// fixtures/private/findings_algorithms/.
//
// See docs/23-security-access.md for the algorithm description and
// docs/17-data-distribution-policy.md §7 for why this lives in-tree.
// -----------------------------------------------------------------------------

// 32-entry × 4-bit S-box, packed one entry per uint8_t.
constexpr std::array<std::uint8_t, 32> kSBox = {
    0x05, 0x06, 0x07, 0x01, 0x09, 0x0c, 0x0d, 0x08,
    0x0a, 0x0d, 0x02, 0x0b, 0x0f, 0x04, 0x00, 0x03,
    0x0b, 0x04, 0x06, 0x00, 0x0f, 0x02, 0x0d, 0x09,
    0x05, 0x0c, 0x01, 0x0a, 0x03, 0x0d, 0x0e, 0x08,
};

// Level-1 round-key table (16 × uint16, big-endian on the wire). Level 1
// is the bootloader-unlock level — sufficient for ReadMemoryByAddress
// and RequestDownload (so, sufficient for a stock ROM dump).
constexpr std::array<std::uint16_t, 16> kRoundKeysL1 = {
    0x78b1, 0x4625, 0x201c, 0x9ea5,
    0xad6b, 0x35f4, 0xfd21, 0x5e71,
    0xb046, 0x7f4a, 0x4b75, 0x93f9,
    0x1895, 0x8961, 0x3ecc, 0x862b,
};

// Level-3 / Level-5 round-key table, shared on Gen-A.2. Byte-verified
// across every A-series ROM sampled (LF7x/LF9x, MY 2015-2021). Loaded
// in-tree because COBB-tuned ECUs swap which table the L1 feistel uses
// — see `ssmcan1_l1_cobb_tuned` below.
constexpr std::array<std::uint16_t, 16> kRoundKeysL35 = {
    0x794b, 0x3caf, 0x3019, 0x8b57,
    0x52a0, 0xa77c, 0x38c9, 0xb0b5,
    0x6520, 0x3b66, 0xa09d, 0x2877,
    0x479f, 0xb685, 0x7568, 0x84d7,
};

constexpr std::uint16_t rol16(std::uint16_t v, unsigned n) noexcept {
    n &= 15U;
    if (n == 0) {
        return v;
    }
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(v) << n) | (static_cast<unsigned>(v) >> (16U - n)));
}

// Per-round F function. XOR with the round key, four overlapping 5-bit
// S-box lookups, then a 16-bit rotate-left by 13. The high-nibble index
// has bit 0 of x promoted into its MSB — the asymmetric construction the
// firmware uses to defeat naive byte-reversal attacks.
constexpr std::uint16_t feistel_F(std::uint16_t x, std::uint16_t k) noexcept {
    x = static_cast<std::uint16_t>(x ^ k);
    unsigned const i3 = ((x & 0x0001U) << 4) | (x >> 12);
    unsigned const i2 = (x >> 8) & 0x1FU;
    unsigned const i1 = (x >> 4) & 0x1FU;
    unsigned const i0 = x & 0x1FU;
    unsigned const y = (unsigned{kSBox[i3]} << 12) |
                       (unsigned{kSBox[i2]} << 8) |
                       (unsigned{kSBox[i1]} << 4) |
                        unsigned{kSBox[i0]};
    return rol16(static_cast<std::uint16_t>(y), 13);
}

// 16 forward Feistel rounds. The ECU runs this direction starting from
// internal_key to produce the seed it emits on the wire. The tester does
// not call this in production, but the round-trip property holds:
// feistel_inverse(feistel_forward(K, rk), rk) == K for every K and rk.
constexpr std::uint32_t feistel_forward(std::uint32_t state,
                                        std::span<std::uint16_t const, 16> rk) noexcept {
    for (auto const k : rk) {
        auto const L = static_cast<std::uint16_t>(state & 0xFFFFU);
        auto const H = static_cast<std::uint16_t>(state >> 16);
        auto const new_L = static_cast<std::uint16_t>(H ^ feistel_F(L, k));
        auto const new_H = L;
        state = (static_cast<std::uint32_t>(new_H) << 16) |
                 static_cast<std::uint32_t>(new_L);
    }
    return state;
}

// 16 inverse Feistel rounds — unwinds rounds in reverse with the same
// table. This is the tester's path: state' → internal_key.
constexpr std::uint32_t feistel_inverse(std::uint32_t state,
                                        std::span<std::uint16_t const, 16> rk) noexcept {
    for (std::size_t i = rk.size(); i-- > 0;) {
        auto const L_new = static_cast<std::uint16_t>(state & 0xFFFFU);
        auto const H_new = static_cast<std::uint16_t>(state >> 16);
        auto const L_old = H_new;
        auto const H_old = static_cast<std::uint16_t>(L_new ^ feistel_F(L_old, rk[i]));
        state = (static_cast<std::uint32_t>(H_old) << 16) |
                 static_cast<std::uint32_t>(L_old);
    }
    return state;
}

// The forward direction's final step is a wordswap (swap the two 16-bit
// halves of the 32-bit state) before the bytes leave the chip. Its own
// inverse, so the tester applies it twice — once to undo, once again
// after unwinding rounds.
constexpr std::uint32_t wordswap32(std::uint32_t v) noexcept {
    return (v >> 16) | (v << 16);
}

constexpr std::uint32_t read_u32_be(std::span<std::uint8_t const> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24) |
           (static_cast<std::uint32_t>(b[1]) << 16) |
           (static_cast<std::uint32_t>(b[2]) << 8) |
            static_cast<std::uint32_t>(b[3]);
}

constexpr void write_u32_be(std::uint32_t v, std::span<std::uint8_t> b) noexcept {
    b[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
    b[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    b[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    b[3] = static_cast<std::uint8_t>(v & 0xFFU);
}

// Shared failure message for the two algorithms that remain stubs.
constexpr char const *kStubMsg =
    "subaru security key stub — algorithm not provided for this Subaru era. "
    "See src/ecu/include/st/ecu/subaru_security.hpp for which eras are "
    "implemented in-tree and how to plug in others via "
    "Flasher::set_security_key_fn(). Until then SecurityAccess at this level "
    "will fail with this NotImplemented error.";

[[nodiscard]] Result<std::vector<std::uint8_t>>
not_implemented(std::span<std::uint8_t const> /*seed*/) {
    return failure(ErrorCode::NotImplemented, std::string{kStubMsg});
}

} // namespace

// -----------------------------------------------------------------------------
// Internal helpers exposed for unit tests via subaru_security_internal.hpp.
// Production code should call the public seed→key functions below.
// -----------------------------------------------------------------------------

namespace internal {

std::uint16_t test_only_F(std::uint16_t x, std::uint16_t k) noexcept {
    return feistel_F(x, k);
}

std::uint32_t test_only_feistel_forward(std::uint32_t state,
                                        std::span<std::uint16_t const, 16> rk) noexcept {
    return feistel_forward(state, rk);
}

std::uint32_t test_only_feistel_inverse(std::uint32_t state,
                                        std::span<std::uint16_t const, 16> rk) noexcept {
    return feistel_inverse(state, rk);
}

std::span<std::uint16_t const, 16> test_only_round_keys_l1() noexcept {
    return std::span<std::uint16_t const, 16>{kRoundKeysL1};
}

} // namespace internal

namespace {

// Shared L1 wire body — only the round-key table varies between stock
// and the COBB-tuned variant. Both call this with the table they want.
Result<std::vector<std::uint8_t>>
ssmcan1_l1_compute(std::span<std::uint8_t const> seed,
                   std::span<std::uint16_t const, 16> rk,
                   char const *variant_name) {
    if (seed.size() != 4) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ssmcan1 ("} + variant_name +
                           " L1): seed must be exactly 4 bytes, got " +
                           std::to_string(seed.size()));
    }
    auto const seed_packed = read_u32_be(seed);
    auto const state_post_rounds = wordswap32(seed_packed);
    auto const internal_key = feistel_inverse(state_post_rounds, rk);
    std::vector<std::uint8_t> key(4);
    write_u32_be(internal_key, key);
    return key;
}

} // namespace

Result<std::vector<std::uint8_t>> ssmcan1_key_stub(std::span<std::uint8_t const> seed) {
    // Historical name. As of 2026-05-24 this is the real Gen-A.2 level-1
    // implementation, not a stub — kept under the original symbol so the
    // Flasher default doesn't need to chase a rename across every caller.
    // The other two functions in this file remain genuine NotImplemented
    // stubs.
    return ssmcan1_l1_compute(
        seed, std::span<std::uint16_t const, 16>{kRoundKeysL1}, "Gen-A");
}

Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_tuned(std::span<std::uint8_t const> seed) {
    // COBB-tuned variant: identical algorithm structure, but the L1 feistel
    // uses kRoundKeysL35 (the stock L3/L5 table) instead of kRoundKeysL1.
    //
    // EMPIRICAL FINDING (2026-05-24, not yet field-verified beyond 1 pair):
    // on a 2017 LF79103P that has been COBB-flashed, the L1 SA challenge
    // `seed=4BC3CC87` returned `key=A73FED09` — which matches this
    // RK_L35-based derivation exactly (random-match probability 2^-32).
    // The same session's L3 pair also matched a parallel table-swap
    // hypothesis (L3 uses RK_L1), suggesting a clean pointer swap in
    // COBB's flash patch.
    //
    // HOWEVER, 3 earlier L3 captures from prior sessions on the same car
    // do NOT match any simple table-swap, and 2 L3 pairs captured 9s apart
    // in the SAME session don't share any recoverable common RK. So L3 has
    // additional session-dependent state we don't yet model. Whether L1
    // shares that property is unverified — only 1 L1 pair on file.
    //
    // Caller risk: if L1 also has hidden session state, this derivation
    // will fail with NRC 0x35 (invalidKey) and burn 1 of 3 SA attempts.
    // Capture additional L1 pairs (multiple reinstalls) before relying
    // on this in production. Plumbed into the CLI behind `--cobb-tuned`
    // so the user opts in explicitly.
    return ssmcan1_l1_compute(
        seed, std::span<std::uint16_t const, 16>{kRoundKeysL35}, "COBB-tuned");
}

Result<std::vector<std::uint8_t>> ssmk1_key_stub(std::span<std::uint8_t const> seed) {
    // Pre-2008 K-Line algorithm; no analyst-mode recovery yet (no live
    // K-Line capture rig in hand).
    return not_implemented(seed);
}

Result<std::vector<std::uint8_t>> cy1_aes_key_stub(std::span<std::uint8_t const> seed) {
    // Gen-B (RH850, 2022+): AES-128 ECB with three universal master keys
    // recovered analyst-side. Not yet implemented in-tree — needs an AES
    // primitive choice + the per-level key plumbed through. The user's
    // 2017 daily-driver is Gen-A, so this isn't on the immediate path.
    return not_implemented(seed);
}

} // namespace st::ecu::subaru
