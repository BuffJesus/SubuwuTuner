// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ap3::cipher::aes256_ctr — layer 3 of the `.ptm` cipher
// chain per spec §13. AES-256 in counter (CTR) mode with the key
// constant `bJTccI%878cPs%2$Tf8EXdzP2!cRUZw&` (32-byte ASCII; spec
// §13 layer 3) and the IV `0x20 0x00 ... 0x00` (4-byte nonce padded
// with 12 zeros).
//
// CTR mode is symmetric — encrypt and decrypt are the same operation
// (XOR plaintext/ciphertext with the keystream generated from the
// counter). Both entry points exist so callers can express intent;
// the implementation is shared.
//
// Build-flag gated. When `ST_AP3_HAVE_CIPHER` is undefined every
// function returns `PolicyDenied`. When defined the implementation
// lifts tiny-AES-c (public domain) for the AES primitive — see
// `THIRD_PARTY_NOTICES.md` and `specs/cobb-ap3-tier3-dep-survey.md`.

#ifndef ST_DEVICES_AP3_AES256_CTR_HPP
#define ST_DEVICES_AP3_AES256_CTR_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace st::devices::ap3::cipher {

// Decrypt under the spec §13 key + IV. Input length is arbitrary
// (CTR doesn't require block alignment); output has the same length.
[[nodiscard]] Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> ciphertext);

// Encrypt under the spec §13 key + IV. Same operation as decrypt
// because CTR is XOR-with-keystream; both directions are exposed
// for intent.
[[nodiscard]] Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> plaintext);

} // namespace st::devices::ap3::cipher

#endif // ST_DEVICES_AP3_AES256_CTR_HPP
