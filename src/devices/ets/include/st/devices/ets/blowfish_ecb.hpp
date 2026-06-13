// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Minimal Blowfish-ECB primitive used by the OTA `.img` cipher chain
// (`ota_cipher.cpp`). Clean-room implementation of Bruce Schneier's
// 1993 Blowfish algorithm; pi-derived initialization constants are
// mathematical facts not subject to copyright.
//
// Only the operations the OTA chain needs are exposed:
//   * `bf_set_key`: key schedule from a variable-length byte key
//   * `bf_encrypt_block`: encrypt one 64-bit (8-byte) plaintext block
//
// Decrypt is not exposed — the OTA cipher uses Blowfish as a keystream
// generator (CTR mode), so only the encrypt direction is ever called.
//
// Gated behind `ST_ETS_HAVE_CIPHER` (same flag as the other cipher
// primitives in this directory). Default OFF.

#ifndef ST_DEVICES_AP3_BLOWFISH_ECB_HPP
#define ST_DEVICES_AP3_BLOWFISH_ECB_HPP

#include <array>
#include <cstdint>
#include <span>

namespace st::devices::ets::cipher::blowfish {

// Schedule state. 18-entry P-array + 4 × 256-entry S-boxes.
struct Context {
    std::array<std::uint32_t, 18> P{};
    std::array<std::array<std::uint32_t, 256>, 4> S{};
};

// Initialize `ctx` from `key`. Standard Blowfish key schedule:
// XOR pi-derived P / S constants with the key bytes (cycling the key
// when shorter than 72 bytes), then encrypt all-zero blocks to derive
// the final P + S arrays.
void bf_set_key(Context &ctx, std::span<std::uint8_t const> key) noexcept;

// Encrypt one 64-bit plaintext block in place. `data` is interpreted
// as two big-endian u32 halves; the output is written back in the
// same byte order.
void bf_encrypt_block(Context const &ctx, std::span<std::uint8_t, 8> data) noexcept;

} // namespace st::devices::ets::cipher::blowfish

#endif // ST_DEVICES_AP3_BLOWFISH_ECB_HPP
