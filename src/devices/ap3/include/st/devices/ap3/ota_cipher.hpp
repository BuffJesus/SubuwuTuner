// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ap3::cipher::ota — `.img` (COBB AccessPort OTA firmware
// image) cipher chain. Per spec §14 / `docs/34-cobb-ap-as-tune-vault.md`:
//
// The `.img` envelope is:
//   [0..3]       u32 BE base_ctr (per-file Blowfish counter)
//   [4..5]       u16 unknown header / version field
//   [6..N-32]    body bytes — Blowfish-CTR-double encrypted
//   [N-32..N]    32-byte trailer (signature?  HMAC?  un-RE'd)
//
// The body is encrypted with Blowfish-CTR applied **twice**: the
// keystream from a Blowfish-CTR pass over the base counter is XOR'd
// with the plaintext, then a second pass over the same counter is
// XOR'd with the result. (Equivalent to two distinct keys composed.)
//
// Status:
//   * Wrapper parse/build  (this header)  — LANDED, no cipher dep
//   * Blowfish primitive   (ota_cipher.cpp) — Tier-A NotImplemented;
//                                              needs vendored
//                                              public-domain Blowfish
//                                              reference (~300 lines C).
//
// Distribution gating matches the `.ptm` cipher chain: every function
// returns `PolicyDenied` with a docs/34 pointer when
// `ST_ENABLE_COBB_AP_CIPHER=OFF` (the default). With the flag ON, the
// wrapper functions are real; the cipher functions return
// `NotImplemented` until the Blowfish primitive lands.

#ifndef ST_DEVICES_AP3_OTA_CIPHER_HPP
#define ST_DEVICES_AP3_OTA_CIPHER_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace st::devices::ap3::cipher {

// Header byte counts — the .img wrapper layout per spec §14.
inline constexpr std::size_t kOtaImgHeaderBytes = 6U;   // 4 base_ctr + 2 unknown
inline constexpr std::size_t kOtaImgTrailerBytes = 32U; // signature / HMAC

struct OtaImgWrapper {
    std::uint32_t base_ctr{0};        // first 4 bytes BE
    std::uint16_t header_word{0};     // bytes [4..5] BE — semantics TBD
    std::vector<std::uint8_t> body;   // encrypted Blowfish-CTR-double body
    std::vector<std::uint8_t> trailer; // last 32 bytes — exact byte echo
};

// Parse the .img wrapper layout. Verifies the input is at least
// `kOtaImgHeaderBytes + kOtaImgTrailerBytes` long and pulls out the
// header / body / trailer slices. **Wrapper-only** — does NOT decrypt
// the body. The body still needs `blowfish_ctr_double_decrypt` (which
// is Tier-A NotImplemented today).
[[nodiscard]] Result<OtaImgWrapper>
parse_ota_img(std::span<std::uint8_t const> img_bytes);

// Inverse of parse_ota_img — splice the wrapper back into the .img
// byte sequence. Same posture: wrapper-only, does NOT re-encrypt the
// body. Body is taken as-is from `wrapper.body`.
[[nodiscard]] std::vector<std::uint8_t>
build_ota_img(OtaImgWrapper const &wrapper);

// Decrypt one .img body via Blowfish-CTR-double. Tier-A
// NotImplemented today; needs a vendored public-domain Blowfish
// reference primitive (~300 lines C). When implemented, the
// decryption takes `base_ctr` from the wrapper as the per-file
// counter and applies the cipher twice.
[[nodiscard]] Result<std::vector<std::uint8_t>>
blowfish_ctr_double_decrypt(std::span<std::uint8_t const> ciphertext,
                             std::uint32_t base_ctr);

[[nodiscard]] Result<std::vector<std::uint8_t>>
blowfish_ctr_double_encrypt(std::span<std::uint8_t const> plaintext,
                             std::uint32_t base_ctr);

// High-level wrapper: parse the .img envelope, decrypt the body,
// return the plaintext + the wrapper header bytes (for round-trip).
// Returns PolicyDenied when the cipher tier is OFF; returns
// NotImplemented while the Blowfish primitive is still Tier-A.
[[nodiscard]] Result<std::vector<std::uint8_t>>
decrypt_ota_img(std::span<std::uint8_t const> img_bytes);

} // namespace st::devices::ap3::cipher

#endif // ST_DEVICES_AP3_OTA_CIPHER_HPP
