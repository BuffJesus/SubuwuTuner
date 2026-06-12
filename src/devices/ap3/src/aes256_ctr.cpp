// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/aes256_ctr.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#ifdef ST_AP3_HAVE_CIPHER

// tiny-AES-c (public domain, see THIRD_PARTY_NOTICES.md). Pulled in
// under `extern "C"` because the source is plain C and we don't want
// C++ name mangling on its symbols.
extern "C" {
#include "aes.h"
}

namespace st::devices::ap3::cipher {

namespace {

// Spec §13 layer 3: AES-256 key is the 32-byte ASCII string
// `bJTccI%878cPs%2$Tf8EXdzP2!cRUZw&` as it appears in
// `libMapFile.so` rodata. Stored as a byte array so the compiler
// can fold it into rodata without runtime initialization.
inline constexpr std::array<std::uint8_t, 32> kAesKey{
    'b', 'J', 'T', 'c', 'c', 'I', '%', '8',
    '7', '8', 'c', 'P', 's', '%', '2', '$',
    'T', 'f', '8', 'E', 'X', 'd', 'z', 'P',
    '2', '!', 'c', 'R', 'U', 'Z', 'w', '&',
};

// Custom CTR construction per pkg::ExrRil::en_de in libMapFile.so.
// 16 KB outer chunks; each chunk has its own outer_nonce derived from
// the file-level nonce plus the chunk's byte offset. Within each chunk,
// per-block counter = outer_nonce + block_idx, AES input is the 4-byte
// counter (BE) repeated 4 times, and the AES-ECB keystream is XOR'd
// with the chunk plaintext/ciphertext.
constexpr std::size_t kChunkSize = 0x4000; // 16 KB

std::vector<std::uint8_t> xcrypt(std::span<std::uint8_t const> input, std::uint32_t nonce) {
    if (input.empty()) {
        return {};
    }
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, kAesKey.data());
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
    return xcrypt(ciphertext, nonce);
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> plaintext, std::uint32_t nonce) {
    return xcrypt(plaintext, nonce);
}

} // namespace st::devices::ap3::cipher

#else // !ST_AP3_HAVE_CIPHER

namespace st::devices::ap3::cipher {

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
aes256_ctr_encrypt(std::span<std::uint8_t const> /*plaintext*/, std::uint32_t /*nonce*/) {
    return st::failure(policy_denied());
}

} // namespace st::devices::ap3::cipher

#endif // ST_AP3_HAVE_CIPHER
