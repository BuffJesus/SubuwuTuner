// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// 0xB6 Subaru bulk-transfer wire cipher (optional capability).
//
// The Subaru bootloader runs a small 32-bit block cipher on bytes
// received via the manufacturer-specific 0xB6 SID before writing them
// to flash. This header exposes the host-side transform for the
// orchestrator's `data_format=0x04` branch.
//
// Whole module is compile-gated behind ST_ENABLE_BULK_REFLASH_CIPHER —
// when the macro is undefined the symbols below do not exist and every
// caller must be guarded by the same #ifdef. The build flag is OFF by
// default; see docs/26-bulk-reflash-cipher.md.
//
// Algorithm (recovered analyst-side from the A-series Subaru bootloader
// at flash 0x0C54F2):
//
//   - 32-bit block, ECB mode
//   - True Feistel: L' = R ; R' = L XOR F(R)
//   - F(R) = ROR16( pack4_nibbles( SBox[ extract_5bit(R XOR rk) ] ), 3 )
//   - 4 rounds (the related SA path is 16; see subaru_security.hpp)
//   - Same 32-entry 4-bit S-box as the SA path
//
// For a true Feistel, decrypt = encrypt-with-reversed-keys. The two
// round-key tables below are the same key material in opposite orders.

#pragma once

#ifdef ST_ENABLE_BULK_REFLASH_CIPHER

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace st::ecu::bulk_reflash {

// 32-entry x 4-bit S-box — byte-identical to the SA path's S-box at flash
// 0x074378. Same firmware-level primitive, shared between two cipher
// contexts.
constexpr std::array<std::uint8_t, 32> kSBox = {
    0x05, 0x06, 0x07, 0x01, 0x09, 0x0c, 0x0d, 0x08,
    0x0a, 0x0d, 0x02, 0x0b, 0x0f, 0x04, 0x00, 0x03,
    0x0b, 0x04, 0x06, 0x00, 0x0f, 0x02, 0x0d, 0x09,
    0x05, 0x0c, 0x01, 0x0a, 0x03, 0x0d, 0x0e, 0x08,
};

// 4 x u16 BE round keys at flash 0x074398. Byte-identical across every
// A-series CID examined (LF75 .. LF9L).
constexpr std::array<std::uint16_t, 4> kRoundKeysDecrypt = {
    0x5FB1, 0xA7CA, 0x42DA, 0xB740,
};

constexpr std::array<std::uint16_t, 4> kRoundKeysEncrypt = {
    0xB740, 0x42DA, 0xA7CA, 0x5FB1,
};

[[nodiscard]] std::uint32_t feistel_block(std::uint32_t block,
                                          std::array<std::uint16_t, 4> const &round_keys) noexcept;

// Encrypt a host plaintext buffer into wire format, ECB mode, big-endian
// 4-byte blocks. Length must be a multiple of 4; partial-block padding is
// the flash orchestrator's responsibility.
[[nodiscard]] std::vector<std::uint8_t>
encrypt_bytes(std::span<std::uint8_t const> plaintext);

[[nodiscard]] std::vector<std::uint8_t>
decrypt_bytes(std::span<std::uint8_t const> ciphertext);

// -- Runtime arming (Layer 1 of the two-step arming model) --------------
//
// Even with the build flag set, the orchestrator checks is_armed() before
// each invocation. The CLI arms it when the runtime flag is passed.
// Default state is unarmed; arming is process-scoped and persists for the
// remainder of the process.
[[nodiscard]] bool is_armed() noexcept;
void arm() noexcept;

// Reset the runtime arming flag. Primarily for tests that need to
// exercise both armed and unarmed paths within the same process; the
// CLI never calls this. Safe to call when already disarmed.
void disarm() noexcept;

}  // namespace st::ecu::bulk_reflash

#endif  // ST_ENABLE_BULK_REFLASH_CIPHER
