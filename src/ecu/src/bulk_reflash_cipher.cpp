// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// See bulk_reflash_cipher.hpp for the algorithm description and the
// build-flag gating rationale.

#include "st/ecu/bulk_reflash_cipher.hpp"

#ifdef ST_ENABLE_BULK_REFLASH_CIPHER

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

namespace st::ecu::bulk_reflash {

namespace {

// Rotate-right a 16-bit value by `n` bits. n in [1, 16] — n==0 would
// shift a u16 by 16 (UB), so the range excludes it. We only ever pass
// n=3 from f_round() below.
constexpr std::uint16_t ror16(std::uint16_t v, unsigned n) noexcept {
    return static_cast<std::uint16_t>((v >> n) | (v << (16 - n)));
}

// Per-round nonlinear F. Takes the 16-bit right half R and the round key,
// returns a 16-bit transform of (R XOR rk). Implementation mirrors the
// bootloader byte-for-byte; the only oddity is the bset-on-iter-3 trick
// in the nibble-extract loop (originating in the firmware's hand-tuned
// SH-2A code path).
constexpr std::uint16_t f_round(std::uint16_t r, std::uint16_t round_key) noexcept {
    std::uint32_t const x = static_cast<std::uint16_t>(r ^ round_key);
    std::uint32_t const bit0_save = x & 1U;

    // Extract four overlapping 5-bit nibbles from x, walking down by 4 bits.
    // On iteration 3, if bit 0 of the original was set, OR 0x10 into the
    // current accumulator before extracting — see bootloader bset #4,R2.
    std::uint8_t nibbles[4];
    std::uint32_t r2 = x;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == 3 && bit0_save == 1U) {
            r2 |= 0x10U;
        }
        nibbles[i] = static_cast<std::uint8_t>(r2 & 0x1FU);
        r2 = (r2 & 0xFFFFU) >> 4;
    }

    // Substitute each 5-bit index through the 4-bit S-box.
    std::uint32_t const subs0 = kSBox[nibbles[0]];
    std::uint32_t const subs1 = kSBox[nibbles[1]];
    std::uint32_t const subs2 = kSBox[nibbles[2]];
    std::uint32_t const subs3 = kSBox[nibbles[3]];

    // Repack four 4-bit results into a 16-bit value, low nibble first.
    std::uint16_t const packed =
        static_cast<std::uint16_t>(subs0 | (subs1 << 4) | (subs2 << 8) | (subs3 << 12));

    return ror16(packed, 3);
}

}  // namespace

std::uint32_t feistel_block(std::uint32_t block,
                            std::array<std::uint16_t, 4> const &round_keys) noexcept {
    std::uint16_t L = static_cast<std::uint16_t>((block >> 16) & 0xFFFFU);
    std::uint16_t R = static_cast<std::uint16_t>(block & 0xFFFFU);

    for (unsigned r = 0; r < 4; ++r) {
        std::uint16_t const f = f_round(R, round_keys[r]);
        std::uint16_t const new_R = static_cast<std::uint16_t>(L ^ f);
        L = R;
        R = new_R;
    }

    // Post-loop swap halves — matches the bootloader's two-mov-and-add
    // sequence after the round counter expires.
    return (static_cast<std::uint32_t>(R) << 16) | static_cast<std::uint32_t>(L);
}

namespace {

// Walk a byte span in 4-byte big-endian blocks through `feistel_block` with
// the supplied round-key order, return a fresh vector with the transformed
// bytes. Used by both encrypt_bytes() and decrypt_bytes().
std::vector<std::uint8_t> transform_bytes(std::span<std::uint8_t const> input,
                                          std::array<std::uint16_t, 4> const &round_keys) {
    std::vector<std::uint8_t> out;
    if (input.empty()) {
        return out;
    }
    // Caller's responsibility to align to 4 bytes; if the input length isn't
    // a multiple of 4 we truncate the tail rather than padding. Returning an
    // error is the orchestrator's job (it sees the partial-block condition
    // first because it has the sector geometry).
    std::size_t const aligned_len = input.size() & ~static_cast<std::size_t>(3);
    out.reserve(aligned_len);

    for (std::size_t i = 0; i < aligned_len; i += 4) {
        std::uint32_t const block = (static_cast<std::uint32_t>(input[i + 0]) << 24) |
                                    (static_cast<std::uint32_t>(input[i + 1]) << 16) |
                                    (static_cast<std::uint32_t>(input[i + 2]) << 8) |
                                    static_cast<std::uint32_t>(input[i + 3]);
        std::uint32_t const transformed = feistel_block(block, round_keys);
        out.push_back(static_cast<std::uint8_t>((transformed >> 24) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((transformed >> 16) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((transformed >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(transformed & 0xFFU));
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> encrypt_bytes(std::span<std::uint8_t const> plaintext) {
    return transform_bytes(plaintext, kRoundKeysEncrypt);
}

std::vector<std::uint8_t> decrypt_bytes(std::span<std::uint8_t const> ciphertext) {
    return transform_bytes(ciphertext, kRoundKeysDecrypt);
}

// Process-global arming flag. Deliberate exception to the "no global
// state" rule in CLAUDE.md: the runtime arm has to be reachable from
// the CLI pre-pass without threading a service through every subcommand
// + the flash::Flasher constructor, and the gate is intentionally
// process-scoped so a single armed invocation cannot leak its arming
// to a future invocation of the same binary.
namespace {
std::atomic<bool> g_armed{false};
}  // namespace

bool is_armed() noexcept {
    return g_armed.load(std::memory_order_acquire);
}

void arm() noexcept {
    g_armed.store(true, std::memory_order_release);
}

void disarm() noexcept {
    g_armed.store(false, std::memory_order_release);
}

}  // namespace st::ecu::bulk_reflash

#endif  // ST_ENABLE_BULK_REFLASH_CIPHER
