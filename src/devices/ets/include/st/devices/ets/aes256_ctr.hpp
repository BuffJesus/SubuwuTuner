// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ets::cipher::aes256_ctr — layer 3 of the `.ptm` cipher
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
// Build-flag gated. When `ST_ETS_HAVE_CIPHER` is undefined every
// function returns `PolicyDenied`. When defined the implementation
// lifts tiny-AES-c (public domain) for the AES primitive — see
// `THIRD_PARTY_NOTICES.md` and `specs/cobb-ap3-tier3-dep-survey.md`.

#ifndef ST_DEVICES_AP3_AES256_CTR_HPP
#define ST_DEVICES_AP3_AES256_CTR_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace st::devices::ets::cipher {

// Decrypt under the spec §13 key + custom CTR construction.
// `nonce` is the 32-bit value extracted from the last 4 bytes (BE) of
// the base64-decoded `encData` blob. Input length is arbitrary;
// output has the same length.
//
// The construction is NOT standard AES-CTR — it processes the body in
// 16 KB outer chunks, each with `outer_nonce = nonce + chunk_offset`,
// and within each chunk uses 16-byte AES inputs built as
// `counter (4B BE) × 4` where counter = outer_nonce + block_idx.
// Mirrors the OEM cipher dispatcher entry in libMapFile.so. Live-verified against
// real `.ptm` samples by the reference Python tool.
[[nodiscard]] Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> ciphertext, std::uint32_t nonce);

// Encrypt — symmetric XOR-with-keystream, same construction as decrypt.
[[nodiscard]] Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> plaintext, std::uint32_t nonce);

} // namespace st::devices::ets::cipher

#endif // ST_DEVICES_AP3_AES256_CTR_HPP
