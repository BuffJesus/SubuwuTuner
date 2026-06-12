// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Analyst-side regression coverage for the cmd 0x28 (UserInfo) parser
// against the ORIGINAL (un-scrubbed) live capture. Counterpart to
// tests/unit/devices/ap3/test_user_info_parse.cpp, which pins against
// the PII-scrubbed public fixture (`SUBTEST000` placeholder).
//
// The original capture lives at `fixtures/private/ap3_live_captures/`
// and is NOT distributed in the public repo (per the clean-room PII
// scrub recorded in
// findings/handoffs/HANDOFF-to-subuwutuner-2026-06-11-cleanroom-pii-scrub.md).
// When that dir is absent (e.g. fresh clone, CI), this test gracefully
// SKIPs; analyst-side machines staging the original captures get the
// extra confidence that nothing in the scrubbing process broke the
// parser against real-AP bytes.
//
// Tag `[.private]` hides the test from default runs — invoke explicitly
// via `st_unit_tests "[.private][ap3]"` to exercise it.

#include "st/devices/ap3/client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <vector>

namespace {

std::vector<std::uint8_t> load_private_capture(char const *name) {
    std::filesystem::path p = std::filesystem::path{ST_FIXTURE_AP3_DIR} / ".." /
                              "private" / "ap3_live_captures" / name;
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

TEST_CASE("ap3 UserInfo (private): parse against the un-scrubbed live capture",
          "[.private][devices][ap3]") {
    auto packet = load_private_capture("cmd28_userinfo_response.bin");
    if (packet.empty()) {
        SKIP("private analyst-side capture not staged "
             "(fixtures/private/ap3_live_captures/ absent — expected on public CI)");
    }
    REQUIRE(packet.size() == 130);
    REQUIRE(packet.size() >= 11U);
    std::span<std::uint8_t const> body{packet.data() + 7U, packet.size() - 7U - 4U};
    REQUIRE(body.size() == 119);

    auto fields = st::devices::ap3::parse_user_info_body(body);
    REQUIRE(fields.firmware_version.has_value());
    REQUIRE(*fields.firmware_version == "v1.7.6.0-28785");
    REQUIRE(fields.ap_product_code.has_value());
    REQUIRE(*fields.ap_product_code == "AP3-SUB-004");
    REQUIRE(fields.ap_serial.has_value());
    // Pin against the original device serial in the un-scrubbed capture.
    // Confirms the scrubbing process only modified the serial bytes,
    // didn't perturb anything else in the framing.
    REQUIRE(*fields.ap_serial == "SUB0484551");
    REQUIRE(fields.married.has_value());
    REQUIRE(*fields.married == true);
    REQUIRE(fields.vehicle.has_value());
    REQUIRE(*fields.vehicle == "2017 USDM WRX MT CCF Gen3");
}
