// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ets/aes256_ctr.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#ifdef ST_ETS_HAVE_CIPHER

// tiny-AES-c (public domain, see THIRD_PARTY_NOTICES.md). Pulled in
// under `extern "C"` because the source is plain C and we don't want
// C++ name mangling on its symbols.
extern "C" {
#include "aes.h"
}

namespace st::devices::ets::cipher {

namespace {

// .ptm format-version dispatcher per
// findings/re-2026-06-14/ptm_format_dispatcher.md +
// libMapFile.so `Maps::MapFile::GetPrivDataEncrypter()` @ 0x8194.
//
// v220 / v230 / v240 all share the MapInternalEn7er AES-256 CTR
// wrapper (same custom CTR construction in xcrypt below) — only the
// 32-byte key differs. v230 (`MapInternalKey3`) is the current
// shipping format and was the first one we landed; the v220 / v240
// keys are recovered from the same RE pass and shipped here for
// forward compat. v210 (`MapInternalKey`) and v9995 (`D3IK`) are
// deliberately not loaded — v210's published key bytes don't
// round-trip cleanly to 32 chars in our reference doc and v9995
// uses a different wrapper (`D3En7`) altogether.
inline constexpr std::array<std::uint8_t, 32> kMapInternalKey2{
    'M', 'I', 'I', 'E', 'o', 'Q', 'I', 'B',
    'A', 'A', 'K', 'C', 'A', 'Q', 'E', 'A',
    't', 'z', 'N', '3', 'M', 'u', 'a', 'y',
    'Z', 'g', 'C', 'D', 'l', 'o', '9', 'Q',
};
inline constexpr std::array<std::uint8_t, 32> kMapInternalKey3{
    'b', 'J', 'T', 'c', 'c', 'I', '%', '8',
    '7', '8', 'c', 'P', 's', '%', '2', '$',
    'T', 'f', '8', 'E', 'X', 'd', 'z', 'P',
    '2', '!', 'c', 'R', 'U', 'Z', 'w', '&',
};
inline constexpr std::array<std::uint8_t, 32> kMapInternalKey4{
    'w', 'r', 'q', 'j', '5', 'G', 'W', 'u',
    'a', 'v', 's', '6', 'Q', '5', 'J', 'g',
    'j', 'u', 'D', 'J', 'M', 'V', 'n', 'f',
    'd', 'v', 'P', 'g', 'q', 'e', 'M', 'T',
};
// Backwards-compat alias for code that still says "kAesKey" — the
// v230 key is the default for both encrypt and the legacy 1-arg
// decrypt overload. Existing call sites stay one-line.
inline constexpr auto const &kAesKey = kMapInternalKey3;

// Custom CTR construction per the OEM cipher dispatcher entry in libMapFile.so.
// 16 KB outer chunks; each chunk has its own outer_nonce derived from
// the file-level nonce plus the chunk's byte offset. Within each chunk,
// per-block counter = outer_nonce + block_idx, AES input is the 4-byte
// counter (BE) repeated 4 times, and the AES-ECB keystream is XOR'd
// with the chunk plaintext/ciphertext.
constexpr std::size_t kChunkSize = 0x4000; // 16 KB

std::vector<std::uint8_t>
xcrypt(std::span<std::uint8_t const> input, std::uint32_t nonce,
        std::span<std::uint8_t const> key) {
    if (input.empty()) {
        return {};
    }
    if (key.size() != 32) {
        return {};
    }
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key.data());
    std::vector<std::uint8_t> out(input.begin(), input.end());
    std::size_t pos = 0;
    while (pos < out.size()) {
        std::uint32_t const outer_nonce =
            (nonce + static_cast<std::uint32_t>(pos)) & 0xFFFFFFFFU;
        // Build a keystream of up to one chunk (1024 × 16 = 16384 B).
        std::size_t const remain = out.size() - pos;
        std::size_t const n = std::min(kChunkSize, remain);
        std::size_t const blocks = (n + 15) / 16;
        std::array<std::uint8_t, 16> aes_block{};
        for (std::size_t b = 0; b < blocks; ++b) {
            std::uint32_t const counter =
                (outer_nonce + static_cast<std::uint32_t>(b)) & 0xFFFFFFFFU;
            // counter (4 bytes BE) repeated 4 times.
            for (std::size_t i = 0; i < 4; ++i) {
                aes_block[i * 4 + 0] = static_cast<std::uint8_t>((counter >> 24) & 0xFFU);
                aes_block[i * 4 + 1] = static_cast<std::uint8_t>((counter >> 16) & 0xFFU);
                aes_block[i * 4 + 2] = static_cast<std::uint8_t>((counter >> 8) & 0xFFU);
                aes_block[i * 4 + 3] = static_cast<std::uint8_t>(counter & 0xFFU);
            }
            AES_ECB_encrypt(&ctx, aes_block.data());
            // XOR keystream block with the corresponding ciphertext bytes.
            std::size_t const block_off = pos + b * 16;
            std::size_t const block_end = std::min(block_off + 16, out.size());
            for (std::size_t j = block_off; j < block_end; ++j) {
                out[j] = static_cast<std::uint8_t>(out[j] ^ aes_block[j - block_off]);
            }
        }
        pos += n;
    }
    return out;
}

} // namespace

Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> ciphertext, std::uint32_t nonce) {
    return xcrypt(ciphertext, nonce, kMapInternalKey3);
}

Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> ciphertext, std::uint32_t nonce,
                    std::span<std::uint8_t const> aes_key) {
    if (aes_key.size() != 32) {
        return st::failure(st::ErrorCode::InvalidArgument,
                            "ap3: aes256_ctr_decrypt key must be exactly 32 bytes");
    }
    return xcrypt(ciphertext, nonce, aes_key);
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> plaintext, std::uint32_t nonce) {
    return xcrypt(plaintext, nonce, kMapInternalKey3);
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> plaintext, std::uint32_t nonce,
                    std::span<std::uint8_t const> aes_key) {
    if (aes_key.size() != 32) {
        return st::failure(st::ErrorCode::InvalidArgument,
                            "ap3: aes256_ctr_encrypt key must be exactly 32 bytes");
    }
    return xcrypt(plaintext, nonce, aes_key);
}

std::span<std::uint8_t const>
ptm_key_for_version(std::uint32_t version) noexcept {
    switch (version) {
    case 220U:
        return std::span<std::uint8_t const>{kMapInternalKey2};
    case 230U:
        return std::span<std::uint8_t const>{kMapInternalKey3};
    case 240U:
        return std::span<std::uint8_t const>{kMapInternalKey4};
    default:
        return {};
    }
}

} // namespace st::devices::ets::cipher

#else // !ST_ETS_HAVE_CIPHER

namespace st::devices::ets::cipher {

namespace {

st::Error policy_denied() {
    return st::Error{st::ErrorCode::PolicyDenied,
                     "ap3: AES-256-CTR layer 3 is excluded from this build. "
                     "Configure with -DST_ENABLE_COBB_AP_CIPHER=ON and pass "
                     "--enable-cobb-ap-cipher at runtime. See "
                     "docs/34-cobb-ap-as-tune-vault.md."};
}

} // namespace

Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> /*ciphertext*/, std::uint32_t /*nonce*/) {
    return st::failure(policy_denied());
}

Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> /*ciphertext*/, std::uint32_t /*nonce*/,
                    std::span<std::uint8_t const> /*aes_key*/) {
    return st::failure(policy_denied());
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> /*plaintext*/, std::uint32_t /*nonce*/) {
    return st::failure(policy_denied());
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> /*plaintext*/, std::uint32_t /*nonce*/,
                    std::span<std::uint8_t const> /*aes_key*/) {
    return st::failure(policy_denied());
}

std::span<std::uint8_t const>
ptm_key_for_version(std::uint32_t /*version*/) noexcept {
    return {};
}

} // namespace st::devices::ets::cipher

#endif // ST_ETS_HAVE_CIPHER
