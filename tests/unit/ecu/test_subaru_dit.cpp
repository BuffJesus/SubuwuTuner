// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/ecu/subaru_dit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace dit = st::ecu::subaru_dit;

namespace {

// Build the verbatim 105-byte frame captured on the bench
// (1 ACK byte + 104 body bytes).
std::vector<std::uint8_t> reference_frame_lf79002p() {
    std::vector<std::uint8_t> frame;
    frame.reserve(dit::kSsmExtensionInfoBodyBytes + 1U);
    frame.push_back(dit::kAckSsmExtensionInfo);
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

} // namespace

TEST_CASE("reference body is exactly 104 B", "[subaru_dit][reference]") {
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    REQUIRE(body.size() == dit::kSsmExtensionInfoBodyBytes);
}

TEST_CASE("reference body byte 0 status flag is 0xA3", "[subaru_dit][reference]") {
    // Round-6 handoff §2 best-guess decode: byte 0 is the high-bit-set
    // status flag. Bench observed 0xA3. Guards against accidental
    // mis-edit of the reference array.
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    REQUIRE(body[0] == 0xA3U);
}

TEST_CASE("parse_ssm_extension_info_body rejects wrong-length input",
          "[subaru_dit][parse]") {
    std::vector<std::uint8_t> short_body(50, 0);
    auto const r = dit::parse_ssm_extension_info_body(short_body);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_ssm_extension_info_body extracts provisional fields from reference",
          "[subaru_dit][parse]") {
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    auto const r = dit::parse_ssm_extension_info_body(body);
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

    REQUIRE(v.raw.size() == dit::kSsmExtensionInfoBodyBytes);
    REQUIRE(v.matches_reference_lf79002p());
}

TEST_CASE("parse_ssm_extension_info_frame strips ACK byte from full frame",
          "[subaru_dit][parse]") {
    auto const frame = reference_frame_lf79002p();
    REQUIRE(frame.size() == dit::kSsmExtensionInfoBodyBytes + 1U);

    auto const r = dit::parse_ssm_extension_info_frame(frame);
    REQUIRE(r.has_value());
    REQUIRE(r.value().status_flag == 0xA3U);
}

TEST_CASE("parse_ssm_extension_info_frame surfaces NRC as EcuRejected",
          "[subaru_dit][parse]") {
    // ECU returned NRC 0x13 for non-empty payloads — emulate that here.
    std::vector<std::uint8_t> const nrc_frame{0x7F, 0xAA, 0x13};
    auto const r = dit::parse_ssm_extension_info_frame(nrc_frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("parse_ssm_extension_info_frame rejects wrong ACK byte",
          "[subaru_dit][parse]") {
    std::vector<std::uint8_t> frame(dit::kSsmExtensionInfoBodyBytes + 1U, 0);
    frame[0] = 0xE8; // SSM-A8 ACK; wrong for 0xAA
    auto const r = dit::parse_ssm_extension_info_frame(frame);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_ssm_extension_info_frame rejects empty frame",
          "[subaru_dit][parse]") {
    std::vector<std::uint8_t> const empty;
    auto const r = dit::parse_ssm_extension_info_frame(empty);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("hex_dump round-trips through 104 bytes", "[subaru_dit][diagnostics]") {
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    auto const r = dit::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());

    auto const dump = r.value().hex_dump();
    // 104 bytes * 2 hex chars + 103 separator spaces = 311
    REQUIRE(dump.size() == 104U * 2U + 103U);

    // First field is "A3 10 0F B0 ..."
    REQUIRE(dump.substr(0, 11) == "A3 10 0F B0");
}

TEST_CASE("matches_reference returns false for tampered raw",
          "[subaru_dit][diagnostics]") {
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    auto r = dit::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());
    REQUIRE(r.value().matches_reference_lf79002p());

    r.value().raw[0] = 0x42;
    REQUIRE_FALSE(r.value().matches_reference_lf79002p());
}

TEST_CASE("device_id_prefix returns first 8 bytes per round-7 spec §2.2",
          "[subaru_dit][device_id]") {
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    auto const r = dit::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());

    auto const prefix = r.value().device_id_prefix();
    REQUIRE(prefix.size() == dit::kDeviceIdPrefixBytes);

    // Round-7 spec §2.2 — LF79002P primary template prefix emitted from
    // ROM 0x00061896. Pin the byte sequence exactly.
    REQUIRE(prefix == dit::kDeviceIdPrefixLf79002pPrimary);
}

TEST_CASE("has_lf79002p_primary_prefix matches bench capture",
          "[subaru_dit][device_id]") {
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    auto const r = dit::parse_ssm_extension_info_body(body);
    REQUIRE(r.has_value());
    REQUIRE(r.value().has_lf79002p_primary_prefix());
}

TEST_CASE("has_lf79002p_primary_prefix rejects secondary-template prefix",
          "[subaru_dit][device_id]") {
    // Synthesize a body starting with the secondary-template prefix
    // (round-7 spec §2.3). Should not match the primary prefix.
    std::vector<std::uint8_t> alt(dit::kSsmExtensionInfoBodyBytes, 0);
    for (std::size_t i = 0; i < dit::kDeviceIdPrefixLf79002pSecondary.size(); ++i) {
        alt[i] = dit::kDeviceIdPrefixLf79002pSecondary[i];
    }
    auto const r = dit::parse_ssm_extension_info_body(alt);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r.value().has_lf79002p_primary_prefix());
}

// Round-7 spec §6 item 5 — round-trip Catch2 fixture: pin AA request
// bytes; assert response parses cleanly; assert 8-byte device-ID prefix.
TEST_CASE("SSM-extension info request/response round-trip pins the wire format",
          "[subaru_dit][round_trip]") {
    // The request is the literal single byte 0xAA. No payload.
    std::vector<std::uint8_t> const request_bytes{dit::kSidSsmExtensionInfo};
    REQUIRE(request_bytes.size() == 1U);
    REQUIRE(request_bytes[0] == 0xAAU);

    // The response on the bench is ACK + 104-byte body. Reconstruct it
    // from the reference body so the fixture stays self-contained.
    std::vector<std::uint8_t> response_bytes;
    response_bytes.reserve(105U);
    response_bytes.push_back(dit::kAckSsmExtensionInfo);
    auto const body = dit::reference_ssm_extension_info_body_lf79002p();
    response_bytes.insert(response_bytes.end(), body.begin(), body.end());
    REQUIRE(response_bytes.size() == 105U);
    REQUIRE(response_bytes[0] == 0xEAU);

    auto const parsed = dit::parse_ssm_extension_info_frame(response_bytes);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().has_lf79002p_primary_prefix());
    REQUIRE(parsed.value().raw.size() == dit::kSsmExtensionInfoBodyBytes);
}

// Round-9 spec §1 — pin Atlas SID byte assignments recovered from
// source-of-truth (`XM.J` static-init block). Guards against drift
// in the SID constants if someone misreads round-1..8 specs again.
TEST_CASE("Atlas SID constants match round-9 source-of-truth",
          "[subaru_dit][atlas_sids]") {
    REQUIRE(dit::kSidDitFlashInformation  == 0xA1U);
    REQUIRE(dit::kSidDitBlockInformation  == 0xA2U);
    REQUIRE(dit::kSidDitRead              == 0xAAU);
    REQUIRE(dit::kSidDitClear             == 0xACU);
    REQUIRE(dit::kSidDitProgram           == 0xADU);
    REQUIRE(dit::kSidDitControl           == 0xAFU);
    REQUIRE(dit::kSidSubaruTransfer         == 0xB6U);
}

TEST_CASE("DIT Read SID collides with SsmExtensionInfo SID by design",
          "[subaru_dit][atlas_sids]") {
    // Per round-9 spec §2.3 the firmware's 0xAA handler is dual-purpose:
    // empty payload returns the 104-B SSM-extension info block (what
    // round-6 caught); (addr u32 BE, length u16 BE) payload returns a
    // memory read (what DIT block-flash uses). Same SID byte, different
    // routes.
    REQUIRE(dit::kSidDitRead == dit::kSidSsmExtensionInfo);
}

// ---------------------------------------------------------------------
// DIT block-flash encoders — wire-byte pinning per round-9 spec §4
// ---------------------------------------------------------------------

TEST_CASE("build_flash_information_request emits single SID byte",
          "[subaru_dit][encode]") {
    auto const req = dit::build_flash_information_request();
    REQUIRE(req == std::vector<std::uint8_t>{0xA1});
}

TEST_CASE("build_block_information_request emits SID + 4-byte BE address",
          "[subaru_dit][encode]") {
    auto const req = dit::build_block_information_request(0xDEADBEEF);
    REQUIRE(req == std::vector<std::uint8_t>{0xA2, 0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("build_read_request emits SID + 4-byte addr + 2-byte length",
          "[subaru_dit][encode]") {
    auto const req = dit::build_read_request(0x00100000U, 0x0080U);
    REQUIRE(req == std::vector<std::uint8_t>{
        0xAA, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80});
}

TEST_CASE("build_clear_request emits SID + 4-byte BE address",
          "[subaru_dit][encode]") {
    auto const req = dit::build_clear_request(0x001E0000U);
    REQUIRE(req == std::vector<std::uint8_t>{0xAC, 0x00, 0x1E, 0x00, 0x00});
}

TEST_CASE("build_program_request emits SID + 4-byte addr + data",
          "[subaru_dit][encode]") {
    std::array<std::uint8_t, 3> const data{0x11, 0x22, 0x33};
    auto const req = dit::build_program_request(0x00006000U, data);
    REQUIRE(req == std::vector<std::uint8_t>{
        0xAD, 0x00, 0x00, 0x60, 0x00, 0x11, 0x22, 0x33});
}

TEST_CASE("build_control_request emits SID + sub-cmd + data",
          "[subaru_dit][encode]") {
    // Control(0x00) flash-entry handshake — no extra payload.
    auto const req_handshake = dit::build_control_request(0x00);
    REQUIRE(req_handshake == std::vector<std::uint8_t>{0xAF, 0x00});

    // Control(0x02) with a 1-byte sub-mode parameter.
    std::array<std::uint8_t, 1> const sub_data{0x01};
    auto const req_with_data = dit::build_control_request(0x02, sub_data);
    REQUIRE(req_with_data == std::vector<std::uint8_t>{0xAF, 0x02, 0x01});
}

// ---------------------------------------------------------------------
// DIT block-flash decoders — positive / NRC / malformed paths
// ---------------------------------------------------------------------

TEST_CASE("parse_flash_information_response unwraps positive ACK body",
          "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{0xE1, 0x01, 0x02, 0x03};
    auto const parsed = dit::parse_flash_information_response(resp);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value() == std::vector<std::uint8_t>{0x01, 0x02, 0x03});
}

TEST_CASE("parse_block_information_response extracts bounds + kind",
          "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{
        0xE2,                              // ACK = 0xA2 | 0x40
        0x00, 0x00, 0x60, 0x00,            // base = 0x00006000
        0x00, 0x00, 0x80, 0x00,            // end = 0x00008000
        0x00, 0x01,                        // kind = 0x0001
        0x00, 0x00, 0x00, 0x00,            // reserved = 0
    };
    auto const parsed = dit::parse_block_information_response(resp);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().base_address == 0x00006000U);
    REQUIRE(parsed.value().end_address == 0x00008000U);
    REQUIRE(parsed.value().block_kind == 0x0001U);
    REQUIRE(parsed.value().reserved == 0U);
    REQUIRE(parsed.value().raw.size() == 14U);
}

TEST_CASE("parse_block_information_response rejects too-short body",
          "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{0xE2, 0x00, 0x01, 0x02};
    auto const parsed = dit::parse_block_information_response(resp);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse_read_response unwraps positive ACK data",
          "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{0xEA, 0xCA, 0xFE, 0xBA, 0xBE};
    auto const parsed = dit::parse_read_response(resp);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value() == std::vector<std::uint8_t>{0xCA, 0xFE, 0xBA, 0xBE});
}

TEST_CASE("parse_clear_response accepts bare ACK", "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{0xEC};
    auto const parsed = dit::parse_clear_response(resp);
    REQUIRE(parsed.has_value());
}

TEST_CASE("parse_program_response accepts bare ACK", "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{0xED};
    auto const parsed = dit::parse_program_response(resp);
    REQUIRE(parsed.has_value());
}

TEST_CASE("parse_control_response unwraps positive ACK body",
          "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const resp{0xEF, 0xAA};
    auto const parsed = dit::parse_control_response(resp);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value() == std::vector<std::uint8_t>{0xAA});
}

TEST_CASE("decoders surface NRC as EcuRejected", "[subaru_dit][decode][nrc]") {
    // NRC 0x22 conditionsNotCorrect — likely if DSC programmingSession
    // isn't active.
    std::vector<std::uint8_t> const nrc_frame{0x7F, 0xAD, 0x22};
    auto const parsed = dit::parse_program_response(nrc_frame);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().code() == st::ErrorCode::EcuRejected);
}

TEST_CASE("decoders reject wrong ACK byte", "[subaru_dit][decode]") {
    // 0xE8 is the SSM-A8 ACK; wrong for a 0xAA-Read response.
    std::vector<std::uint8_t> const wrong_ack{0xE8, 0x00, 0x01};
    auto const parsed = dit::parse_read_response(wrong_ack);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("decoders reject empty frame", "[subaru_dit][decode]") {
    std::vector<std::uint8_t> const empty;
    auto const parsed = dit::parse_flash_information_response(empty);
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().code() == st::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------
// DitFlashClient end-to-end via MockTransport
// ---------------------------------------------------------------------

#include "st/transport/mock.hpp"

using namespace std::chrono_literals;

TEST_CASE("DitFlashClient::flash_information end-to-end via MockTransport",
          "[subaru_dit][client]") {
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    mt.expect_send_recv({0xA1}, {0xE1, 0x10, 0x20, 0x30});

    dit::DitFlashClient client{mt};
    auto const r = client.flash_information();
    REQUIRE(r.has_value());
    REQUIRE(r.value() == std::vector<std::uint8_t>{0x10, 0x20, 0x30});
}

TEST_CASE("DitFlashClient::block_information returns DitBlockInformation",
          "[subaru_dit][client]") {
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    mt.expect_send_recv(
        {0xA2, 0x00, 0x00, 0x60, 0x00},
        {0xE2,
         0x00, 0x00, 0x60, 0x00,
         0x00, 0x00, 0x80, 0x00,
         0x00, 0x02,
         0x00, 0x00, 0x00, 0x00});

    dit::DitFlashClient client{mt};
    auto const r = client.block_information(0x00006000U);
    REQUIRE(r.has_value());
    REQUIRE(r.value().base_address == 0x00006000U);
    REQUIRE(r.value().end_address == 0x00008000U);
    REQUIRE(r.value().block_kind == 0x0002U);
}

TEST_CASE("DitFlashClient::read returns requested bytes",
          "[subaru_dit][client]") {
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    mt.expect_send_recv(
        {0xAA, 0x00, 0x00, 0x60, 0x00, 0x00, 0x04},
        {0xEA, 0xDE, 0xAD, 0xBE, 0xEF});

    dit::DitFlashClient client{mt};
    auto const r = client.read(0x00006000U, 4);
    REQUIRE(r.has_value());
    REQUIRE(r.value() == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("DitFlashClient::clear succeeds on bare ACK",
          "[subaru_dit][client]") {
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    mt.expect_send_recv({0xAC, 0x00, 0x1E, 0x00, 0x00}, {0xEC});

    dit::DitFlashClient client{mt};
    auto const r = client.clear(0x001E0000U);
    REQUIRE(r.has_value());
}

TEST_CASE("DitFlashClient::program transmits SID+addr+data and accepts ACK",
          "[subaru_dit][client]") {
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    std::array<std::uint8_t, 4> const data{0xCA, 0xFE, 0xBA, 0xBE};
    mt.expect_send_recv(
        {0xAD, 0x00, 0x00, 0x60, 0x00, 0xCA, 0xFE, 0xBA, 0xBE},
        {0xED});

    dit::DitFlashClient client{mt};
    auto const r = client.program(0x00006000U, data);
    REQUIRE(r.has_value());
}

TEST_CASE("DitFlashClient::control sends bare handshake Control(0x00)",
          "[subaru_dit][client]") {
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    mt.expect_send_recv({0xAF, 0x00}, {0xEF, 0xAA});

    dit::DitFlashClient client{mt};
    auto const r = client.control(0x00);
    REQUIRE(r.has_value());
    REQUIRE(r.value() == std::vector<std::uint8_t>{0xAA});
}

TEST_CASE("DitFlashClient surfaces NRC 0x22 as EcuRejected — DSC gate closed",
          "[subaru_dit][client][nrc]") {
    // The expected bench scenario today: every DIT SID returns NRC 0x22
    // because DSC 0x10 0x02 ProgrammingSession isn't active.
    st::transport::MockTransport mt;
    REQUIRE(mt.open({}).has_value());
    mt.expect_send_recv({0xA1}, {0x7F, 0xA1, 0x22});

    dit::DitFlashClient client{mt};
    auto const r = client.flash_information();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::EcuRejected);
}
