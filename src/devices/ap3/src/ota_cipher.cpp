// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/ota_cipher.hpp"

#include "st/core/error.hpp"

namespace st::devices::ap3::cipher {

#if defined(ST_AP3_HAVE_CIPHER)
constexpr char const *kCipherOffMessage =
    "ota_cipher: Blowfish primitive not yet vendored — see "
    "docs/34-cobb-ap-as-tune-vault.md §Session 3.";
#else
constexpr char const *kGateOffMessage =
    "ota_cipher: build flag ST_ENABLE_COBB_AP_CIPHER is OFF "
    "(default). The .img cipher chain is gated alongside the .ptm "
    "chain — see docs/34.";
#endif

Result<OtaImgWrapper>
parse_ota_img(std::span<std::uint8_t const> img_bytes) {
    if (img_bytes.size() < kOtaImgHeaderBytes + kOtaImgTrailerBytes) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "ota_cipher::parse_ota_img: input shorter than "
                           "minimum envelope (6-byte header + 32-byte trailer)");
    }
    OtaImgWrapper out;
    // u32 BE base_ctr in the first 4 bytes.
    out.base_ctr = (static_cast<std::uint32_t>(img_bytes[0]) << 24) |
                   (static_cast<std::uint32_t>(img_bytes[1]) << 16) |
                   (static_cast<std::uint32_t>(img_bytes[2]) << 8) |
                   static_cast<std::uint32_t>(img_bytes[3]);
    // u16 BE header word at [4..5]. Semantics TBD.
    out.header_word = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(img_bytes[4]) << 8) | img_bytes[5]);
    // Body = everything between header and the trailing 32 bytes.
    std::size_t const body_len =
        img_bytes.size() - kOtaImgHeaderBytes - kOtaImgTrailerBytes;
    for (std::size_t i = 0; i < body_len; ++i) {
        out.body.push_back(img_bytes[kOtaImgHeaderBytes + i]);
    }
    for (std::size_t i = 0; i < kOtaImgTrailerBytes; ++i) {
        out.trailer.push_back(img_bytes[img_bytes.size() - kOtaImgTrailerBytes + i]);
    }
    return out;
}

std::vector<std::uint8_t> build_ota_img(OtaImgWrapper const &wrapper) {
    std::vector<std::uint8_t> out;
    // u32 BE base_ctr
    out.push_back(static_cast<std::uint8_t>((wrapper.base_ctr >> 24) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((wrapper.base_ctr >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((wrapper.base_ctr >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(wrapper.base_ctr & 0xFFU));
    // u16 BE header word
    out.push_back(static_cast<std::uint8_t>((wrapper.header_word >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(wrapper.header_word & 0xFFU));
    for (auto const b : wrapper.body) {
        out.push_back(b);
    }
    for (auto const b : wrapper.trailer) {
        out.push_back(b);
    }
    return out;
}

#if defined(ST_AP3_HAVE_CIPHER)

Result<std::vector<std::uint8_t>>
blowfish_ctr_double_decrypt(std::span<std::uint8_t const> /*ciphertext*/,
                              std::uint32_t /*base_ctr*/) {
    return st::failure(st::ErrorCode::NotImplemented, kCipherOffMessage);
}

Result<std::vector<std::uint8_t>>
blowfish_ctr_double_encrypt(std::span<std::uint8_t const> /*plaintext*/,
                              std::uint32_t /*base_ctr*/) {
    return st::failure(st::ErrorCode::NotImplemented, kCipherOffMessage);
}

Result<std::vector<std::uint8_t>>
decrypt_ota_img(std::span<std::uint8_t const> /*img_bytes*/) {
    return st::failure(st::ErrorCode::NotImplemented, kCipherOffMessage);
}

#else

Result<std::vector<std::uint8_t>>
blowfish_ctr_double_decrypt(std::span<std::uint8_t const> /*ciphertext*/,
                              std::uint32_t /*base_ctr*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Result<std::vector<std::uint8_t>>
blowfish_ctr_double_encrypt(std::span<std::uint8_t const> /*plaintext*/,
                              std::uint32_t /*base_ctr*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

Result<std::vector<std::uint8_t>>
decrypt_ota_img(std::span<std::uint8_t const> /*img_bytes*/) {
    return st::failure(st::ErrorCode::PolicyDenied, kGateOffMessage);
}

#endif // ST_AP3_HAVE_CIPHER

} // namespace st::devices::ap3::cipher
