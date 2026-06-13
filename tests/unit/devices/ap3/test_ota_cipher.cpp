// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the .img OTA cipher wrapper parser + cipher gating.
// Wrapper is cipher-independent and always compiles. The Blowfish
// primitive is Tier-A NotImplemented until the public-domain
// reference is vendored — assertions for the cipher-call path
// distinguish PolicyDenied (build flag OFF) from NotImplemented
// (build flag ON, primitive pending).

#include "st/devices/ap3/ota_cipher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

TEST_CASE("parse_ota_img extracts header / body / trailer",
          "[devices][ap3][ota_cipher]") {
    // Build a synthetic .img:
    //   4 BE base_ctr = 0xDEADBEEF
    //   2 BE header   = 0x0001
    //   8 bytes body
    //   32 byte trailer = ascending pattern 0x00..0x1F
    std::vector<std::uint8_t> img;
    img.push_back(0xDE);
    img.push_back(0xAD);
    img.push_back(0xBE);
    img.push_back(0xEF);
    img.push_back(0x00);
    img.push_back(0x01);
    for (std::uint8_t b = 0xA0; b < 0xA8; ++b) {
        img.push_back(b);
    }
    for (std::uint8_t b = 0x00; b < 0x20; ++b) {
        img.push_back(b);
    }
    REQUIRE(img.size() == 6 + 8 + 32);

    auto r = st::devices::ap3::cipher::parse_ota_img(img);
    REQUIRE(r.has_value());
    REQUIRE(r->base_ctr == 0xDEADBEEFU);
    REQUIRE(r->header_word == 0x0001U);
    REQUIRE(r->body.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        REQUIRE(r->body[i] == static_cast<std::uint8_t>(0xA0 + i));
    }
    REQUIRE(r->trailer.size() == 32);
    for (std::size_t i = 0; i < 32; ++i) {
        REQUIRE(r->trailer[i] == static_cast<std::uint8_t>(i));
    }
}

TEST_CASE("parse_ota_img refuses input shorter than the minimum envelope",
          "[devices][ap3][ota_cipher][error]") {
    // 6 header + 32 trailer = 38 minimum (zero-body allowed). Pass 37.
    std::array<std::uint8_t, 37> too_short{};
    auto r = st::devices::ap3::cipher::parse_ota_img(too_short);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_ota_img / build_ota_img round-trip preserves bytes",
          "[devices][ap3][ota_cipher]") {
    // Build a synthetic wrapper, splice to bytes, parse back, verify
    // every field matches.
    st::devices::ap3::cipher::OtaImgWrapper src;
    src.base_ctr = 0x12345678;
    src.header_word = 0xABCD;
    for (int i = 0; i < 100; ++i) {
        src.body.push_back(static_cast<std::uint8_t>((i * 31) & 0xFF));
    }
    for (int i = 0; i < 32; ++i) {
        src.trailer.push_back(static_cast<std::uint8_t>((i * 7 + 13) & 0xFF));
    }
    auto const bytes = st::devices::ap3::cipher::build_ota_img(src);
    auto round = st::devices::ap3::cipher::parse_ota_img(bytes);
    REQUIRE(round.has_value());
    REQUIRE(round->base_ctr == src.base_ctr);
    REQUIRE(round->header_word == src.header_word);
    REQUIRE(round->body == src.body);
    REQUIRE(round->trailer == src.trailer);
}

TEST_CASE("blowfish_ctr_double_decrypt gated OFF returns PolicyDenied",
          "[devices][ap3][ota_cipher][gating]") {
#if defined(ST_AP3_HAVE_CIPHER)
    SKIP("Build flag is ON — primitive is live; covered by fixture tests");
#else
    std::array<std::uint8_t, 8> ct{};
    auto const r = st::devices::ap3::cipher::blowfish_ctr_double_decrypt(ct, 0x42);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);
#endif
}

TEST_CASE("decrypt_ota_img gated OFF returns PolicyDenied",
          "[devices][ap3][ota_cipher][gating]") {
#if defined(ST_AP3_HAVE_CIPHER)
    SKIP("Build flag is ON — primitive is live; covered by fixture tests");
#else
    std::vector<std::uint8_t> img(6 + 16 + 32, 0xCD);
    img[0] = 0xDE;
    img[1] = 0xAD;
    img[2] = 0xBE;
    img[3] = 0xEF;
    auto const r = st::devices::ap3::cipher::decrypt_ota_img(img);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);
#endif
}

#if defined(ST_AP3_HAVE_CIPHER)

// Fixture loader — borrows the helper pattern from test_ptm_cipher.cpp
// via a local copy so this file stays self-contained.
namespace {

std::vector<std::uint8_t> load_blowfish_fixture(std::string_view name) {
    std::filesystem::path const path =
        std::filesystem::path{ST_FIXTURE_AP3_CIPHER_DIR} / name;
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        return {};
    }
    auto const sz = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char *>(bytes.data()), sz);
    return bytes;
}

} // namespace

TEST_CASE("blowfish_ctr_double_decrypt fixture-pinned byte-identity",
          "[devices][ap3][ota_cipher][fixtures]") {
    // base_ctr 0xdeadbeef per fixtures/ap3/cipher/blowfish_base_ctr.txt.
    // input  → plaintext bytes
    // output → ciphertext (what the AP firmware emits after Blowfish-CTR)
    // The cipher is its own inverse (XOR stream), so encrypt(plaintext) ==
    // ciphertext AND decrypt(ciphertext) == plaintext are the same call.
    auto plaintext = load_blowfish_fixture("blowfish_input.bin");
    auto ciphertext = load_blowfish_fixture("blowfish_output_body.bin");
    if (plaintext.empty() || ciphertext.empty()) {
        SKIP("blowfish fixtures not staged");
    }
    REQUIRE(plaintext.size() == 8192);
    REQUIRE(ciphertext.size() == 8192);

    constexpr std::uint32_t kBaseCtr = 0xDEADBEEFU;
    auto encrypted = st::devices::ap3::cipher::blowfish_ctr_double_encrypt(
        plaintext, kBaseCtr);
    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->size() == ciphertext.size());
    for (std::size_t i = 0; i < ciphertext.size(); ++i) {
        REQUIRE((*encrypted)[i] == ciphertext[i]);
    }

    auto decrypted = st::devices::ap3::cipher::blowfish_ctr_double_decrypt(
        ciphertext, kBaseCtr);
    REQUIRE(decrypted.has_value());
    REQUIRE(decrypted->size() == plaintext.size());
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        REQUIRE((*decrypted)[i] == plaintext[i]);
    }
}

TEST_CASE("decrypt_ota_img end-to-end against full .img fixture",
          "[devices][ap3][ota_cipher][fixtures]") {
    auto img = load_blowfish_fixture("blowfish_output_full.bin");
    auto plaintext = load_blowfish_fixture("blowfish_input.bin");
    if (img.empty() || plaintext.empty()) {
        SKIP("blowfish fixtures not staged");
    }
    REQUIRE(img.size() == 8230); // 6 header + 8192 body + 32 trailer

    auto decrypted = st::devices::ap3::cipher::decrypt_ota_img(img);
    REQUIRE(decrypted.has_value());
    REQUIRE(decrypted->size() == plaintext.size());
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        REQUIRE((*decrypted)[i] == plaintext[i]);
    }
}

#endif // ST_AP3_HAVE_CIPHER
