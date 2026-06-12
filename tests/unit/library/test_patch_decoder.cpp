// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Spec: specs/cobb-ap3-patch-decoder-spec.md

#include "st/library/patch_decoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Synthetic E2E fixture — verbatim from the patch decoder spec.
constexpr std::string_view kSyntheticPrivateData =
    "<PrivateData>"
    "<vendorID>SUBUWU_TEST</vendorID>"
    "<vehicleID>SYNTHETIC_E2E_FIXTURE</vehicleID>"
    "<lock mask=\"0\" />"
    "<romSum value=\"12345\" />"
    "<saveDateTime value=\"0\" />"
    "<patches>"
    "<patch romOffset=\"32768\" ramOffset=\"-657408\" length=\"5\">SGVsbG8=</patch>"
    "<patch romOffset=\"65536\" ramOffset=\"-624640\" length=\"5\">V29ybGQ=</patch>"
    "</patches>"
    "</PrivateData>";

} // namespace

TEST_CASE("patch decoder: synthetic E2E fixture round-trip",
          "[library][patch_decoder]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("base64_decode lives behind ST_AP3_HAVE_CIPHER");
#else
    auto result = st::library::decode_ptm_xml(kSyntheticPrivateData);
    REQUIRE(result.has_value());
    REQUIRE(result->metadata.vendor_id == "SUBUWU_TEST");
    REQUIRE(result->metadata.vehicle_id == "SYNTHETIC_E2E_FIXTURE");
    REQUIRE(result->metadata.lock_mask == 0);
    REQUIRE(result->metadata.rom_sum == "12345");
    REQUIRE(result->metadata.save_date_time == "0");
    REQUIRE(result->patches.size() == 2);
    REQUIRE(result->patches[0].rom_offset == 32768U);
    REQUIRE(result->patches[0].ram_offset == -657408);
    REQUIRE(result->patches[0].length == 5U);
    REQUIRE(result->patches[0].bytes ==
            std::vector<std::uint8_t>{'H', 'e', 'l', 'l', 'o'});
    REQUIRE(result->patches[1].rom_offset == 65536U);
    REQUIRE(result->patches[1].ram_offset == -624640);
    REQUIRE(result->patches[1].bytes ==
            std::vector<std::uint8_t>{'W', 'o', 'r', 'l', 'd'});
#endif
}

TEST_CASE("patch decoder: handles missing saveDateTime",
          "[library][patch_decoder]") {
    // Empty patches list and no <saveDateTime> — save_date_time defaults
    // to "0". base64_decode isn't called so the test runs even with
    // cipher gated OFF.
    constexpr std::string_view kXml =
        "<PrivateData>"
        "<vendorID>X</vendorID>"
        "<vehicleID>Y</vehicleID>"
        "<lock mask=\"0\" />"
        "<romSum value=\"0\" />"
        "<patches></patches>"
        "</PrivateData>";
    auto result = st::library::decode_ptm_xml(kXml);
    REQUIRE(result.has_value());
    REQUIRE(result->metadata.save_date_time == "0");
    REQUIRE(result->patches.empty());
}

TEST_CASE("patch decoder: tolerates whitespace + line breaks",
          "[library][patch_decoder]") {
    constexpr std::string_view kXml =
        "<PrivateData>\n"
        "  <vendorID>SUBUWU_TEST</vendorID>\n"
        "  <vehicleID>YYY</vehicleID>\n"
        "  <lock mask=\"7\" />\n"
        "  <romSum value=\"42\" />\n"
        "  <patches>\n"
        "  </patches>\n"
        "</PrivateData>\n";
    auto result = st::library::decode_ptm_xml(kXml);
    REQUIRE(result.has_value());
    REQUIRE(result->metadata.vendor_id == "SUBUWU_TEST");
    REQUIRE(result->metadata.vehicle_id == "YYY");
    REQUIRE(result->metadata.lock_mask == 7U);
    REQUIRE(result->metadata.rom_sum == "42");
}

TEST_CASE("patch decoder: tolerates unknown PrivateData child elements",
          "[library][patch_decoder]") {
    // Firmware versions may add new metadata fields; the decoder
    // should ignore unrecognised PrivateData children rather than
    // hard-fail.
    constexpr std::string_view kXml =
        "<PrivateData>"
        "<vendorID>X</vendorID>"
        "<vehicleID>Y</vehicleID>"
        "<lock mask=\"0\" />"
        "<romSum value=\"0\" />"
        "<futureField value=\"hello\" />"
        "<patches></patches>"
        "</PrivateData>";
    auto result = st::library::decode_ptm_xml(kXml);
    REQUIRE(result.has_value());
    REQUIRE(result->patches.empty());
}

TEST_CASE("patch decoder: rejects missing root element",
          "[library][patch_decoder][negative]") {
    constexpr std::string_view kBad = "<NotPrivateData></NotPrivateData>";
    auto r = st::library::decode_ptm_xml(kBad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch decoder: rejects empty input",
          "[library][patch_decoder][negative]") {
    auto r = st::library::decode_ptm_xml("");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch decoder: rejects non-numeric romOffset",
          "[library][patch_decoder][negative]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("base64_decode lives behind ST_AP3_HAVE_CIPHER");
#else
    constexpr std::string_view kBad =
        "<PrivateData><vendorID>X</vendorID><vehicleID>Y</vehicleID>"
        "<lock mask=\"0\" /><romSum value=\"0\" /><saveDateTime value=\"0\" />"
        "<patches>"
        "<patch romOffset=\"abc\" ramOffset=\"0\" length=\"1\">AA==</patch>"
        "</patches></PrivateData>";
    auto r = st::library::decode_ptm_xml(kBad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
#endif
}

TEST_CASE("patch decoder: rejects length / decoded-size mismatch",
          "[library][patch_decoder][negative]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("base64_decode lives behind ST_AP3_HAVE_CIPHER");
#else
    // length=5 declared but base64 body decodes to 3 bytes ("ABC")
    constexpr std::string_view kBad =
        "<PrivateData><vendorID>X</vendorID><vehicleID>Y</vehicleID>"
        "<lock mask=\"0\" /><romSum value=\"0\" /><saveDateTime value=\"0\" />"
        "<patches>"
        "<patch romOffset=\"0\" ramOffset=\"0\" length=\"5\">QUJD</patch>"
        "</patches></PrivateData>";
    auto r = st::library::decode_ptm_xml(kBad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
#endif
}

TEST_CASE("patch decoder: rejects length=0",
          "[library][patch_decoder][negative]") {
    constexpr std::string_view kBad =
        "<PrivateData><vendorID>X</vendorID><vehicleID>Y</vehicleID>"
        "<lock mask=\"0\" /><romSum value=\"0\" />"
        "<patches>"
        "<patch romOffset=\"0\" ramOffset=\"0\" length=\"0\"></patch>"
        "</patches></PrivateData>";
    auto r = st::library::decode_ptm_xml(kBad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch decoder: rejects implausibly large length",
          "[library][patch_decoder][negative]") {
    // Past the kMaxPatchLengthBytes cap (1 MB). Sanity-cap check
    // prevents allocate-bomb if XML attacker-controlled.
    std::string bad =
        "<PrivateData><vendorID>X</vendorID><vehicleID>Y</vehicleID>"
        "<lock mask=\"0\" /><romSum value=\"0\" />"
        "<patches>"
        "<patch romOffset=\"0\" ramOffset=\"0\" length=\"" +
        std::to_string(st::library::kMaxPatchLengthBytes + 1U) +
        "\">AA==</patch></patches></PrivateData>";
    auto r = st::library::decode_ptm_xml(bad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("patch decoder: rejects missing required field",
          "[library][patch_decoder][negative]") {
    // No <vehicleID>
    constexpr std::string_view kBad =
        "<PrivateData><vendorID>X</vendorID>"
        "<lock mask=\"0\" /><romSum value=\"0\" />"
        "<patches></patches></PrivateData>";
    auto r = st::library::decode_ptm_xml(kBad);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}
