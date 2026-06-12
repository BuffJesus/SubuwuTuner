// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// .ptm cipher tests. Session 1 surface: XTEA-CBC + base64 + outer
// envelope extraction + decrypt_ptm_outer round-trip. Build-flag
// dual-mode: when ST_AP3_HAVE_CIPHER is undefined, the same tests
// verify the PolicyDenied stubs fire.

#include "st/devices/ap3/aes256_ctr.hpp"
#include "st/devices/ap3/bzip2_decompress.hpp"
#include "st/devices/ap3/ptm_cipher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[maybe_unused]] std::vector<std::uint8_t> load_cipher_fixture(char const *name) {
    std::filesystem::path p = std::filesystem::path{ST_FIXTURE_AP3_CIPHER_DIR} / name;
    std::ifstream in{p, std::ios::binary | std::ios::ate};
    if (!in) {
        return {};
    }
    auto const sz = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char *>(bytes.data()), sz);
    return bytes;
}

[[maybe_unused]] std::string load_cipher_fixture_text(char const *name) {
    auto bytes = load_cipher_fixture(name);
    std::string s(reinterpret_cast<char const *>(bytes.data()), bytes.size());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

} // namespace

#ifdef ST_AP3_HAVE_CIPHER

TEST_CASE("ap3 cipher: XTEA-CBC encrypt -> decrypt round-trip preserves bytes",
          "[devices][ap3][cipher]") {
    // File-level: encrypt produces output with 5-byte trailer; decrypt
    // parses the trailer and strips zero-padding. Arbitrary length OK.
    std::array<std::uint8_t, 27> const plaintext{
        'H', 'e', 'l', 'l', 'o', ',', ' ', 'A', 'c', 'c', 'e', 's',
        's', 'P', 'o', 'r', 't', '!', ' ', 'p', 't', 'm', '!', '?',
        '$', '#', '@'
    };
    std::uint32_t const seed = 0x12345678U;
    auto encrypted = st::devices::ap3::cipher::xtea_cbc_encrypt(plaintext, seed);
    REQUIRE(encrypted.has_value());
    // 27 bytes plaintext → 5 pad bytes → 32 ciphertext + 5 trailer = 37 bytes.
    REQUIRE(encrypted->size() == 32 + 5);

    auto decrypted = st::devices::ap3::cipher::xtea_cbc_decrypt(*encrypted);
    REQUIRE(decrypted.has_value());
    REQUIRE(decrypted->size() == plaintext.size());
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        REQUIRE((*decrypted)[i] == plaintext[i]);
    }
}

TEST_CASE("ap3 cipher: XTEA-CBC decrypt rejects short input",
          "[devices][ap3][cipher]") {
    std::array<std::uint8_t, 7> bad{0, 0, 0, 0, 0, 0, 0};
    auto r = st::devices::ap3::cipher::xtea_cbc_decrypt(bad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("ap3 cipher: base64 encodes \"Man\" to \"TWFu\"", "[devices][ap3][cipher]") {
    std::array<std::uint8_t, 3> const m{'M', 'a', 'n'};
    auto enc = st::devices::ap3::cipher::base64_encode(m);
    REQUIRE(enc == "TWFu");
}

TEST_CASE("ap3 cipher: base64 round-trip on assorted lengths", "[devices][ap3][cipher]") {
    for (std::string_view const txt : {std::string_view{"f"}, std::string_view{"fo"},
                                       std::string_view{"foo"}, std::string_view{"foob"},
                                       std::string_view{"fooba"}, std::string_view{"foobar"}}) {
        std::span<std::uint8_t const> bytes{reinterpret_cast<std::uint8_t const *>(txt.data()),
                                            txt.size()};
        auto enc = st::devices::ap3::cipher::base64_encode(bytes);
        auto dec = st::devices::ap3::cipher::base64_decode(enc);
        REQUIRE(dec.has_value());
        REQUIRE(dec->size() == txt.size());
        for (std::size_t i = 0; i < txt.size(); ++i) {
            REQUIRE(static_cast<char>((*dec)[i]) == txt[i]);
        }
    }
}

TEST_CASE("ap3 cipher: base64_decode tolerates whitespace and padding",
          "[devices][ap3][cipher]") {
    auto dec = st::devices::ap3::cipher::base64_decode("TWFu\n\r  ");
    REQUIRE(dec.has_value());
    REQUIRE(dec->size() == 3);
    REQUIRE((*dec)[0] == 'M');
    REQUIRE((*dec)[1] == 'a');
    REQUIRE((*dec)[2] == 'n');
}

TEST_CASE("ap3 cipher: base64_decode rejects invalid characters",
          "[devices][ap3][cipher]") {
    auto dec = st::devices::ap3::cipher::base64_decode("TWF!");
    REQUIRE_FALSE(dec.has_value());
    REQUIRE(dec.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("ap3 cipher: extract_enc_data finds the body between the tags",
          "[devices][ap3][cipher]") {
    std::string_view const xml = "<root><encData>abcDEF==</encData></root>";
    auto body = st::devices::ap3::cipher::extract_enc_data(xml);
    REQUIRE(body.has_value());
    REQUIRE(*body == "abcDEF==");
}

TEST_CASE("ap3 cipher: extract_enc_data returns ParseError on missing tags",
          "[devices][ap3][cipher]") {
    auto a = st::devices::ap3::cipher::extract_enc_data("<root></root>");
    REQUIRE_FALSE(a.has_value());
    REQUIRE(a.error().code() == st::ErrorCode::ParseError);

    auto b = st::devices::ap3::cipher::extract_enc_data("<root><encData>oops");
    REQUIRE_FALSE(b.has_value());
    REQUIRE(b.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("ap3 cipher: decrypt_ptm_outer round-trips a synthetic .ptm envelope",
          "[devices][ap3][cipher]") {
    std::string const outer_xml = "<root><meta>vendor=test</meta></root>";
    std::uint32_t const seed = 0x12345678U;
    std::span<std::uint8_t const> pt_span{
        reinterpret_cast<std::uint8_t const *>(outer_xml.data()), outer_xml.size()};
    auto encrypted = st::devices::ap3::cipher::xtea_cbc_encrypt(pt_span, seed);
    REQUIRE(encrypted.has_value());
    auto recovered = st::devices::ap3::cipher::decrypt_ptm_outer(*encrypted);
    REQUIRE(recovered.has_value());
    REQUIRE(*recovered == outer_xml);
}

TEST_CASE("ap3 cipher: decrypt_ptm_outer rejects invalid pad_count",
          "[devices][ap3][cipher]") {
    // Valid block-aligned ciphertext + a trailer claiming pad_count=8
    // (out of the [0..7] range per spec §13). Should ParseError before
    // touching XTEA.
    std::array<std::uint8_t, 13> ptm{};
    ptm[8] = 0x08; // pad_count out of range
    auto r = st::devices::ap3::cipher::decrypt_ptm_outer(ptm);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

// ---------------------------------------------------------------------------
// Canonical fixture pins (specs/fixtures/ap3/cipher_unit_fixtures/).
// Verify byte-identity against the analyst-generated test vectors.
// ---------------------------------------------------------------------------

// RESOLVED 2026-06-11 PM: the fixture had a Python operator-precedence
// bug in the generator (`cipher_unit_test_fixtures.py`) — missing
// parens in `xtea_encrypt_block` parsed `v0 + EXPR1 ^ EXPR2` as
// `(v0 + EXPR1) ^ EXPR2` (Python `+` binds tighter than `^`) instead
// of the standard `v0 + (EXPR1 ^ EXPR2)`. The walkthrough §1 algorithm
// + my implementation were correct all along; the analyst regenerated
// `xtea_layer1_output.bin` against the fixed generator. New byte 0 is
// `0x89` — matches my output exactly. Tag dropped, test re-enabled.
TEST_CASE("ap3 cipher fixtures: XTEA-CBC layer 1 byte-identical against canonical vectors",
          "[devices][ap3][cipher][fixtures]") {
    auto plaintext = load_cipher_fixture("xtea_layer1_input.bin");
    auto expected = load_cipher_fixture("xtea_layer1_output.bin");
    auto seed_text = load_cipher_fixture_text("xtea_layer1_seed.txt");
    if (plaintext.empty() || expected.empty() || seed_text.empty()) {
        SKIP("xtea layer 1 fixtures not staged");
    }
    REQUIRE(seed_text.starts_with("0x"));
    std::uint32_t const seed =
        static_cast<std::uint32_t>(std::stoul(seed_text.substr(2), nullptr, 16));

    auto encrypted = st::devices::ap3::cipher::xtea_cbc_encrypt(plaintext, seed);
    REQUIRE(encrypted.has_value());
    // Round-trip-self consistency: regardless of the fixture
    // disagreement, encrypt → decrypt must recover the input.
    auto decrypted = st::devices::ap3::cipher::xtea_cbc_decrypt(*encrypted);
    REQUIRE(decrypted.has_value());
    REQUIRE(*decrypted == plaintext);
    // The byte-identity check below is the !shouldfail trigger.
    REQUIRE(encrypted->size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        REQUIRE((*encrypted)[i] == expected[i]);
    }
}

TEST_CASE("ap3 cipher fixtures: base64 layer 2 envelope + decode against canonical vectors",
          "[devices][ap3][cipher][fixtures]") {
    auto input = load_cipher_fixture("base64_layer2_input.txt");
    auto extracted_expected = load_cipher_fixture("base64_layer2_extracted.b64");
    auto inner_expected = load_cipher_fixture("base64_layer2_inner_bytes.bin");
    if (input.empty() || extracted_expected.empty() || inner_expected.empty()) {
        SKIP("base64 layer 2 fixtures not staged");
    }
    std::string_view input_xml{reinterpret_cast<char const *>(input.data()), input.size()};
    auto extracted = st::devices::ap3::cipher::extract_enc_data(input_xml);
    REQUIRE(extracted.has_value());

    // The fixture's extracted .b64 may carry a trailing newline; trim
    // before comparing.
    std::string_view extracted_expected_view{
        reinterpret_cast<char const *>(extracted_expected.data()), extracted_expected.size()};
    while (!extracted_expected_view.empty() &&
           (extracted_expected_view.back() == '\n' || extracted_expected_view.back() == '\r')) {
        extracted_expected_view.remove_suffix(1);
    }
    REQUIRE(*extracted == extracted_expected_view);

    auto inner = st::devices::ap3::cipher::base64_decode(*extracted);
    REQUIRE(inner.has_value());
    REQUIRE(inner->size() == inner_expected.size());
    for (std::size_t i = 0; i < inner_expected.size(); ++i) {
        REQUIRE((*inner)[i] == inner_expected[i]);
    }
}

TEST_CASE("ap3 cipher: AES-256-CTR symmetric round-trip with the custom construction",
          "[devices][ap3][cipher]") {
    // The canonical aes_layer3_input/output fixture was generated for
    // STANDARD AES-CTR (linear 16-byte counter), but the real `.ptm`
    // format uses a custom CTR with 16 KB chunks + counter-repeated-4
    // input blocks — see pkg::ExrRil::en_de + the reference Python
    // tool. Live-verified 2026-06-12 against real tunes from the AP.
    //
    // Self-consistency round-trip is sufficient here; byte-identity
    // against an external fixture is gated on the analyst staging a
    // generator for the custom construction.
    std::vector<std::uint8_t> plaintext;
    for (std::size_t i = 0; i < 8192; ++i) {
        plaintext.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }
    constexpr std::uint32_t kNonce = 0x00000020U;
    auto encrypted = st::devices::ap3::cipher::aes256_ctr_encrypt(plaintext, kNonce);
    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->size() == plaintext.size());
    auto decrypted = st::devices::ap3::cipher::aes256_ctr_decrypt(*encrypted, kNonce);
    REQUIRE(decrypted.has_value());
    REQUIRE(*decrypted == plaintext);
    // Different nonce → different keystream → wrong plaintext.
    auto wrong = st::devices::ap3::cipher::aes256_ctr_decrypt(*encrypted, 0x21U);
    REQUIRE(wrong.has_value());
    REQUIRE_FALSE(*wrong == plaintext);
}

TEST_CASE("ap3 cipher fixtures: bzip2 decompress layer 4 against canonical vectors",
          "[devices][ap3][cipher][fixtures]") {
    auto plaintext = load_cipher_fixture("bzip2_layer4_input.bin");
    auto compressed = load_cipher_fixture("bzip2_layer4_output.bin");
    if (plaintext.empty() || compressed.empty()) {
        SKIP("bzip2 layer 4 fixtures not staged");
    }
    auto decompressed = st::devices::ap3::cipher::bzip2_decompress(compressed);
    REQUIRE(decompressed.has_value());
    auto const expected_text =
        std::string(reinterpret_cast<char const *>(plaintext.data()), plaintext.size());
    REQUIRE(*decompressed == expected_text);
}

TEST_CASE("ap3 cipher: bzip2_decompress rejects non-bzip2 input",
          "[devices][ap3][cipher]") {
    std::array<std::uint8_t, 16> garbage{};
    auto r = st::devices::ap3::cipher::bzip2_decompress(garbage);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("ap3 cipher: bzip2_compress round-trips against decompress",
          "[devices][ap3][cipher]") {
#ifndef ST_AP3_HAVE_PTM_REWRITE
    // The rewrite gate is off — bzip2_compress should return
    // NotImplemented, not actually compress. Pin that and skip the
    // real round-trip.
    std::string_view const sv{"hello bzip2 round trip"};
    std::span<std::uint8_t const> input{
        reinterpret_cast<std::uint8_t const *>(sv.data()), sv.size()};
    auto compressed = st::devices::ap3::cipher::bzip2_compress(input);
    REQUIRE_FALSE(compressed.has_value());
    REQUIRE(compressed.error().code() == st::ErrorCode::NotImplemented);
#else
    std::string const plaintext =
        "<PrivateData><patches><patch romOffset=\"0\" ramOffset=\"0\" length=\"5\">"
        "SGVsbG8=</patch></patches></PrivateData>";
    std::span<std::uint8_t const> input{
        reinterpret_cast<std::uint8_t const *>(plaintext.data()), plaintext.size()};
    auto compressed = st::devices::ap3::cipher::bzip2_compress(input);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() >= 3U);
    // BZh magic at the start of the stream.
    REQUIRE((*compressed)[0] == 'B');
    REQUIRE((*compressed)[1] == 'Z');
    REQUIRE((*compressed)[2] == 'h');
    auto decompressed = st::devices::ap3::cipher::bzip2_decompress(*compressed);
    REQUIRE(decompressed.has_value());
    REQUIRE(*decompressed == plaintext);
#endif
}

TEST_CASE("ap3 cipher: encrypt_ptm -> decrypt_ptm logical round-trip",
          "[devices][ap3][cipher]") {
#ifndef ST_AP3_HAVE_PTM_REWRITE
    // Rewrite gate off — encrypt_ptm cascades into bzip2_compress's
    // NotImplemented. Confirm that's the failure mode.
    auto r = st::devices::ap3::cipher::encrypt_ptm(
        "<PrivateData></PrivateData>", "<Outer/>", 0x12345678U);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
#else
    std::string const private_data =
        "<PrivateData>"
        "<vendorID>SUBUWU_TEST</vendorID>"
        "<vehicleID>SYNTHETIC_E2E_FIXTURE</vehicleID>"
        "<lock mask=\"0\" />"
        "<romSum value=\"0\" />"
        "<saveDateTime value=\"0\" />"
        "<patches>"
        "<patch romOffset=\"32768\" ramOffset=\"-657408\" length=\"5\">SGVsbG8=</patch>"
        "</patches>"
        "</PrivateData>";
    std::string const outer_meta =
        "<root><meta>vendor=subuwu_test</meta></root>";
    std::uint32_t const seed = 0x12345678U;

    auto ptm_bytes = st::devices::ap3::cipher::encrypt_ptm(
        private_data, outer_meta, seed);
    REQUIRE(ptm_bytes.has_value());

    auto contents = st::devices::ap3::cipher::decrypt_ptm(*ptm_bytes);
    REQUIRE(contents.has_value());
    // The inner XML must round-trip byte-identical (bzip2 + AES-CTR
    // are deterministic given the spec key/IV).
    REQUIRE(contents->private_data_xml == private_data);
    // The outer metadata should still contain the original "vendor"
    // marker (the `<encData>` injection point sits before `</root>`).
    REQUIRE(contents->outer_metadata_xml.find("vendor=subuwu_test") !=
            std::string::npos);
    // And the stripped outer should NOT contain `<encData>` after the
    // decrypt path scrubbed it.
    REQUIRE(contents->outer_metadata_xml.find("<encData>") == std::string::npos);
#endif
}

#else // !ST_AP3_HAVE_CIPHER

TEST_CASE("ap3 cipher: PolicyDenied when build flag is off",
          "[devices][ap3][cipher]") {
    std::array<std::uint8_t, 8> dummy{};
    auto r = st::devices::ap3::cipher::decrypt_ptm_outer(dummy);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);

    auto e = st::devices::ap3::cipher::xtea_cbc_encrypt(dummy, std::uint32_t{0});
    REQUIRE_FALSE(e.has_value());
    REQUIRE(e.error().code() == st::ErrorCode::PolicyDenied);

    auto d = st::devices::ap3::cipher::xtea_cbc_decrypt(dummy);
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::PolicyDenied);

    auto a = st::devices::ap3::cipher::aes256_ctr_decrypt(dummy, 0x20U);
    REQUIRE_FALSE(a.has_value());
    REQUIRE(a.error().code() == st::ErrorCode::PolicyDenied);

    auto b = st::devices::ap3::cipher::bzip2_decompress(dummy);
    REQUIRE_FALSE(b.has_value());
    REQUIRE(b.error().code() == st::ErrorCode::PolicyDenied);

    auto f = st::devices::ap3::cipher::decrypt_ptm(dummy);
    REQUIRE_FALSE(f.has_value());
    REQUIRE(f.error().code() == st::ErrorCode::PolicyDenied);
}

#endif // ST_AP3_HAVE_CIPHER
