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

// SA round-key tables for Gen-A.2 SH7058 silicon.
//
// The L1 SA dispatcher (bootloader code at flash 0x000BE8CC) loads its
// round-key table from flash address 0x074338; the L3/L5 dispatcher
// loads from 0x074358. The constants below are named to match what the
// dispatcher does.
//
// Provenance: extracted from a 2017 LF79103P dump (post-aftermarket-
// uninstall state) SHA-256
// 52e60da2c1e7f5d1bdc3f45ee1ed78745cf32a81261c07a7aa6c5906538831cc;
// byte-identical to every other LF79 reference ROM in our corpus, all
// of which have been touched by an aftermarket installer at some point.
// Per-CID variation does exist across the LF79 ECU family, so these
// values are NOT universal across all Gen-A.2 hardware.
//
// We do NOT have a validated truly-stock LF79103P reference on file.
// A user running a never-tuned LF79103P would likely need different
// bytes here for SA to succeed; on the upside, the bytes can be read
// out of any successfully-dumped ROM at the same flash offsets.

// Loaded for L1 dispatch (subfn 0x01 / 0x02). Flash address 0x074338.
constexpr std::array<std::uint16_t, 16> kSaTableL1 = {
    0x794b, 0x3caf, 0x3019, 0x8b57,
    0x52a0, 0xa77c, 0x38c9, 0xb0b5,
    0x6520, 0x3b66, 0xa09d, 0x2877,
    0x479f, 0xb685, 0x7568, 0x84d7,
};

// Loaded for L3 / L5 dispatch (subfn 0x03/0x04 / 0x05/0x06).
// Flash address 0x074358.
constexpr std::array<std::uint16_t, 16> kSaTableL35 = {
    0x78b1, 0x4625, 0x201c, 0x9ea5,
    0xad6b, 0x35f4, 0xfd21, 0x5e71,
    0xb046, 0x7f4a, 0x4b75, 0x93f9,
    0x1895, 0x8961, 0x3ecc, 0x862b,
};

// Aftermarket-framework L1 round-key table. Any tune installed by a
// common aftermarket flasher on this CID family writes THESE bytes to
// flash 0x074338 (replacing the factory L1 keys) and patches the SA
// dispatcher's loop iteration from `for r in 0..15` to `for r in 15..0`.
// The byte-level evidence is a 5-byte code patch at flash
// 0xBE911 + 0xBE9C7..0xBE9CE.
//
// The constants are tied to the aftermarket framework, not to any one
// vendor: every installer we have sampled writes the SAME L1 bytes to
// 0x074338, the SAME L35 bytes to 0x074358, and the SAME loop-reversal
// patch. The tuner-tag region at flash 0x001FFFC0 IS per-vendor
// (4-byte ASCII vendor tag), so use that region — not the SA keys —
// to discriminate vendor at runtime.
//
// Validated by computing the SA key for several independently captured
// (seed, key) pairs and confirming byte-exact matches, plus a live
// hardware confirmation: read the pairing token at flash 0x001FFFB0
// over UDS SA + RMBA, expected `64 11 4A 47`, got exactly that. Full
// 2 MB live dump matched our reference byte-for-byte at every non-0xFF
// position.
//
// Net effect of the two patches: per Feistel structural symmetry,
// running the forward routine with reversed key iteration is
// equivalent to running the forward routine on a swapped state with
// the un-reversed keys. Concretely: the aftermarket tester runs forward
// Feistel + final wordswap on the SEED bytes to produce the KEY bytes —
// opposite of the factory direction (factory tester runs inverse
// Feistel on wordswapped seed).
//
// S-box at 0x074378 and B6 cipher constants at 0x074398 are NOT
// modified by the aftermarket framework.
constexpr std::array<std::uint16_t, 16> kSaTableL1Aftermarket = {
    0x9ec3, 0x9190, 0x095b, 0xbb25,
    0xf476, 0xe722, 0xb623, 0xb3b9,
    0xe513, 0x8c80, 0xc3a1, 0x5cb2,
    0xe9ac, 0xc45b, 0xc832, 0x415c,
};

// Aftermarket-framework L35 round-key table. Same provenance and
// cross-vendor scope as `kSaTableL1Aftermarket` above — the aftermarket
// install writes these bytes to flash 0x074358, regardless of which
// specific tune is being applied.
//
// Also used as the SSM-V/VI COBB SA table per RE wave 3 §F3
// (findings/re-2026-06-12-pm/) — `Init_SSMV_COBB::GetDecryptKeys` +
// `Init_SSMVI_COBB::GetDecryptKeys` resolve to this exact byte
// sequence. Same key reuse story as the COBB_CF / COBB_MAF_SD
// cross-generation reuse documented in `kSaTableCobbFlash` below.
//
// Validated against an independently captured L3 pair
// (seed=0x4ADFFE07 → key=0x24243A06), ACK'd by the ECU and followed
// by a successful RMBA read of the tuner-tag region. Joint
// random-match probability for this single 32-bit pair: 2^-32.
constexpr std::array<std::uint16_t, 16> kSaTableL35Aftermarket = {
    0x8593, 0xc32d, 0x4402, 0x21d3,
    0x8496, 0xfb45, 0x477d, 0xce15,
    0x7f48, 0xcc0d, 0xc771, 0x0562,
    0x86f0, 0x107e, 0xbf37, 0x60c8,
};

// SSM-V factory round-key table. Per RE wave 3 §F3: SSM-V applies
// the same round-key set as `kFeistelRoundKeysL35` (the L35 factory
// table) but in inverse order — Feistel forward + inverse swap on
// the wire seed. The bytes below are the reversed sequence of L35
// factory.
//
// SSM-V is the ISO-CAN / newer RH850 era (VB chassis 2022+). SSM-VI
// is a later revision sharing this same key reuse pattern with
// COBB-installed variants (see kSaTableL35Aftermarket note above).
constexpr std::array<std::uint16_t, 16> kFeistelRoundKeysSSMVFactory = {
    0x862b, 0x3ecc, 0x8961, 0x1895,
    0x93f9, 0x4b75, 0x7f4a, 0xb046,
    0x5e71, 0xfd21, 0x35f4, 0xad6b,
    0x9ea5, 0x201c, 0x4625, 0x78b1,
};

// COBB-active round-key tables. RE5b extracted these from
// libFlashSubaru.so 2026-06-12 PM via Capstone disasm of
// Init_SSM{III,IV}_COBB_{CF,MAF_SD}::GetDecryptKeys (each a
// 3-instruction trampoline returning a base+offset pointer into a
// shared key table). See findings/re-2026-06-12-pm/
// cobb_sa_keys_extracted.md for the full derivation + cross-checks.
//
// Cross-validation cred: SSM-IV factory keys at the same trampoline
// family decode as byte-identical to `kFeistelRoundKeysL35`, which
// the implementer had recovered independently from the 2017 WRX
// ROM dump. Two independent extraction paths agreeing → high
// confidence in the COBB tables too.
//
// Key reuse across generations: SSM-III COBB_CF == SSM-IV COBB_CF
// and SSM-III COBB_MAF_SD == SSM-IV COBB_MAF_SD (analyst confirmed
// byte-identical). One table per variant covers both SH7058 (Gen-A.1)
// and SH-2A (Gen-A.2) chassis.
//
// Direction: COBB tunes install the same SA-dispatcher reversed-
// iteration patch the broader aftermarket framework uses (see
// findings/cobb_install_flow.md), so the tester runs forward Feistel
// + wordswap with these tables — same shape as `ssmcan1_l1_aftermarket`.
// The bench-rig SA exchange is the authoritative validation gate
// (Phase 5.5 of docs/28); until then this is the high-confidence
// best-guess based on the analyst-confirmed dispatcher behavior.
constexpr std::array<std::uint16_t, 16> kSaTableCobbFlash = {
    0x5920, 0x4821, 0x14b5, 0xfdeb,
    0xce83, 0x5a66, 0x02bc, 0xf90a,
    0x5a7d, 0xbbf7, 0x14ef, 0x9ffa,
    0x8f61, 0x19ff, 0x73ee, 0x972d,
};

constexpr std::array<std::uint16_t, 16> kSaTableCobbMafSd = {
    0xaa74, 0xf6d9, 0xecab, 0x8b17,
    0x6155, 0xa6d2, 0x496c, 0x75b8,
    0x9f2c, 0x7b27, 0xc6d1, 0xb485,
    0x22b0, 0x90d4, 0xa684, 0x60bb,
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

// Forward Feistel composed with the post-loop wordswap. Used for the
// aftermarket-framework tester direction: `key_u32 =
// forward_with_swap(seed_u32, aftermarket_L1)`.
//
// Same 16 forward rounds as `feistel_forward` but with the swap baked
// in, so the seed bytes can be packed BE and fed in directly without a
// separate pre-wordswap step.
constexpr std::uint32_t feistel_forward_with_swap(std::uint32_t state,
                                                  std::span<std::uint16_t const, 16> rk) noexcept {
    auto state_hi = static_cast<std::uint16_t>(state >> 16);
    auto state_lo = static_cast<std::uint16_t>(state & 0xFFFFU);
    for (auto const k : rk) {
        auto const f_out = feistel_F(state_lo, k);
        auto const new_lo = static_cast<std::uint16_t>(state_hi ^ f_out);
        auto const new_hi = state_lo;
        state_hi = new_hi;
        state_lo = new_lo;
    }
    // Final wordswap baked in: low half goes high, high half goes low.
    return (static_cast<std::uint32_t>(state_lo) << 16) | state_hi;
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
    return std::span<std::uint16_t const, 16>{kSaTableL1};
}

} // namespace internal

namespace {

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
    // Gen-A.2 SSMCAN1 level-1 derivation using the round-key table that
    // the SA dispatcher loads for L1 (bootloader code at flash 0x000BE8CC).
    //
    // CAVEATS:
    // 1. The constants in `kSaTableL1` are observed on a post-aftermarket-
    //    uninstall LF79103P. A truly never-tuned LF79103P likely has
    //    different constants at the same flash slot — per-CID variation
    //    exists across the LF79 family. This function will fail with NRC
    //    0x35 on a truly-stock LF79103P unless its constants happen to
    //    match.
    // 2. Captured L1 pairs from this car produce seeds that vary across
    //    sessions even with the same constants, which means the Feistel
    //    has an as-yet-unidentified session-variable input. The function
    //    DOES reproduce captured keys for post-aftermarket-uninstall
    //    pairs, so empirically it works for that state — but the
    //    algorithm-level model isn't complete.
    // 3. The "_stub" suffix is historical. This is the real
    //    implementation, kept under the original symbol so the Flasher
    //    default doesn't need a rename across every caller.
    return ssmcan1_l1_compute(
        seed, std::span<std::uint16_t const, 16>{kSaTableL1}, "Gen-A L1");
}

Result<std::vector<std::uint8_t>>
ssmcan1_l1_aftermarket(std::span<std::uint8_t const> seed) {
    // Aftermarket-framework L1 SA: the ECU's reversed-iteration patch
    // makes the tester's key-derivation direction the OPPOSITE of
    // factory. Apply forward Feistel + final wordswap on the SEED bytes
    // directly to produce the KEY bytes. See `kSaTableL1Aftermarket`
    // for the derivation.
    if (seed.size() != 4) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ssmcan1 (Gen-A L1 aftermarket framework): "
                                   "seed must be exactly 4 bytes, got "} +
                           std::to_string(seed.size()));
    }
    auto const seed_packed = read_u32_be(seed);
    auto const key_u32 = feistel_forward_with_swap(
        seed_packed, std::span<std::uint16_t const, 16>{kSaTableL1Aftermarket});
    std::vector<std::uint8_t> key(4);
    write_u32_be(key_u32, key);
    return key;
}

namespace {

// Factory SEED_PERM[3] applied to a 4-byte wire seed.
// SEED_PERM[3] = {1,2,0,3} — for each wire byte i, the permuted state's
// position perm[i] receives that byte. Output bytes: out[1]=in[0],
// out[2]=in[1], out[0]=in[2], out[3]=in[3].
constexpr std::uint32_t apply_seed_perm_l3(std::uint32_t wire_seed) noexcept {
    std::uint8_t const in0 = static_cast<std::uint8_t>((wire_seed >> 24) & 0xFFU);
    std::uint8_t const in1 = static_cast<std::uint8_t>((wire_seed >> 16) & 0xFFU);
    std::uint8_t const in2 = static_cast<std::uint8_t>((wire_seed >> 8) & 0xFFU);
    std::uint8_t const in3 = static_cast<std::uint8_t>(wire_seed & 0xFFU);
    std::uint8_t const out0 = in2;
    std::uint8_t const out1 = in0;
    std::uint8_t const out2 = in1;
    std::uint8_t const out3 = in3;
    return (static_cast<std::uint32_t>(out0) << 24) |
           (static_cast<std::uint32_t>(out1) << 16) |
           (static_cast<std::uint32_t>(out2) << 8) |
            static_cast<std::uint32_t>(out3);
}

// Inverse of factory KEY_PERM[3] = {3,1,2,0}.
// Forward: nonce[perm[i]] = wire_key[i]. So wire_key[i] = state[perm[i]]:
// wire_key[0]=state[3], wire_key[1]=state[1], wire_key[2]=state[2],
// wire_key[3]=state[0].
constexpr std::uint32_t apply_inverse_key_perm_l3(std::uint32_t state) noexcept {
    std::uint8_t const s0 = static_cast<std::uint8_t>((state >> 24) & 0xFFU);
    std::uint8_t const s1 = static_cast<std::uint8_t>((state >> 16) & 0xFFU);
    std::uint8_t const s2 = static_cast<std::uint8_t>((state >> 8) & 0xFFU);
    std::uint8_t const s3 = static_cast<std::uint8_t>(state & 0xFFU);
    std::uint8_t const k0 = s3;
    std::uint8_t const k1 = s1;
    std::uint8_t const k2 = s2;
    std::uint8_t const k3 = s0;
    return (static_cast<std::uint32_t>(k0) << 24) |
           (static_cast<std::uint32_t>(k1) << 16) |
           (static_cast<std::uint32_t>(k2) << 8) |
            static_cast<std::uint32_t>(k3);
}

} // namespace

Result<std::vector<std::uint8_t>>
ssmcan1_l3_aftermarket(std::span<std::uint8_t const> seed) {
    // Aftermarket-framework L3 SA: same reversed-iteration patch as L1
    // (the SA dispatcher patch at flash 0xBE911 + 0xBE9C7..0xBE9CE is
    // shared between levels), so the tester runs forward Feistel +
    // final wordswap rather than the factory's inverse Feistel
    // direction. Differs from L1 in two places:
    //   * Round-key table is `kSaTableL35Aftermarket` (flash 0x074358).
    //   * The factory dispatcher inserts a per-level byte permutation
    //     on each side of the core: SEED_PERM[3] before the rounds and
    //     inverse KEY_PERM[3] after. These permutations are factory
    //     firmware; the aftermarket framework does NOT modify them.
    if (seed.size() != 4) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ssmcan1 (Gen-A L3 aftermarket framework): "
                                   "seed must be exactly 4 bytes, got "} +
                           std::to_string(seed.size()));
    }
    auto const seed_packed = read_u32_be(seed);
    auto const permuted_seed = apply_seed_perm_l3(seed_packed);
    auto const cipher_out = feistel_forward_with_swap(
        permuted_seed, std::span<std::uint16_t const, 16>{kSaTableL35Aftermarket});
    auto const wire_key_u32 = apply_inverse_key_perm_l3(cipher_out);
    std::vector<std::uint8_t> key(4);
    write_u32_be(wire_key_u32, key);
    return key;
}

Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_flash(std::span<std::uint8_t const> seed) {
    // L1 SecurityAccess derivation for ECUs in the COBB-installed-tune
    // state ("COBB Flash" variant). Same algorithm shape as
    // `ssmcan1_l1_aftermarket` — forward Feistel + final wordswap —
    // differing only in the round-key table. See `kSaTableCobbFlash`
    // comment for analyst attribution + bench-rig validation status.
    if (seed.size() != 4) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ssmcan1 (Gen-A L1 COBB-flash): seed must "
                                   "be exactly 4 bytes, got "} +
                           std::to_string(seed.size()));
    }
    auto const seed_packed = read_u32_be(seed);
    auto const key_u32 = feistel_forward_with_swap(
        seed_packed, std::span<std::uint16_t const, 16>{kSaTableCobbFlash});
    std::vector<std::uint8_t> key(4);
    write_u32_be(key_u32, key);
    return key;
}

Result<std::vector<std::uint8_t>>
ssmcan1_l1_ssmv_factory(std::span<std::uint8_t const> seed) {
    // SSM-V factory L1 SA derivation. Per RE wave 3 §F3, SSM-V's
    // factory key flow applies the L35 round-key set in reverse
    // order (Feistel forward + inverse swap on the wire seed). Same
    // algorithm shape as `ssmcan1_l1_aftermarket`; only the round-
    // key table differs. See `kFeistelRoundKeysSSMVFactory`.
    if (seed.size() != 4) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ssmcan1 (Gen-A L1 SSM-V factory): seed "
                                   "must be exactly 4 bytes, got "} +
                           std::to_string(seed.size()));
    }
    auto const seed_packed = read_u32_be(seed);
    auto const key_u32 = feistel_forward_with_swap(
        seed_packed,
        std::span<std::uint16_t const, 16>{kFeistelRoundKeysSSMVFactory});
    std::vector<std::uint8_t> key(4);
    write_u32_be(key_u32, key);
    return key;
}

Result<std::vector<std::uint8_t>>
ssmcan1_l1_cobb_maf_sd(std::span<std::uint8_t const> seed) {
    // L1 SecurityAccess derivation for ECUs running COBB's MAF-based
    // Speed-Density variant. Same algorithm shape as the COBB-flash
    // variant above; only the round-key table differs. See
    // `kSaTableCobbMafSd` comment for analyst attribution.
    if (seed.size() != 4) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"ssmcan1 (Gen-A L1 COBB-MAF-SD): seed must "
                                   "be exactly 4 bytes, got "} +
                           std::to_string(seed.size()));
    }
    auto const seed_packed = read_u32_be(seed);
    auto const key_u32 = feistel_forward_with_swap(
        seed_packed, std::span<std::uint16_t const, 16>{kSaTableCobbMafSd});
    std::vector<std::uint8_t> key(4);
    write_u32_be(key_u32, key);
    return key;
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
