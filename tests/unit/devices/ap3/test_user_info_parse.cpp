// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Canonical fixture pin for cmd 0x28 (UserInfo) response body parsing
// per spec §6.13. The fixture
// `code/fixtures/ap3/cmd28_userinfo_response.bin` is a 130-byte full
// response packet (7-byte header + 119-byte body + 4-byte CRC trailer)
// captured live from the analyst's married AP (PII scrubbed for
// public-repo distribution per
// findings/handoffs/HANDOFF-to-subuwutuner-2026-06-11-cleanroom-pii-scrub.md —
// original capture preserved at fixtures/private/ap3_live_captures/;
// scrubbed serial below is SUBTEST000; firmware 1.7.6.0-28785). The body
// decodes to the 6 expected ASCII fields per the response README; the
// scrubbed canonical values are pinned here.

#include "st/devices/ap3/client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <vector>

namespace {

std::vector<std::uint8_t> load_ap3_fixture(char const *name) {
    std::filesystem::path p = std::filesystem::path{ST_FIXTURE_AP3_DIR} / name;
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

} // namespace

TEST_CASE("ap3 UserInfo: parse_user_info_body pins the canonical cmd 0x28 fixture",
          "[devices][ap3][fixtures]") {
    auto packet = load_ap3_fixture("cmd28_userinfo_response.bin");
    if (packet.empty()) {
        SKIP("cmd 0x28 response fixture not staged");
    }
    REQUIRE(packet.size() == 130);
    // The fixture is a complete response packet. Strip the 7-byte
    // header (sync + wire_len + reserved + type) and the 4-byte CRC
    // trailer to get the body.
    REQUIRE(packet.size() >= 11U);
    std::span<std::uint8_t const> body{packet.data() + 7U, packet.size() - 7U - 4U};
    REQUIRE(body.size() == 119);

    auto fields = st::devices::ap3::parse_user_info_body(body);
    REQUIRE(fields.firmware_version.has_value());
    REQUIRE(*fields.firmware_version == "v1.7.6.0-28785");
    REQUIRE(fields.ap_product_code.has_value());
    REQUIRE(*fields.ap_product_code == "AP3-SUB-004");
    REQUIRE(fields.ap_serial.has_value());
    REQUIRE(*fields.ap_serial == "SUBTEST000");
    REQUIRE(fields.married.has_value());
    REQUIRE(*fields.married == true);
    REQUIRE(fields.vehicle.has_value());
    REQUIRE(*fields.vehicle == "2017 USDM WRX MT CCF Gen3");
}

TEST_CASE("ap3 UserInfo: parse_user_info_body returns nullopt fields on truncated body",
          "[devices][ap3]") {
    std::array<std::uint8_t, 20> short_body{};
    auto fields = st::devices::ap3::parse_user_info_body(short_body);
    REQUIRE_FALSE(fields.firmware_version.has_value());
    REQUIRE_FALSE(fields.ap_serial.has_value());
    REQUIRE_FALSE(fields.married.has_value());
}

TEST_CASE("ap3 UserInfo: parse_user_info_body distinguishes Installed vs Not Installed",
          "[devices][ap3]") {
    // Build a minimal synthetic body per spec §6.13 with field 4 set
    // to "Not Installed" and verify the parser flips the married flag.
    std::vector<std::uint8_t> body;
    // 30-byte FileInfo2 prefix (we only need the first 27 to match
    // exactly; the remaining 3 are opaque to the parser).
    std::array<std::uint8_t, 30> const kPrefix{
        0x16, 0x00, 0x00, 0x00, 0x73, 0x65, 0x72, 0x69, 0x61, 0x6C, 0x69, 0x7A, 0x61, 0x74, 0x69,
        0x6F, 0x6E, 0x3A, 0x3A, 0x61, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65, 0x03, 0x00, 0x00, 0x00,
    };
    body.insert(body.end(), kPrefix.begin(), kPrefix.end());
    body.push_back(0x08U); // archive flag
    auto push_u32_le = [&](std::uint32_t v) {
        body.push_back(static_cast<std::uint8_t>(v & 0xFFU));
        body.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
        body.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
        body.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFU));
    };
    push_u32_le(1U); // tracking ID
    std::string const payload =
        "v0\nP\nS\nNot Installed\n2017 vehicle\n00000";
    push_u32_le(static_cast<std::uint32_t>(payload.size()));
    body.insert(body.end(), payload.begin(), payload.end());

    auto fields = st::devices::ap3::parse_user_info_body(body);
    REQUIRE(fields.married.has_value());
    REQUIRE(*fields.married == false);
    REQUIRE(fields.firmware_version.has_value());
    REQUIRE(*fields.firmware_version == "v0");
    REQUIRE(fields.vehicle.has_value());
    REQUIRE(*fields.vehicle == "2017 vehicle");
}
