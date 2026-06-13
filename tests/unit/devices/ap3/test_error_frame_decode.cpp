// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the H3 daze-body decoder in `Client::receive_packet_body`.
// Per RE wave 4 default-handler decode: the AP's listen() default
// handler emits `type=0x02` (Error) responses with body = `itoa(cmd)`
// (ASCII decimal). Bodies "32" / "38" / "40" observed in §4.2 daze
// reports decode to cmds 50 / 56 / 64.

#include "st/devices/ap3/client.hpp"
#include "st/transport/cobb_ap_packet.hpp"
#include "../../../_helpers/loopback_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
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
    p.push_back(0x00U);
    p.push_back(type);
    p.insert(p.end(), body.begin(), body.end());
    p.push_back(static_cast<std::uint8_t>((crc >> 24) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>((crc >> 16) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return p;
}

} // namespace

TEST_CASE("ap3 error-frame decoder maps ASCII-decimal body to cmd byte",
          "[devices][ap3][error_frame]") {
    // Body "64" (ASCII bytes 0x36 0x34) decodes to cmd 0x40 — a
    // realistic default-handler reject for an out-of-range cmd. The
    // existing error message keeps the raw body=64 prefix; the new
    // suffix names the decoded cmd byte so the user knows what got
    // rejected.
    st::test::transport::LoopbackByteChannel channel;
    std::array<std::uint8_t, 2> body{'6', '4'};
    channel.queue_read(make_packet(
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Error), body));

    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{200};
    st::devices::ap3::Client client{channel, cfg};
    // Drive query_state to trigger receive_packet_body — it sends cmd
    // 0x28 first, then reads back the error frame we queued.
    auto const r = client.query_state();
    REQUIRE_FALSE(r.has_value());
    auto const msg = std::string{r.error().message()};
    // Raw body hex still present (load-bearing for diagnostics).
    REQUIRE(msg.find("body=3634") != std::string::npos);
    // Decoded cmd byte present.
    REQUIRE(msg.find("cmd 0x40") != std::string::npos);
    REQUIRE(msg.find("default-handler") != std::string::npos);
}

TEST_CASE("ap3 error-frame decoder leaves non-decimal bodies alone",
          "[devices][ap3][error_frame]") {
    // Body 0xDE 0xAD doesn't decode as ASCII decimal — the decoder
    // must NOT append the default-handler suffix.
    st::test::transport::LoopbackByteChannel channel;
    std::array<std::uint8_t, 2> body{0xDE, 0xAD};
    channel.queue_read(make_packet(
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Error), body));

    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{200};
    st::devices::ap3::Client client{channel, cfg};
    auto const r = client.query_state();
    REQUIRE_FALSE(r.has_value());
    auto const msg = std::string{r.error().message()};
    REQUIRE(msg.find("body=DEAD") != std::string::npos);
    REQUIRE(msg.find("default-handler") == std::string::npos);
}
