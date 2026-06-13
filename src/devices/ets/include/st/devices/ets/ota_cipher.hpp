// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ets::cipher::ota — `.img` (COBB AccessPort OTA firmware
// image) cipher chain. Per spec §14 / `docs/34-cobb-ap-as-tune-vault.md`:
//
// The `.img` envelope is:
//   [0..3]       u32 BE base_ctr (per-file Blowfish counter)
//   [4..5]       u16 unknown header / version field
//   [6..N-32]    body bytes — Blowfish-CTR-double encrypted
//   [N-32..N]    32-byte trailer (signature?  HMAC?  un-RE'd)
//
// The body is encrypted with **single-pass Blowfish-ECB in a custom
// CTR mode** (the "double" in the function name is a holdover —
// see the function-level comment). Per the analyst's RE of
// APManager.exe's `StreamEncrypter::Process`:
//
//   * 512-byte chunks. 32 Blowfish blocks per chunk.
//   * Per 16-byte block: counter (4 BE) doubled → 8-byte BF input
//     → BF-ECB-encrypt with the 32-byte ASCII key → 8-byte output
//     doubled → 16-byte keystream → XOR with 16 bytes of data.
//   * Counter advances by 1 per 16-byte block within a chunk.
//   * **Between chunks** the counter jumps by 512, NOT 32 —
//     so counter values 32..511 are skipped per chunk transition.
//     `chunk_base = base_ctr + chunk_idx × 512`.
//
// The XOR-stream construction is its own inverse: encrypt and decrypt
// are the same call.
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

namespace st::devices::ets::cipher {

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

// Decrypt one .img body via the Blowfish-ECB-in-custom-CTR
// construction described in the file header. `base_ctr` comes from
// the wrapper as the per-file counter seed (the AP's per-firmware-
// build constant). The function name preserves "double" for binary-
// compat with the original Session-3 skeleton; the underlying
// construction is single-pass.
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

} // namespace st::devices::ets::cipher

#endif // ST_DEVICES_AP3_OTA_CIPHER_HPP
