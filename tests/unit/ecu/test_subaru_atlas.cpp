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
    frame.reserve(atlas::kSsmExtensionInfoBodyBytes + 1U);
    frame.push_back(atlas::kAckSsmExtensionInfo);
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

} // namespace

TEST_CASE("reference body is exactly 104 B", "[subaru_atlas][reference]") {
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    REQUIRE(body.size() == atlas::kSsmExtensionInfoBodyBytes);
}

TEST_CASE("reference body byte 0 status flag is 0xA3", "[subaru_atlas][reference]") {
    // Round-6 handoff §2 best-guess decode: byte 0 is the high-bit-set
    // status flag. Bench observed 0xA3. Guards against accidental
    // mis-edit of the reference array.
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    REQUIRE(body[0] == 0xA3U);
}

TEST_CASE("parse_ssm_extension_info_body rejects wrong-length input",
          "[subaru_atlas][parse]") {
    std::vector<std::uint8_t> short_body(50, 0);
    auto const r = atlas::parse_ssm_extension_info_body(short_body);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_ssm_extension_info_body extracts provisional fields from reference",
          "[subaru_atlas][parse]") {
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    auto const r = atlas::parse_ssm_extension_info_body(body);
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

    REQUIRE(v.raw.size() == atlas::kSsmExtensionInfoBodyBytes);
    REQUIRE(v.matches_reference_lf79002p());
}

TEST_CASE("parse_ssm_extension_info_frame strips ACK byte from full frame",
          "[subaru_atlas][parse]") {
    auto const frame = reference_frame_lf79002p();
    REQUIRE(frame.size() == atlas::kSsmExtensionInfoBodyBytes + 1U);

    auto const r = atlas::parse_ssm_extension_info_frame(frame);
    REQUIRE(r.has_value());
    REQUIRE(r.value().status_flag == 0xA3U);
}

TEST_CASE("parse_ssm_extension_info_frame surfaces NRC as EcuRejected",
          "[subaru_atlas][parse]") {
    // ECU returned NRC 0x13 for non-empty payloads — emulate that here.
    std::vector<std::uint8_t> const nrc_frame{0x7F, 0xAA, 0x13};
    auto const r = atlas::parse_ssm_extension_info_frame(nrc_frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_ssm_extension_info_frame rejects wrong ACK byte",
          "[subaru_atlas][parse]") {
    std::vector<std::uint8_t> frame(atlas::kSsmExtensionInfoBodyBytes + 1U, 0);
    frame[0] = 0xE8; // SSM-A8 ACK; wrong for 0xAA
    auto const r = atlas::parse_ssm_extension_info_frame(frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_ssm_extension_info_frame rejects empty frame",
          "[subaru_atlas][parse]") {
    std::vector<std::uint8_t> const empty;
    auto const r = atlas::parse_ssm_extension_info_frame(empty);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("hex_dump round-trips through 104 bytes", "[subaru_atlas][diagnostics]") {
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    auto const r = atlas::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());

    auto const dump = r.value().hex_dump();
    // 104 bytes * 2 hex chars + 103 separator spaces = 311
    REQUIRE(dump.size() == 104U * 2U + 103U);

    // First field is "A3 10 0F B0 ..."
    REQUIRE(dump.substr(0, 11) == "A3 10 0F B0");
}

TEST_CASE("matches_reference returns false for tampered raw",
          "[subaru_atlas][diagnostics]") {
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    auto r = atlas::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());
    REQUIRE(r.value().matches_reference_lf79002p());

    r.value().raw[0] = 0x42;
    REQUIRE_FALSE(r.value().matches_reference_lf79002p());
}

TEST_CASE("device_id_prefix returns first 8 bytes per round-7 spec §2.2",
          "[subaru_atlas][device_id]") {
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    auto const r = atlas::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());

    auto const prefix = r.value().device_id_prefix();
    REQUIRE(prefix.size() == atlas::kDeviceIdPrefixBytes);

    // Round-7 spec §2.2 — LF79002P primary template prefix emitted from
    // ROM 0x00061896. Pin the byte sequence exactly.
    REQUIRE(prefix == atlas::kDeviceIdPrefixLf79002pPrimary);
}

TEST_CASE("has_lf79002p_primary_prefix matches bench capture",
          "[subaru_atlas][device_id]") {
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    auto const r = atlas::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());
    REQUIRE(r.value().has_lf79002p_primary_prefix());
}

TEST_CASE("has_lf79002p_primary_prefix rejects secondary-template prefix",
          "[subaru_atlas][device_id]") {
    // Synthesize a body starting with the secondary-template prefix
    // (round-7 spec §2.3). Should not match the primary prefix.
    std::vector<std::uint8_t> alt(atlas::kSsmExtensionInfoBodyBytes, 0);
    for (std::size_t i = 0; i < atlas::kDeviceIdPrefixLf79002pSecondary.size(); ++i) {
        alt[i] = atlas::kDeviceIdPrefixLf79002pSecondary[i];
    }
    auto const r = atlas::parse_ssm_extension_info_body(alt);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r.value().has_lf79002p_primary_prefix());
}

// Round-7 spec §6 item 5 — round-trip Catch2 fixture: pin AA request
// bytes; assert response parses cleanly; assert 8-byte device-ID prefix.
TEST_CASE("SSM-extension info request/response round-trip pins the wire format",
          "[subaru_atlas][round_trip]") {
    // The request is the literal single byte 0xAA. No payload.
    std::vector<std::uint8_t> const request_bytes{atlas::kSidSsmExtensionInfo};
    REQUIRE(request_bytes.size() == 1U);
    REQUIRE(request_bytes[0] == 0xAAU);

    // The response on the bench is ACK + 104-byte body. Reconstruct it
    // from the reference body so the fixture stays self-contained.
    std::vector<std::uint8_t> response_bytes;
    response_bytes.reserve(105U);
    response_bytes.push_back(atlas::kAckSsmExtensionInfo);
    auto const body = atlas::reference_ssm_extension_info_body_lf79002p();
    response_bytes.insert(response_bytes.end(), body.begin(), body.end());
    REQUIRE(response_bytes.size() == 105U);
    REQUIRE(response_bytes[0] == 0xEAU);

    auto const parsed = atlas::parse_ssm_extension_info_frame(response_bytes);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().has_lf79002p_primary_prefix());
    REQUIRE(parsed.value().raw.size() == atlas::kSsmExtensionInfoBodyBytes);
}

// Round-9 spec §1 — pin Atlas SID byte assignments recovered from
// source-of-truth (`XM.J` static-init block). Guards against drift
// in the SID constants if someone misreads round-1..8 specs again.
TEST_CASE("Atlas SID constants match round-9 source-of-truth",
          "[subaru_atlas][atlas_sids]") {
    REQUIRE(atlas::kSidAtlasFlashInformation  == 0xA1U);
    REQUIRE(atlas::kSidAtlasBlockInformation  == 0xA2U);
    REQUIRE(atlas::kSidAtlasRead              == 0xAAU);
    REQUIRE(atlas::kSidAtlasClear             == 0xACU);
    REQUIRE(atlas::kSidAtlasProgram           == 0xADU);
    REQUIRE(atlas::kSidAtlasControl           == 0xAFU);
    REQUIRE(atlas::kSidSubaruTransfer         == 0xB6U);
}

TEST_CASE("AtlasRead SID collides with SsmExtensionInfo SID by design",
          "[subaru_atlas][atlas_sids]") {
    // Per round-9 spec §2.3 the firmware's 0xAA handler is dual-purpose:
    // empty payload returns the 104-B SSM-extension info block (what
    // round-6 caught); (addr u32 BE, length u16 BE) payload returns a
    // memory read (what Atlas uses). Same SID byte, different routes.
    REQUIRE(atlas::kSidAtlasRead == atlas::kSidSsmExtensionInfo);
}
