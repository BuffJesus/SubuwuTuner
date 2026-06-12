// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Validates Client::receive_packet_body's zero-CRC tolerance. The AP
// firmware empirically emits 0x00000000 as the CRC trailer on cmd 0x21
// (ReadFile DATA) responses — observed 2026-06-12 against a real
// `/backupcksum` pull. The receive path treats an all-zero CRC trailer
// as a sentinel meaning "AP skipped CRC computation" and returns the
// body unchecked rather than refusing with BadChecksum. Non-zero CRC
// mismatches still surface as BadChecksum.

#include "st/devices/ap3/client.hpp"
#include "st/transport/cobb_ap_channel.hpp"
#include "st/transport/cobb_ap_packet.hpp"
#include "../../../_helpers/loopback_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace {

// Build a complete AP3 response packet with header + body + a 4-byte
// CRC trailer of `crc`. Used to inject controlled zero-CRC vs non-zero
// non-matching CRC into the LoopbackByteChannel.
std::vector<std::uint8_t> make_packet(std::uint8_t type,
                                      std::span<std::uint8_t const> body,
                                      std::uint32_t crc) {
    std::uint32_t const wire_len =
        static_cast<std::uint32_t>(body.size() + st::transport::ap3::kCrcSize);
    std::vector<std::uint8_t> p;
    p.reserve(st::transport::ap3::kHeaderSize + body.size() + st::transport::ap3::kCrcSize);
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

} // namespace

TEST_CASE("ap3 client accepts zero-CRC trailer as AP firmware sentinel",
          "[devices][ap3][zero_crc]") {
    st::test::transport::LoopbackByteChannel channel;
    // query_state issues 3 receives (cmd 0x28, cmd 0x04, cmd 0x03). The
    // body content doesn't matter for this test — parse_user_info_body
    // and the firmware/settings parsers all degrade to nullopt fields
    // when the body shape doesn't match, and query_state itself returns
    // success regardless.
    std::array<std::uint8_t, 1> tiny_body{0xAA};
    auto pkt = make_packet(static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info),
                           tiny_body, 0x00000000U);
    channel.queue_read(pkt); // cmd 0x28 response with ZERO CRC
    channel.queue_read(pkt); // cmd 0x04 response with ZERO CRC
    channel.queue_read(pkt); // cmd 0x03 response with ZERO CRC

    st::devices::ap3::Client client{channel};
    auto state = client.query_state();
    REQUIRE(state.has_value());
    // Each receive should have produced the 1-byte body, not a
    // BadChecksum error frame.
    REQUIRE(state->user_info_body.size() == 1U);
    REQUIRE(state->user_info_body[0] == 0xAA);
    REQUIRE(state->firmware_body.size() == 1U);
    REQUIRE(state->device_settings_body.size() == 1U);
}

TEST_CASE("ap3 client rejects non-zero CRC mismatch",
          "[devices][ap3][zero_crc]") {
    st::test::transport::LoopbackByteChannel channel;
    std::array<std::uint8_t, 1> tiny_body{0xAA};
    // A non-zero but wrong CRC still trips BadChecksum — the tolerance
    // is specifically for the all-zero sentinel, not "any mismatch".
    auto pkt = make_packet(static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info),
                           tiny_body, 0xDEADBEEFU);
    channel.queue_read(pkt);

    st::devices::ap3::Client client{channel};
    auto state = client.query_state();
    REQUIRE_FALSE(state.has_value());
    // BadChecksum bubbles up unchanged.
    REQUIRE(state.error().code() == st::ErrorCode::BadChecksum);
}
