// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/aes256_ctr.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <array>
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

// Spec §13 layer 3: 16-byte IV is the 4-byte nonce 0x00000020
// (observed constant across every reference `.ptm` sample) padded
// with 12 trailing zero bytes. Per the walkthrough §3 "Gotchas":
// "the literal 16 bytes as written" — not a u128-LE counter
// representation of 0x20.
inline constexpr std::array<std::uint8_t, 16> kAesIv{
    0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

// Single implementation; both decrypt and encrypt routes call this.
// AES-CTR's keystream depends only on key + IV, so XOR'ing input
// against it recovers the other side regardless of direction.
std::vector<std::uint8_t> xcrypt(std::span<std::uint8_t const> input) {
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, kAesKey.data(), kAesIv.data());
    std::vector<std::uint8_t> out(input.begin(), input.end());
    if (!out.empty()) {
        AES_CTR_xcrypt_buffer(&ctx, out.data(),
                              static_cast<std::uint32_t>(out.size()));
    }
    return out;
}

} // namespace

Result<std::vector<std::uint8_t>>
aes256_ctr_decrypt(std::span<std::uint8_t const> ciphertext) {
    return xcrypt(ciphertext);
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> plaintext) {
    return xcrypt(plaintext);
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
aes256_ctr_decrypt(std::span<std::uint8_t const> /*ciphertext*/) {
    return st::failure(policy_denied());
}

Result<std::vector<std::uint8_t>>
aes256_ctr_encrypt(std::span<std::uint8_t const> /*plaintext*/) {
    return st::failure(policy_denied());
}

} // namespace st::devices::ap3::cipher

#endif // ST_AP3_HAVE_CIPHER
