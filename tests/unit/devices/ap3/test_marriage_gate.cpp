// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the marriage-gate plumbing — both the
// `parse_marriage_from_device_settings` hook (analyst RE drop-in
// point) and the `query_state` rule that the cmd 0x28 ASCII-payload
// marriage flag is authoritative over the cmd 0x03 device-settings
// hook output.
//
// Today the hook is a placeholder returning nullopt for everything;
// these tests pin the placeholder + the "don't clobber" rule so the
// analyst's drop-in is a one-line swap (replace the function body),
// not a refactor.

#include "st/devices/ap3/client.hpp"
#include "st/devices/ap3/file_info.hpp"
#include "st/transport/cobb_ap_packet.hpp"
#include "../../../_helpers/loopback_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> make_packet(std::uint8_t type,
                                      std::span<std::uint8_t const> body,
                                      std::uint32_t crc = 0U) {
    std::uint32_t const wire_len =
        static_cast<std::uint32_t>(body.size() + st::transport::ap3::kCrcSize);
    std::vector<std::uint8_t> p;
    p.push_back(st::transport::ap3::kSyncByte0);
    p.push_back(st::transport::ap3::kSyncByte1);
    p.push_back(static_cast<std::uint8_t>((wire_len >> 16) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>((wire_len >> 8) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>(wire_len & 0xFFU));
    p.push_back(0x00U); // reserved
    p.push_back(type);
    p.insert(p.end(), body.begin(), body.end());
    p.push_back(static_cast<std::uint8_t>((crc >> 24) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>((crc >> 16) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return p;
}

// Build a synthetic cmd 0x28 UserInfo response with the install state
// field set to `installed_marker` ("Installed" or "Not Installed").
// The other 5 fields are placeholders. Spec §6.13 wire shape: 30-byte
// FileInfo2 prefix + u8 0x08 + u32 LE tracking + u32 LE str_len +
// ASCII payload.
std::vector<std::uint8_t> make_userinfo_body(std::string const &installed_marker) {
    std::string const payload =
        std::string{"v1.7.6.0-28785\nAPV3-USDM\nSUBTEST000\n"} +
        installed_marker + "\n2017 USDM WRX MT\n00000";
    std::vector<std::uint8_t> body;
    // Reuse the canonical kFileInfo2Prefix (27 magic + 3 archive
    // tail). parse_user_info_body validates the first 27 bytes
    // strictly; the 3 trailing bytes are part of the prefix per
    // spec §6.13.
    body.insert(body.end(),
                st::devices::ap3::kFileInfo2Prefix.begin(),
                st::devices::ap3::kFileInfo2Prefix.end());
    body.push_back(0x08U); // archive flag at [30]
    body.insert(body.end(), {0x01U, 0x00U, 0x00U, 0x00U}); // tracking id = 1
    auto const len = static_cast<std::uint32_t>(payload.size());
    body.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    body.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    body.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFFU));
    body.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFFU));
    body.insert(body.end(), payload.begin(), payload.end());
    return body;
}

void queue_query_state(st::test::transport::LoopbackByteChannel &channel,
                       std::vector<std::uint8_t> const &userinfo_body) {
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info);
    channel.queue_read(make_packet(info_type, userinfo_body));
    std::array<std::uint8_t, 1> tiny{0xAAU};
    channel.queue_read(make_packet(info_type, tiny)); // cmd 0x04 firmware echo
    channel.queue_read(make_packet(info_type, tiny)); // cmd 0x03 device settings
}

} // namespace

TEST_CASE("parse_marriage_from_device_settings is a placeholder returning nullopt",
          "[devices][ap3][marriage]") {
    // Until the analyst pins the byte offset of the Installed marker
    // in the cmd 0x03 DeviceSettings blob (spec §15 open question),
    // this hook returns nullopt for every input. The CLI marriage gate
    // (`gate_marriage` in main.cpp) falls back to a "warn + proceed"
    // path in that case, which matches the current user-visible
    // behavior.
    //
    // When the offset lands, replace this test with a fixture-pinned
    // one loading fixtures/private/ap3_live_captures/cmd03_*.bin and
    // asserting the parsed bool agrees with the cmd 0x28 marriage for
    // that same session.
    std::array<std::uint8_t, 0> empty{};
    REQUIRE_FALSE(
        st::devices::ap3::parse_marriage_from_device_settings(empty).has_value());
    std::array<std::uint8_t, 256> garbage{};
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<std::uint8_t>(i & 0xFFU);
    }
    REQUIRE_FALSE(
        st::devices::ap3::parse_marriage_from_device_settings(garbage).has_value());
}

TEST_CASE("query_state preserves cmd 0x28 marriage flag — Installed",
          "[devices][ap3][marriage]") {
    // The cmd 0x28 ASCII payload is the authoritative marriage source
    // on current firmware (v1.7.6.0-28785). When it yields a value,
    // the cmd 0x03 hook's output must NOT clobber it — even if a
    // future analyst-side hook accidentally returns the wrong bool,
    // the cmd 0x28 result wins.
    st::test::transport::LoopbackByteChannel channel;
    queue_query_state(channel, make_userinfo_body("Installed"));
    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    st::devices::ap3::Client client{channel, cfg};
    auto state = client.query_state();
    REQUIRE(state.has_value());
    REQUIRE(state->married.has_value());
    CHECK(*state->married == true);
}

TEST_CASE("query_state preserves cmd 0x28 marriage flag — Not Installed",
          "[devices][ap3][marriage]") {
    st::test::transport::LoopbackByteChannel channel;
    queue_query_state(channel, make_userinfo_body("Not Installed"));
    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    st::devices::ap3::Client client{channel, cfg};
    auto state = client.query_state();
    REQUIRE(state.has_value());
    REQUIRE(state->married.has_value());
    CHECK(*state->married == false);
}

TEST_CASE("query_state leaves marriage nullopt when cmd 0x28 + cmd 0x03 both silent",
          "[devices][ap3][marriage]") {
    // Cmd 0x28 response with a malformed prefix → parse_user_info_body
    // returns all-nullopt → state.married = nullopt. Cmd 0x03 hook
    // returns nullopt today. Combined: state.married stays nullopt and
    // the CLI gate emits "marriage unparsed — proceeding". This is the
    // current observable behavior; pinning it makes sure a future
    // refactor doesn't accidentally default marriage to false (which
    // would break every CLI invocation on an unparsed AP).
    st::test::transport::LoopbackByteChannel channel;
    std::array<std::uint8_t, 4> garbage_prefix{0xFFU, 0xFEU, 0xFDU, 0xFCU};
    std::vector<std::uint8_t> bad_body{garbage_prefix.begin(), garbage_prefix.end()};
    queue_query_state(channel, bad_body);
    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    st::devices::ap3::Client client{channel, cfg};
    auto state = client.query_state();
    REQUIRE(state.has_value());
    CHECK_FALSE(state->married.has_value());
}
