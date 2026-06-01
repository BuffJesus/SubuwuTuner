// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace fp = st::feature_patch;

TEST_CASE("ManifestRecord::is_terminator detects the all-zero sentinel",
          "[feature_patch][manifest]") {
    fp::ManifestRecord const zero{};
    REQUIRE(zero.is_terminator());

    fp::ManifestRecord const non_zero{0x00031868, 0x10, 0x02, 608};
    REQUIRE_FALSE(non_zero.is_terminator());

    // Any single non-zero field is enough to disqualify.
    REQUIRE_FALSE((fp::ManifestRecord{1, 0, 0, 0}.is_terminator()));
    REQUIRE_FALSE((fp::ManifestRecord{0, 0x10, 0, 0}.is_terminator()));
    REQUIRE_FALSE((fp::ManifestRecord{0, 0, 0x01, 0}.is_terminator()));
    REQUIRE_FALSE((fp::ManifestRecord{0, 0, 0, 1}.is_terminator()));
}

TEST_CASE("encode emits big-endian u32 address + type/op + big-endian u16 length",
          "[feature_patch][manifest][encode]") {
    // Record from fixtures/private/findings_calibration_deltas/patch_manifest.md:
    //   target=0x00031868, type=0x10, op=0x02, length=608
    // Encoded bytes (BE u32 + u8 + u8 + BE u16):
    //   00 03 18 68 10 02 02 60
    std::array<fp::ManifestRecord, 1> records{
        fp::ManifestRecord{0x00031868, 0x10, 0x02, 608}};
    auto const bytes = fp::encode(records);

    // 1 record + 1 terminator = 16 bytes
    REQUIRE(bytes.size() == 16);

    // First record bytes
    REQUIRE(bytes[0] == 0x00);
    REQUIRE(bytes[1] == 0x03);
    REQUIRE(bytes[2] == 0x18);
    REQUIRE(bytes[3] == 0x68);
    REQUIRE(bytes[4] == 0x10);
    REQUIRE(bytes[5] == 0x02);
    REQUIRE(bytes[6] == 0x02);
    REQUIRE(bytes[7] == 0x60);

    // Terminator (all zero)
    for (std::size_t i = 8; i < 16; ++i) {
        REQUIRE(bytes[i] == 0x00);
    }
}

TEST_CASE("encode/decode round-trip — multiple records",
          "[feature_patch][manifest][round-trip]") {
    // Four records from the staged analyst manifest (different type/op/length
    // combinations to exercise the field-width handling).
    std::vector<fp::ManifestRecord> in{
        {0x00030F64, 0x10, 0x04, 256}, // Sensor Scale
        {0x00031868, 0x10, 0x02, 608}, // Boost Target Main
        {0x00040F3A, 0x10, 0x02, 32},  // Injector Mult Table
        {0x00024316, 0x10, 0x02, 2},   // Rev Limit A
    };
    auto const bytes = fp::encode(in);
    REQUIRE(bytes.size() == (4 + 1) * fp::kRecordSize);

    auto const out = fp::decode(bytes);
    REQUIRE(out.has_value());
    REQUIRE(out->size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE((*out)[i] == in[i]);
    }
}

TEST_CASE("encode of empty records produces just the terminator",
          "[feature_patch][manifest][edge]") {
    std::vector<fp::ManifestRecord> empty;
    auto const bytes = fp::encode(empty);
    REQUIRE(bytes.size() == fp::kRecordSize);
    for (auto b : bytes) {
        REQUIRE(b == 0x00);
    }
}

TEST_CASE("decode of just-a-terminator returns an empty record list",
          "[feature_patch][manifest][edge]") {
    std::vector<std::uint8_t> const just_term(fp::kRecordSize, 0x00);
    auto const out = fp::decode(just_term);
    REQUIRE(out.has_value());
    REQUIRE(out->empty());
}

TEST_CASE("decode ignores bytes past the terminator",
          "[feature_patch][manifest][edge]") {
    // One real record + terminator + garbage record (should be ignored)
    std::vector<std::uint8_t> bytes = {
        // record 1: addr=0x12345678, type=0x10, op=0x01, length=4
        0x12, 0x34, 0x56, 0x78, 0x10, 0x01, 0x00, 0x04,
        // terminator
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // garbage past terminator — must be ignored
        0xAB, 0xCD, 0xEF, 0x12, 0x30, 0x02, 0x00, 0xFF};
    auto const out = fp::decode(bytes);
    REQUIRE(out.has_value());
    REQUIRE(out->size() == 1);
    REQUIRE((*out)[0].target_address == 0x12345678U);
    REQUIRE((*out)[0].type == 0x10);
    REQUIRE((*out)[0].op == 0x01);
    REQUIRE((*out)[0].length_bytes == 4);
}

TEST_CASE("decode rejects byte length not divisible by 8",
          "[feature_patch][manifest][error]") {
    std::vector<std::uint8_t> const odd_length(7, 0x00);
    auto const r = fp::decode(odd_length);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("decode rejects un-terminated input",
          "[feature_patch][manifest][error]") {
    // Two records, no terminator — the on-ECU scan would walk past the
    // end of the manifest region. Refuse to parse as a safety measure.
    std::vector<std::uint8_t> const bytes = {
        0x12, 0x34, 0x56, 0x78, 0x10, 0x01, 0x00, 0x04,
        0xAB, 0xCD, 0xEF, 0x12, 0x30, 0x02, 0x00, 0xFF,
    };
    auto const r = fp::decode(bytes);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("decode preserves type-byte values across the enum range",
          "[feature_patch][manifest][types]") {
    // 0x10 normal, 0x30 verify-only — both observed in fixtures.
    std::vector<fp::ManifestRecord> in{
        {0x00010000, static_cast<std::uint8_t>(fp::ManifestType::NormalPatch), 0x02, 16},
        {0x00010100, static_cast<std::uint8_t>(fp::ManifestType::VerifyOnly), 0x00, 0},
    };
    auto const bytes = fp::encode(in);
    auto const out = fp::decode(bytes);
    REQUIRE(out.has_value());
    REQUIRE(out->size() == 2);
    REQUIRE((*out)[0].type == 0x10);
    REQUIRE((*out)[1].type == 0x30);
    REQUIRE((*out)[1].op == 0x00);
    REQUIRE((*out)[1].length_bytes == 0);
}

TEST_CASE("encode handles a u16-max length",
          "[feature_patch][manifest][edge]") {
    fp::ManifestRecord const big{0x00010000, 0x10, 0x01, 0xFFFF};
    auto const bytes = fp::encode(std::span<fp::ManifestRecord const>{&big, 1});
    REQUIRE(bytes[6] == 0xFF);
    REQUIRE(bytes[7] == 0xFF);

    auto const out = fp::decode(bytes);
    REQUIRE(out.has_value());
    REQUIRE((*out)[0].length_bytes == 0xFFFF);
}
