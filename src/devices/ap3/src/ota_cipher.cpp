// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/ota_cipher.hpp"

#include "st/core/error.hpp"

#if defined(ST_AP3_HAVE_CIPHER)
#include "st/devices/ap3/blowfish_ecb.hpp"
#endif

namespace st::devices::ap3::cipher {

#if !defined(ST_AP3_HAVE_CIPHER)
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

namespace {

// COBB AP firmware key per analyst RE (firmware_decrypt.py + the
// VA 0xb01024 literal pool in APManager.exe + libap.so). 32-byte ASCII,
// not null-terminated.
constexpr std::array<std::uint8_t, 32> kBlowfishKey = {
    '(',  '\'', 'R',  'Y',  'q',  '{',  '&',  '@',
    'V',  'R',  'i',  'F',  'v',  'X',  '>',  ' ',
    'b',  '(',  '6',  '_',  'a',  '^',  '5',  'e',
    '^',  'M',  's',  'M',  'v',  'Q',  'U',  'H',
};

constexpr std::size_t kChunkSize = 0x200; // 512 bytes per CTR chunk

// XOR-stream the Blowfish keystream against `data` in place. Encrypt
// and decrypt are the same operation under XOR. Per the analyst's
// firmware_decrypt.py:
//
//   for chunk_idx in 0..n:
//     chunk_base = base_ctr + chunk_idx * 512   (note: × 512 not × 32)
//     for block in 0..32:                       (16 bytes per block)
//       counter = chunk_base + block            (advance by 1)
//       8-byte BF input = counter (4 BE) doubled  ↓
//       8-byte BF output = BF-ECB-encrypt(input)
//       16-byte keystream = BF output doubled
//       XOR with 16 bytes of data
//
// Counter values 32..511 are SKIPPED between adjacent chunks — this is
// the "custom CTR" aspect that distinguishes it from a standard CTR
// mode (which would advance the counter by 1 per 16-byte block, not 16
// per 512-byte chunk + a 480-block gap).
void xor_keystream(std::span<std::uint8_t> data, std::uint32_t base_ctr) {
    blowfish::Context ctx;
    blowfish::bf_set_key(ctx, kBlowfishKey);

    std::size_t pos = 0;
    std::size_t chunk_idx = 0;
    while (pos < data.size()) {
        std::uint32_t const chunk_base =
            static_cast<std::uint32_t>((base_ctr + chunk_idx * kChunkSize) & 0xFFFFFFFFU);
        std::size_t const chunk_end =
            std::min(pos + kChunkSize, data.size());
        std::size_t off = 0;
        while (pos + off < chunk_end) {
            std::uint32_t const ctr_val =
                static_cast<std::uint32_t>((chunk_base + (off / 16)) & 0xFFFFFFFFU);
            // 8-byte BF input: counter (4 BE) doubled.
            std::array<std::uint8_t, 8> bf_in{
                static_cast<std::uint8_t>((ctr_val >> 24) & 0xFFU),
                static_cast<std::uint8_t>((ctr_val >> 16) & 0xFFU),
                static_cast<std::uint8_t>((ctr_val >> 8) & 0xFFU),
                static_cast<std::uint8_t>(ctr_val & 0xFFU),
                static_cast<std::uint8_t>((ctr_val >> 24) & 0xFFU),
                static_cast<std::uint8_t>((ctr_val >> 16) & 0xFFU),
                static_cast<std::uint8_t>((ctr_val >> 8) & 0xFFU),
                static_cast<std::uint8_t>(ctr_val & 0xFFU),
            };
            blowfish::bf_encrypt_block(ctx, std::span<std::uint8_t, 8>{bf_in});
            // 16-byte keystream = BF output doubled. XOR with up to 16
            // bytes of data (final block may be short).
            std::size_t const block_n =
                std::min<std::size_t>(16, chunk_end - (pos + off));
            for (std::size_t i = 0; i < block_n; ++i) {
                data[pos + off + i] ^= bf_in[i % 8];
            }
            off += 16;
        }
        pos = chunk_end;
        ++chunk_idx;
    }
}

} // namespace

Result<std::vector<std::uint8_t>>
blowfish_ctr_double_decrypt(std::span<std::uint8_t const> ciphertext,
                              std::uint32_t base_ctr) {
    std::vector<std::uint8_t> out{ciphertext.begin(), ciphertext.end()};
    xor_keystream(out, base_ctr);
    return out;
}

Result<std::vector<std::uint8_t>>
blowfish_ctr_double_encrypt(std::span<std::uint8_t const> plaintext,
                              std::uint32_t base_ctr) {
    // Symmetric — XOR stream is its own inverse.
    std::vector<std::uint8_t> out{plaintext.begin(), plaintext.end()};
    xor_keystream(out, base_ctr);
    return out;
}

Result<std::vector<std::uint8_t>>
decrypt_ota_img(std::span<std::uint8_t const> img_bytes) {
    auto wrapper = parse_ota_img(img_bytes);
    if (!wrapper.has_value()) {
        return st::failure(std::move(wrapper).error());
    }
    auto plaintext = blowfish_ctr_double_decrypt(wrapper->body, wrapper->base_ctr);
    if (!plaintext.has_value()) {
        return st::failure(std::move(plaintext).error());
    }
    return plaintext;
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
