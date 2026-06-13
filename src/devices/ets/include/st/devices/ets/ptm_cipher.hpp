// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::devices::ets::cipher — `.ptm` (COBB AccessPort tune-file) cipher
// chain. Per spec §13, the chain has four layers:
//
//   1. XTEA-CBC outer  (32 rounds, 8-byte block, 16-byte key, seed = last
//                       4 bytes BE of the .ptm file)
//   2. XML envelope    (extract <encData> base64 from the outer XML)
//   3. AES-256-CTR     (32-byte ASCII key, 4-byte nonce padded to 16 B IV)
//   4. bzip2-L4        (decompress → PrivateData XML with the patches)
//
// Distribution gating per spec §13.1 / docs/34: the implementation
// compiles only when `ST_ENABLE_COBB_AP_CIPHER=ON`. The default
// public build has every function return `PolicyDenied` with a
// pointer at docs/34.
//
// Implementation tiers (per
// specs/cobb-ap3-tier3-cipher-implementation-spec.md):
//   - Session 1 (LANDED): XTEA-CBC outer + base64 + outer envelope
//     parsing → `decrypt_ptm_outer` returns the outer XML with the
//     `<encData>` base64 still intact.
//   - Session 2 (NOT YET): AES-256-CTR + bzip2-L4 → full
//     `decrypt_ptm` + `encrypt_ptm` returning byte-identical .ptm.
//   - Session 3 (NOT YET): see `ota_cipher.hpp`.

#ifndef ST_DEVICES_AP3_PTM_CIPHER_HPP
#define ST_DEVICES_AP3_PTM_CIPHER_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::devices::ets::cipher {

struct EtmContents {
    // Layer-4 plaintext: the <PrivateData> XML body containing the
    // tune's <patch> elements. Empty until Session 2 lands AES + bzip2.
    std::string inner_xml;
    // Layer-1 plaintext: the outer XML envelope (vendor / vehicle /
    // lock-state / etc.) with the <encData> element scrubbed.
    std::string outer_xml;
};

// Full chain. Returns NotImplemented while only Session 1 has landed;
// returns PolicyDenied when the cipher tier isn't compiled in.
[[nodiscard]] Result<EtmContents>
decrypt_ptm(std::span<std::uint8_t const> ptm_bytes);

// Reverse of decrypt_ptm. NotImplemented until Session 2; PolicyDenied
// when the cipher tier is OFF.
//
// `seed` is the per-file XTEA IV (typically the last 4 bytes BE of the
// original encrypted file the caller is replacing — pass the existing
// seed when round-tripping a tune to preserve byte-identity).
// `nonce` is the inner AES-CTR 32-bit nonce (default 0x00000020 per
// spec §13 — every reference sample uses this constant).
[[nodiscard]] Result<std::vector<std::uint8_t>>
encrypt_ptm(std::string_view inner_xml,
            std::string_view outer_xml,
            std::uint32_t seed,
            std::uint32_t nonce = 0x00000020U);

// Decrypt only the outer XTEA-CBC layer + strip CBC padding. Returns
// the outer XML as a UTF-8 string (the `<encData>` element's base64
// body is still intact). Useful for tools that only want the per-file
// metadata (vendor / vehicle / lock state / file_hash) and don't need
// to introspect the inner patch list.
//
// Session 1 — implemented when the cipher tier is built.
[[nodiscard]] Result<std::string>
decrypt_ptm_outer(std::span<std::uint8_t const> ptm_bytes);

// ---------------------------------------------------------------------------
// Low-level primitives exposed for testability.
// ---------------------------------------------------------------------------

// XTEA-CBC decrypt. File-level — takes the WHOLE .ptm-style buffer
// (encrypted body + 5-byte trailer [pad_count: u8][seed: u32 BE]),
// parses the trailer, decrypts the body in CBC mode (LE u32 blocks,
// IV = (seed, seed ^ kIvHalfXor)), strips the trailing pad_count
// bytes, and returns the plaintext. Matches the analyst's reference
// API in `specs/cobb-ap3-tier3-cipher-walkthrough.md` §1 so the
// canonical fixtures under `fixtures/ap3/cipher/` pin byte-identical.
[[nodiscard]] Result<std::vector<std::uint8_t>>
xtea_cbc_decrypt(std::span<std::uint8_t const> ptm_bytes);

// XTEA-CBC encrypt. File-level — zero-pads plaintext to the next
// 8-byte boundary, encrypts under XTEA-CBC, appends the 5-byte trailer
// [pad_count: u8][seed: u32 BE]. The returned bytes are the full
// .ptm-style buffer suitable for `xtea_cbc_decrypt` round-trip.
[[nodiscard]] Result<std::vector<std::uint8_t>>
xtea_cbc_encrypt(std::span<std::uint8_t const> plaintext, std::uint32_t seed);

// Base64 (RFC 4648 standard alphabet) decode. Tolerates `=` padding,
// `\n` and `\r` whitespace.
[[nodiscard]] Result<std::vector<std::uint8_t>> base64_decode(std::string_view encoded);

// Base64 (RFC 4648 standard alphabet) encode. Does NOT insert line
// breaks — that's the caller's job if needed.
[[nodiscard]] std::string base64_encode(std::span<std::uint8_t const> bytes);

// Extract the body of `<encData>...</encData>` from the outer XML.
// Returns ParseError if the element isn't present.
[[nodiscard]] Result<std::string_view> extract_enc_data(std::string_view outer_xml);

} // namespace st::devices::ets::cipher

#endif // ST_DEVICES_AP3_PTM_CIPHER_HPP
