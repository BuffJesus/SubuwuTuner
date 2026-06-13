// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// .ptm cipher chain — Session 1 of the Tier 3 implementation. See
// ptm_cipher.hpp for the layer map. Build-flag gated: when
// ST_ETS_HAVE_CIPHER is undefined every API returns PolicyDenied.

#include "st/devices/ets/ptm_cipher.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#ifdef ST_ETS_HAVE_CIPHER
#include "st/devices/ets/aes256_ctr.hpp"
#include "st/devices/ets/bzip2_decompress.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::devices::ets::cipher {

#ifdef ST_ETS_HAVE_CIPHER

namespace {

// ---------------------------------------------------------------------------
// XTEA primitives (per spec §13 layer 1)
//
// Standard XTEA with the RFC delta `0x9E3779B9`, 32 rounds. Block is
// two big-endian u32. CBC mode is built on top below.
// ---------------------------------------------------------------------------

inline constexpr std::array<std::uint32_t, 4> kXteaKey{
    0x374C2D3CU, 0x67255F4BU, 0x2A6B596DU, 0x27675B40U
};

inline constexpr std::uint32_t kXteaDelta = 0x9E3779B9U;
inline constexpr unsigned kXteaRounds = 32U;
inline constexpr std::size_t kXteaBlockSize = 8U;

void xtea_encrypt_block(std::uint32_t &v0, std::uint32_t &v1,
                        std::array<std::uint32_t, 4> const &key) noexcept {
    std::uint32_t sum = 0;
    for (unsigned i = 0; i < kXteaRounds; ++i) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3U]);
        sum += kXteaDelta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3U]);
    }
}

void xtea_decrypt_block(std::uint32_t &v0, std::uint32_t &v1,
                        std::array<std::uint32_t, 4> const &key) noexcept {
    std::uint32_t sum = kXteaDelta * kXteaRounds;
    for (unsigned i = 0; i < kXteaRounds; ++i) {
        v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3U]);
        sum -= kXteaDelta;
        v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3U]);
    }
}

// Per-half IV split per spec §13: prev_c0 = seed, prev_c1 = seed XOR
// 0x5AA5A55A. Non-standard CBC convention but consistent across every
// reference sample. Constant is verbatim per the walkthrough.
inline constexpr std::uint32_t kIvHalfXor = 0x5AA5A55AU;

// Blocks on the wire are little-endian u32 pairs (matches what the
// analyst's Python toolkit produced; do NOT use big-endian here).
std::uint32_t read_le32(std::span<std::uint8_t const> bytes, std::size_t off) noexcept {
    return static_cast<std::uint32_t>(bytes[off]) |
           (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[off + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
}

void write_le32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFU));
}

} // namespace

namespace {

// Block-only primitive used by the file-level wrappers below. Not
// exposed in the public header: callers always go through the
// trailer-aware functions so they can't accidentally hand the AP a
// shape mismatch (which dazes the firmware per spec §4.2).
Result<std::vector<std::uint8_t>>
xtea_cbc_decrypt_blocks(std::span<std::uint8_t const> ciphertext, std::uint32_t seed) {
    std::vector<std::uint8_t> plaintext;
    plaintext.reserve(ciphertext.size());

    std::uint32_t prev_c0 = seed;
    std::uint32_t prev_c1 = seed ^ kIvHalfXor;

    for (std::size_t off = 0; off < ciphertext.size(); off += kXteaBlockSize) {
        std::uint32_t const ct0 = read_le32(ciphertext, off);
        std::uint32_t const ct1 = read_le32(ciphertext, off + 4);
        std::uint32_t v0 = ct0;
        std::uint32_t v1 = ct1;
        xtea_decrypt_block(v0, v1, kXteaKey);
        write_le32(plaintext, v0 ^ prev_c0);
        write_le32(plaintext, v1 ^ prev_c1);
        prev_c0 = ct0;
        prev_c1 = ct1;
    }
    return plaintext;
}

void xtea_cbc_encrypt_blocks_in_place(std::vector<std::uint8_t> &padded,
                                      std::uint32_t seed,
                                      std::vector<std::uint8_t> &out) {
    out.reserve(padded.size());
    std::uint32_t prev_c0 = seed;
    std::uint32_t prev_c1 = seed ^ kIvHalfXor;
    for (std::size_t off = 0; off < padded.size(); off += kXteaBlockSize) {
        std::uint32_t p0 = read_le32(padded, off) ^ prev_c0;
        std::uint32_t p1 = read_le32(padded, off + 4) ^ prev_c1;
        xtea_encrypt_block(p0, p1, kXteaKey);
        write_le32(out, p0);
        write_le32(out, p1);
        prev_c0 = p0;
        prev_c1 = p1;
    }
}

} // namespace

Result<std::vector<std::uint8_t>>
xtea_cbc_decrypt(std::span<std::uint8_t const> ptm_bytes) {
    // 5-byte trailer per spec §13: [pad_count: u8][seed: u32 BE].
    constexpr std::size_t kTrailerSize = 5U;
    if (ptm_bytes.size() < kTrailerSize + kXteaBlockSize) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 xtea_cbc_decrypt: input shorter than 8-byte block + 5-byte trailer");
    }
    std::size_t const body_size = ptm_bytes.size() - kTrailerSize;
    if (body_size % kXteaBlockSize != 0) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 xtea_cbc_decrypt: ciphertext body length not a multiple of 8");
    }
    std::uint8_t const pad_count = ptm_bytes[body_size];
    if (pad_count >= kXteaBlockSize) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 xtea_cbc_decrypt: pad_count " + std::to_string(pad_count) +
                               " out of range [0,7]");
    }
    std::uint32_t const seed =
        (static_cast<std::uint32_t>(ptm_bytes[body_size + 1]) << 24) |
        (static_cast<std::uint32_t>(ptm_bytes[body_size + 2]) << 16) |
        (static_cast<std::uint32_t>(ptm_bytes[body_size + 3]) << 8) |
        static_cast<std::uint32_t>(ptm_bytes[body_size + 4]);

    auto plaintext = xtea_cbc_decrypt_blocks(ptm_bytes.subspan(0, body_size), seed);
    if (!plaintext.has_value()) {
        return st::failure(std::move(plaintext).error());
    }
    if (pad_count > 0) {
        plaintext->resize(plaintext->size() - pad_count);
    }
    return plaintext;
}

Result<std::vector<std::uint8_t>>
xtea_cbc_encrypt(std::span<std::uint8_t const> plaintext, std::uint32_t seed) {
    std::uint8_t const pad_count =
        static_cast<std::uint8_t>((kXteaBlockSize - (plaintext.size() % kXteaBlockSize)) %
                                  kXteaBlockSize);
    std::vector<std::uint8_t> padded(plaintext.begin(), plaintext.end());
    padded.resize(plaintext.size() + pad_count, 0);

    std::vector<std::uint8_t> out;
    out.reserve(padded.size() + 5);
    xtea_cbc_encrypt_blocks_in_place(padded, seed, out);
    out.push_back(pad_count);
    out.push_back(static_cast<std::uint8_t>((seed >> 24) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((seed >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((seed >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(seed & 0xFFU));
    return out;
}

// ---------------------------------------------------------------------------
// Base64 (RFC 4648 standard alphabet)
// ---------------------------------------------------------------------------

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr int decode_byte(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return 26 + (c - 'a');
    if (c >= '0' && c <= '9')
        return 52 + (c - '0');
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

} // namespace

std::string base64_encode(std::span<std::uint8_t const> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        std::uint32_t const triplet = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                      (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                      static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kBase64Alphabet[(triplet >> 18) & 0x3FU]);
        out.push_back(kBase64Alphabet[(triplet >> 12) & 0x3FU]);
        out.push_back(kBase64Alphabet[(triplet >> 6) & 0x3FU]);
        out.push_back(kBase64Alphabet[triplet & 0x3FU]);
        i += 3;
    }
    if (i < bytes.size()) {
        std::uint32_t triplet = static_cast<std::uint32_t>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            triplet |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
        }
        out.push_back(kBase64Alphabet[(triplet >> 18) & 0x3FU]);
        out.push_back(kBase64Alphabet[(triplet >> 12) & 0x3FU]);
        if (i + 1 < bytes.size()) {
            out.push_back(kBase64Alphabet[(triplet >> 6) & 0x3FU]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

Result<std::vector<std::uint8_t>> base64_decode(std::string_view encoded) {
    std::vector<std::uint8_t> out;
    out.reserve(encoded.size() * 3 / 4);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char raw : encoded) {
        unsigned char const c = static_cast<unsigned char>(raw);
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            continue;
        }
        if (c == '=') {
            // Padding marks end-of-stream; ignore the remaining bits.
            break;
        }
        int const v = decode_byte(c);
        if (v < 0) {
            return st::failure(st::ErrorCode::ParseError,
                               "ap3 base64_decode: invalid character");
        }
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFFU));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Outer XML envelope: extract <encData>...</encData>
// ---------------------------------------------------------------------------

Result<std::string_view> extract_enc_data(std::string_view outer_xml) {
    constexpr std::string_view kOpen{"<encData>"};
    constexpr std::string_view kClose{"</encData>"};
    auto const open_pos = outer_xml.find(kOpen);
    if (open_pos == std::string_view::npos) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 extract_enc_data: <encData> element not found");
    }
    auto const body_start = open_pos + kOpen.size();
    auto const close_pos = outer_xml.find(kClose, body_start);
    if (close_pos == std::string_view::npos) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3 extract_enc_data: </encData> not found after open tag");
    }
    return outer_xml.substr(body_start, close_pos - body_start);
}

// ---------------------------------------------------------------------------
// Session-1 public API
// ---------------------------------------------------------------------------

Result<std::string> decrypt_ptm_outer(std::span<std::uint8_t const> ptm_bytes) {
    // Thin wrapper that pipes through the file-level decrypt and
    // re-views the bytes as a UTF-8 string. The strict trailer parse
    // + pad strip + seed extraction all live in xtea_cbc_decrypt.
    auto plaintext = xtea_cbc_decrypt(ptm_bytes);
    if (!plaintext.has_value()) {
        return st::failure(std::move(plaintext).error());
    }
    return std::string(plaintext->begin(), plaintext->end());
}

Result<PtmContents>
decrypt_ptm(std::span<std::uint8_t const> ptm_bytes) {
    // Full 4-layer chain per spec §13 + the Session 2 integration
    // guide:
    //   layer 1 — XTEA-CBC outer  (decrypt_ptm_outer)
    //   layer 2 — XML envelope    (extract_enc_data + base64_decode)
    //   layer 3 — AES-256-CTR     (aes256_ctr_decrypt)
    //   layer 4 — bzip2 inflate   (bzip2_decompress)
    auto outer_xml = decrypt_ptm_outer(ptm_bytes);
    if (!outer_xml.has_value()) {
        return st::failure(std::move(outer_xml).error());
    }
    auto b64 = extract_enc_data(*outer_xml);
    if (!b64.has_value()) {
        return st::failure(std::move(b64).error());
    }
    auto inner_ct = base64_decode(*b64);
    if (!inner_ct.has_value()) {
        return st::failure(std::move(inner_ct).error());
    }
    // The base64-decoded encData blob carries the per-file AES nonce
    // in its last 4 bytes (BE u32). Strip those before feeding the
    // body to the custom CTR construction. Per `pkg::ExrRil::en_de`
    // and the reference Python tool — verified live 2026-06-12.
    if (inner_ct->size() < 4) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3: encData blob too short to carry a 4-byte nonce");
    }
    std::uint32_t const aes_nonce =
        (static_cast<std::uint32_t>((*inner_ct)[inner_ct->size() - 4]) << 24) |
        (static_cast<std::uint32_t>((*inner_ct)[inner_ct->size() - 3]) << 16) |
        (static_cast<std::uint32_t>((*inner_ct)[inner_ct->size() - 2]) << 8) |
        static_cast<std::uint32_t>((*inner_ct)[inner_ct->size() - 1]);
    inner_ct->resize(inner_ct->size() - 4);
    auto bz_blob = aes256_ctr_decrypt(*inner_ct, aes_nonce);
    if (!bz_blob.has_value()) {
        return st::failure(std::move(bz_blob).error());
    }
    auto private_xml = bzip2_decompress(*bz_blob);
    if (!private_xml.has_value()) {
        return st::failure(std::move(private_xml).error());
    }

    // Strip the `<encData>...</encData>` element from the outer XML
    // so the caller's outer_metadata_xml view contains just the
    // envelope (vendor / vehicle / lock state / etc.) without the
    // large base64 blob duplicating what's in private_data_xml.
    std::string outer_meta = *outer_xml;
    constexpr std::string_view kOpen{"<encData>"};
    constexpr std::string_view kClose{"</encData>"};
    auto open_pos = outer_meta.find(kOpen);
    if (open_pos != std::string::npos) {
        auto close_pos = outer_meta.find(kClose, open_pos);
        if (close_pos != std::string::npos) {
            outer_meta.erase(open_pos, close_pos + kClose.size() - open_pos);
        }
    }

    PtmContents out;
    out.private_data_xml = std::move(*private_xml);
    out.outer_metadata_xml = std::move(outer_meta);
    return out;
}

Result<std::vector<std::uint8_t>>
encrypt_ptm(std::string_view private_data_xml,
            std::string_view outer_metadata_xml,
            std::uint32_t seed,
            std::uint32_t nonce) {
    // Reverse of decrypt_ptm. Pipeline: bzip2 compress → AES-256-CTR
    // encrypt → base64 encode → inject into outer XML → XTEA-CBC
    // encrypt + 5-byte trailer.
    //
    // The `nonce` parameter is preserved in the signature for forward
    // compatibility but is currently unused — every reference `.ptm`
    // sample uses the spec §13 constant nonce 0x00000020, and the
    // aes256_ctr_encrypt implementation hardcodes that IV per the
    // walkthrough §3 "Gotchas" guidance. If a future firmware
    // revision rotates the nonce, plumb it through here.
    std::span<std::uint8_t const> pt_span{
        reinterpret_cast<std::uint8_t const *>(private_data_xml.data()),
        private_data_xml.size()};
    auto compressed = bzip2_compress(pt_span);
    if (!compressed.has_value()) {
        return st::failure(std::move(compressed).error());
    }
    // AES-256-CTR with the per-file nonce. The `nonce` parameter is
    // the same value that gets appended (BE u32) at the end of the
    // encData blob so decrypt_ptm can recover it. Every observed real
    // .ptm uses the spec §13 constant 0x00000020 here.
    auto encrypted_inner = aes256_ctr_encrypt(*compressed, nonce);
    if (!encrypted_inner.has_value()) {
        return st::failure(std::move(encrypted_inner).error());
    }
    // Append 4-byte BE nonce trailer to match the per-file format.
    encrypted_inner->push_back(static_cast<std::uint8_t>((nonce >> 24) & 0xFFU));
    encrypted_inner->push_back(static_cast<std::uint8_t>((nonce >> 16) & 0xFFU));
    encrypted_inner->push_back(static_cast<std::uint8_t>((nonce >> 8) & 0xFFU));
    encrypted_inner->push_back(static_cast<std::uint8_t>(nonce & 0xFFU));
    std::string const encdata_b64 = base64_encode(*encrypted_inner);

    // Inject `<encData>BASE64</encData>` into the outer XML. Heuristic:
    // insert before the last closing element so the encData is a
    // direct child of the document root. If the outer XML is empty,
    // wrap with a minimal envelope.
    std::string outer = std::string{outer_metadata_xml};
    std::string const encdata_element = "<encData>" + encdata_b64 + "</encData>";
    if (outer.empty()) {
        outer = "<PtmOuter>" + encdata_element + "</PtmOuter>";
    } else {
        // Insert before the last '</' which closes the root element.
        auto const last_close = outer.rfind("</");
        if (last_close == std::string::npos) {
            // No closing tag — just append.
            outer += encdata_element;
        } else {
            outer.insert(last_close, encdata_element);
        }
    }

    std::span<std::uint8_t const> outer_span{
        reinterpret_cast<std::uint8_t const *>(outer.data()), outer.size()};
    return xtea_cbc_encrypt(outer_span, seed);
}

#else // !ST_ETS_HAVE_CIPHER

namespace {

st::Error make_policy_denied() {
    return st::Error{st::ErrorCode::PolicyDenied,
                     "ap3: .ptm cipher is excluded from this build. Configure with "
                     "-DST_ENABLE_COBB_AP_CIPHER=ON and pass --enable-cobb-ap-cipher at "
                     "runtime. See docs/34-cobb-ap-as-tune-vault.md."};
}

} // namespace

Result<std::vector<std::uint8_t>>
xtea_cbc_decrypt(std::span<std::uint8_t const> /*ptm_bytes*/) {
    return st::failure(make_policy_denied());
}

Result<std::vector<std::uint8_t>>
xtea_cbc_encrypt(std::span<std::uint8_t const> /*plaintext*/, std::uint32_t /*seed*/) {
    return st::failure(make_policy_denied());
}

Result<std::vector<std::uint8_t>> base64_decode(std::string_view /*encoded*/) {
    return st::failure(make_policy_denied());
}

std::string base64_encode(std::span<std::uint8_t const> /*bytes*/) {
    return {};
}

Result<std::string_view> extract_enc_data(std::string_view /*outer_xml*/) {
    return st::failure(make_policy_denied());
}

Result<std::string> decrypt_ptm_outer(std::span<std::uint8_t const> /*ptm_bytes*/) {
    return st::failure(make_policy_denied());
}

Result<PtmContents> decrypt_ptm(std::span<std::uint8_t const> /*ptm_bytes*/) {
    return st::failure(make_policy_denied());
}

Result<std::vector<std::uint8_t>>
encrypt_ptm(std::string_view /*private_data_xml*/,
            std::string_view /*outer_metadata_xml*/,
            std::uint32_t /*seed*/, std::uint32_t /*nonce*/) {
    return st::failure(make_policy_denied());
}

#endif // ST_ETS_HAVE_CIPHER

} // namespace st::devices::ets::cipher
