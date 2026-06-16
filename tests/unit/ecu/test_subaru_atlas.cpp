// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/ecu/subaru_atlas.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace atlas = st::ecu::subaru_atlas;

namespace {

// Build the verbatim 105-byte frame captured on the bench
// (1 ACK byte + 104 body bytes).
std::vector<std::uint8_t> reference_frame_lf79002p() {
    std::vector<std::uint8_t> frame;
    frame.reserve(atlas::kFlashInformationBodyBytes + 1U);
    frame.push_back(atlas::kAckAtlasProprietary);
    auto const body = atlas::reference_flash_information_body_lf79002p();
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

} // namespace

TEST_CASE("reference body is exactly 104 B", "[subaru_atlas][reference]") {
    auto const body = atlas::reference_flash_information_body_lf79002p();
    REQUIRE(body.size() == atlas::kFlashInformationBodyBytes);
}

TEST_CASE("reference body byte 0 status flag is 0xA3", "[subaru_atlas][reference]") {
    // Round-6 handoff §2 best-guess decode: byte 0 is the high-bit-set
    // status flag. Bench observed 0xA3. Guards against accidental
    // mis-edit of the reference array.
    auto const body = atlas::reference_flash_information_body_lf79002p();
    REQUIRE(body[0] == 0xA3U);
}

TEST_CASE("parse_flash_information_body rejects wrong-length input",
          "[subaru_atlas][parse]") {
    std::vector<std::uint8_t> short_body(50, 0);
    auto const r = atlas::parse_flash_information_body(short_body);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_flash_information_body extracts provisional fields from reference",
          "[subaru_atlas][parse]") {
    auto const body = atlas::reference_flash_information_body_lf79002p();
    auto const r = atlas::parse_flash_information_body(body);
    REQUIRE(r.has_value());
    auto const &v = r.value();

    REQUIRE(v.status_flag == 0xA3U);
    REQUIRE(v.field_a == 0x100FU);
    REQUIRE(v.field_b == 0xB029B040U);
    REQUIRE(v.field_c == 0x07U);
    REQUIRE(v.field_d == 0x41U);
    REQUIRE(v.marker_pair_0[0] == 0x80U);
    REQUIRE(v.marker_pair_0[1] == 0x80U);
    REQUIRE(v.marker_offset_40 == 0x80U);
    REQUIRE(v.marker_offset_48 == 0x04U);
    REQUIRE(v.marker_offset_55 == 0x20U);

    REQUIRE(v.raw.size() == atlas::kFlashInformationBodyBytes);
    REQUIRE(v.matches_reference_lf79002p());
}

TEST_CASE("parse_flash_information_frame strips ACK byte from full frame",
          "[subaru_atlas][parse]") {
    auto const frame = reference_frame_lf79002p();
    REQUIRE(frame.size() == atlas::kFlashInformationBodyBytes + 1U);

    auto const r = atlas::parse_flash_information_frame(frame);
    REQUIRE(r.has_value());
    REQUIRE(r.value().status_flag == 0xA3U);
}

TEST_CASE("parse_flash_information_frame surfaces NRC as EcuRejected",
          "[subaru_atlas][parse]") {
    // ECU returned NRC 0x13 for non-empty payloads — emulate that here.
    std::vector<std::uint8_t> const nrc_frame{0x7F, 0xAA, 0x13};
    auto const r = atlas::parse_flash_information_frame(nrc_frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_flash_information_frame rejects wrong ACK byte",
          "[subaru_atlas][parse]") {
    std::vector<std::uint8_t> frame(atlas::kFlashInformationBodyBytes + 1U, 0);
    frame[0] = 0xE8; // SSM-A8 ACK; wrong for 0xAA
    auto const r = atlas::parse_flash_information_frame(frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_flash_information_frame rejects empty frame",
          "[subaru_atlas][parse]") {
    std::vector<std::uint8_t> const empty;
    auto const r = atlas::parse_flash_information_frame(empty);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("hex_dump round-trips through 104 bytes", "[subaru_atlas][diagnostics]") {
    auto const body = atlas::reference_flash_information_body_lf79002p();
    auto const r = atlas::parse_flash_information_body(body);
    REQUIRE(r.has_value());

    auto const dump = r.value().hex_dump();
    // 104 bytes * 2 hex chars + 103 separator spaces = 311
    REQUIRE(dump.size() == 104U * 2U + 103U);

    // First field is "A3 10 0F B0 ..."
    REQUIRE(dump.substr(0, 11) == "A3 10 0F B0");
}

TEST_CASE("matches_reference returns false for tampered raw",
          "[subaru_atlas][diagnostics]") {
    auto const body = atlas::reference_flash_information_body_lf79002p();
    auto r = atlas::parse_flash_information_body(body);
    REQUIRE(r.has_value());
    REQUIRE(r.value().matches_reference_lf79002p());

    r.value().raw[0] = 0x42;
    REQUIRE_FALSE(r.value().matches_reference_lf79002p());
}
