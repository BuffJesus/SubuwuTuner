// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the three RE8b-CORRECTED status probes wired into
// Client::query_state — cmd 0x2e OnHardwareType, cmd 0x30
// OnGetVehicleManufacturer, cmd 0x31 OnGetApManufacturer. All three
// are body-less reads with ASCII-string responses, same shape as
// cmd 0x04 (OnVersion).
//
// Per the corrected dispatch table at
// findings/re-2026-06-12-pm/dispatch_table_CORRECTED.md (firmware
// v1.7.6.0-28785), these cmd bytes route to dedicated handlers; on
// older firmware they fall through to the default error responder
// and we degrade to nullopt rather than failing query_state.

#include "st/devices/ets/client.hpp"
#include "st/transport/ets_packet.hpp"
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
        static_cast<std::uint32_t>(body.size() + st::transport::ets::kCrcSize);
    std::vector<std::uint8_t> p;
    p.push_back(st::transport::ets::kSyncByte0);
    p.push_back(st::transport::ets::kSyncByte1);
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

// Find the i-th OUT packet whose cmd byte is `cmd`.
std::span<std::uint8_t const>
nth_packet(std::vector<std::uint8_t> const &written, std::uint8_t cmd,
           std::size_t idx) {
    std::size_t const hs = st::transport::ets::kHeaderSize;
    std::size_t hits = 0;
    for (std::size_t i = 0; i + hs <= written.size(); ++i) {
        if (written[i] != st::transport::ets::kSyncByte0 ||
            written[i + 1] != st::transport::ets::kSyncByte1 ||
            written[i + 5] != 0x00U ||
            written[i + 6] != cmd) {
            continue;
        }
        if (hits++ != idx) {
            continue;
        }
        std::uint32_t const wire_len =
            (static_cast<std::uint32_t>(written[i + 2]) << 16) |
            (static_cast<std::uint32_t>(written[i + 3]) << 8) |
            static_cast<std::uint32_t>(written[i + 4]);
        return std::span<std::uint8_t const>{written.data() + i,
                                             hs + wire_len};
    }
    REQUIRE(false);
    return {};
}

} // namespace

TEST_CASE("ap3 query_state sends cmd 0x2e / 0x30 / 0x31 with empty bodies",
          "[devices][ap3][status_probes]") {
    st::test::transport::LoopbackByteChannel channel;
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ets::ResponseType::Info);
    // 6 IN packets — content doesn't matter; parsers degrade.
    std::array<std::uint8_t, 1> tiny{0xAAU};
    for (int i = 0; i < 6; ++i) {
        channel.queue_read(make_packet(info_type, tiny));
    }

    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());

    auto const written = channel.drain_written();
    // Each of the three new probes is a header-only packet (no body)
    // — wire_len == kCrcSize (4 bytes of zero CRC).
    auto const expect_empty_body = [&](std::uint8_t cmd) {
        auto const pkt = nth_packet(written, cmd, 0);
        std::size_t const hs = st::transport::ets::kHeaderSize;
        std::size_t const cs = st::transport::ets::kCrcSize;
        REQUIRE(pkt.size() == hs + cs); // header + CRC trailer only
        // wire_len bytes [2..4] big-endian must equal kCrcSize (4).
        std::uint32_t const wire_len =
            (static_cast<std::uint32_t>(pkt[2]) << 16) |
            (static_cast<std::uint32_t>(pkt[3]) << 8) |
            static_cast<std::uint32_t>(pkt[4]);
        REQUIRE(wire_len == cs);
    };
    expect_empty_body(0x2eU);
    expect_empty_body(0x30U);
    expect_empty_body(0x31U);
}

TEST_CASE("ap3 query_state parses ASCII payloads from cmd 0x2e / 0x30 / 0x31",
          "[devices][ap3][status_probes]") {
    st::test::transport::LoopbackByteChannel channel;
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ets::ResponseType::Info);
    // First three replies (cmd 0x28, 0x04, 0x03) — junk bodies, parsers
    // degrade. The three new cmds get realistic ASCII payloads.
    std::array<std::uint8_t, 1> tiny{0xAAU};
    channel.queue_read(make_packet(info_type, tiny));
    channel.queue_read(make_packet(info_type, tiny));
    channel.queue_read(make_packet(info_type, tiny));
    auto queue_ascii = [&](std::string const &s) {
        std::vector<std::uint8_t> body{s.begin(), s.end()};
        channel.queue_read(make_packet(info_type, body));
    };
    queue_ascii("AP-V3");
    queue_ascii("Subaru");
    queue_ascii("COBB Tuning");

    st::devices::ets::Client client{channel};
    auto const state = client.query_state();
    REQUIRE(state.has_value());
    REQUIRE(state->hardware_type.has_value());
    CHECK(*state->hardware_type == "AP-V3");
    REQUIRE(state->vehicle_manufacturer.has_value());
    CHECK(*state->vehicle_manufacturer == "Subaru");
    REQUIRE(state->ap_manufacturer.has_value());
    CHECK(*state->ap_manufacturer == "COBB Tuning");
}

TEST_CASE("ap3 query_state degrades to nullopt on non-printable status responses",
          "[devices][ap3][status_probes]") {
    // Older firmware (pre-v1.7.6.0-28785) won't have these handlers;
    // the listen() switch falls through to the default error responder
    // which emits non-printable bytes. query_state still succeeds.
    st::test::transport::LoopbackByteChannel channel;
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ets::ResponseType::Info);
    std::array<std::uint8_t, 1> tiny{0xAAU};
    channel.queue_read(make_packet(info_type, tiny)); // cmd 0x28
    channel.queue_read(make_packet(info_type, tiny)); // cmd 0x04
    channel.queue_read(make_packet(info_type, tiny)); // cmd 0x03
    // Non-printable bodies for the three new cmds — parser drops to
    // nullopt without failing query_state.
    std::array<std::uint8_t, 4> non_printable{0x01U, 0x02U, 0x03U, 0xFFU};
    channel.queue_read(make_packet(info_type, non_printable));
    channel.queue_read(make_packet(info_type, non_printable));
    channel.queue_read(make_packet(info_type, non_printable));

    st::devices::ets::Client client{channel};
    auto const state = client.query_state();
    REQUIRE(state.has_value());
    CHECK_FALSE(state->hardware_type.has_value());
    CHECK_FALSE(state->vehicle_manufacturer.has_value());
    CHECK_FALSE(state->ap_manufacturer.has_value());
    // The raw bodies are still captured for diagnostic dumps.
    CHECK(state->hardware_type_body.size() == non_printable.size());
    CHECK(state->vehicle_manufacturer_body.size() == non_printable.size());
    CHECK(state->ap_manufacturer_body.size() == non_printable.size());
}
