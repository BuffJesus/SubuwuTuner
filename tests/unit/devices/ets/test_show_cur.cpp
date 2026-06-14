// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// T22 — query_currently_flashed regression. Pins the cmd 0x1a wire
// format (Boost-archived vector<string>{"show_cur no_comms"}) and
// the chunk-drain + reflash= line-parse contract that the AP browser's
// connect pane depends on.

#include "st/devices/ets/client.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/transport/ets_packet.hpp"
#include "../../../_helpers/loopback_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#ifdef ST_HAVE_AP_WORKFLOW

// gcc 15 -O3 inlines the helper-vector path aggressively enough that
// -Wfree-nonheap-object fires inside the standard library's
// vector::reserve(). The trigger is the test-harness pattern of
// allocating a small response packet per chunk, not the production
// code under test. Suppress just for this TU — the warning is
// genuinely useful elsewhere.
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

namespace {

std::vector<std::uint8_t>
make_packet(std::uint8_t type, std::span<std::uint8_t const> body) {
    std::uint32_t const wire_len =
        static_cast<std::uint32_t>(body.size() +
                                    st::transport::ets::kCrcSize);
    std::vector<std::uint8_t> p;
    p.reserve(st::transport::ets::kHeaderSize + body.size() +
               st::transport::ets::kCrcSize);
    p.push_back(st::transport::ets::kSyncByte0);
    p.push_back(st::transport::ets::kSyncByte1);
    p.push_back(static_cast<std::uint8_t>((wire_len >> 16) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>((wire_len >> 8) & 0xFFU));
    p.push_back(static_cast<std::uint8_t>(wire_len & 0xFFU));
    p.push_back(0x00U); // reserved
    p.push_back(type);
    p.insert(p.end(), body.begin(), body.end());
    // CRC zero — the wire reader tolerates a zero CRC trailer for
    // cmd 0x21 data frames; cmd 0x1a chunk frames follow the same
    // wire shape, so the zero CRC is acceptable for the loopback.
    p.push_back(0U);
    p.push_back(0U);
    p.push_back(0U);
    p.push_back(0U);
    return p;
}

void prime_warmup(st::test::transport::LoopbackByteChannel &channel,
                   std::size_t extra_response_packets) {
    std::array<std::uint8_t, 1> tiny_body{0xAAU};
    auto warmup_pkt = make_packet(
        static_cast<std::uint8_t>(st::transport::ets::ResponseType::Info),
        tiny_body);
    // 6 packets for the query_state() warmup
    // (cmd 0x28 + 0x04 + 0x03 + 0x2e + 0x30 + 0x31).
    for (std::size_t i = 0; i < 6 + extra_response_packets; ++i) {
        channel.queue_read(warmup_pkt);
    }
}

[[nodiscard]] std::vector<std::uint8_t>
text_packet(std::string_view body) {
    std::vector<std::uint8_t> body_bytes;
    body_bytes.reserve(body.size());
    for (char c : body) {
        body_bytes.push_back(static_cast<std::uint8_t>(c));
    }
    return make_packet(
        static_cast<std::uint8_t>(st::transport::ets::ResponseType::Info),
        std::span<std::uint8_t const>{body_bytes});
}

} // namespace

TEST_CASE("ap3 query_currently_flashed parses reflash= line",
          "[devices][ap3][cmd1a][show_cur]") {
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, 0);
    // The chunked response: a few stdout lines, then the terminator.
    // Each chunk is a separate packet body. The reflash= line is in
    // the middle of the stream.
    channel.queue_read(text_packet("reading cfg file for realtime info\n"));
    channel.queue_read(text_packet("defaulting using_realtime=yes\n"));
    channel.queue_read(text_packet(
        "reflash=/user/ap-user/maps/Fehr_WRK3.ptm\n"));
    channel.queue_read(text_packet("[RUNTASK_FINISH]"));

    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{100};
    cfg.file_data_io_timeout = std::chrono::milliseconds{100};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    auto flashed = client.query_currently_flashed();
    REQUIRE(flashed.has_value());
    CHECK(*flashed == "/user/ap-user/maps/Fehr_WRK3.ptm");
}

TEST_CASE("ap3 query_currently_flashed handles terminator with trailing newline",
          "[devices][ap3][cmd1a][show_cur]") {
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, 0);
    channel.queue_read(text_packet(
        "reflash=/user/ap-user/maps/Stage1 v401.ptm\n"));
    channel.queue_read(text_packet("[RUNTASK_FINISH]\n"));

    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{100};
    cfg.file_data_io_timeout = std::chrono::milliseconds{100};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    auto flashed = client.query_currently_flashed();
    REQUIRE(flashed.has_value());
    CHECK(*flashed == "/user/ap-user/maps/Stage1 v401.ptm");
}

TEST_CASE("ap3 query_currently_flashed returns ParseError when no reflash= line",
          "[devices][ap3][cmd1a][show_cur]") {
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, 0);
    channel.queue_read(text_packet("some unrelated output\n"));
    channel.queue_read(text_packet("[RUNTASK_FINISH]"));

    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{100};
    cfg.file_data_io_timeout = std::chrono::milliseconds{100};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    auto flashed = client.query_currently_flashed();
    REQUIRE_FALSE(flashed.has_value());
    CHECK(flashed.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("ap3 query_currently_flashed sends cmd 0x1a with runtask body",
          "[devices][ap3][cmd1a][show_cur][wire]") {
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, 0);
    channel.queue_read(text_packet("reflash=/user/ap-user/maps/X.ptm\n"));
    channel.queue_read(text_packet("[RUNTASK_FINISH]"));

    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{100};
    cfg.file_data_io_timeout = std::chrono::milliseconds{100};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    (void)client.query_currently_flashed();
    auto const written = channel.drain_written();
    // Find the cmd 0x1a packet in the outbound stream.
    bool found_cmd1a = false;
    std::size_t const hs = st::transport::ets::kHeaderSize;
    for (std::size_t i = 0; i + hs <= written.size(); ++i) {
        if (written[i] != st::transport::ets::kSyncByte0 ||
            written[i + 1] != st::transport::ets::kSyncByte1 ||
            written[i + 5] != 0x00U ||
            written[i + 6] != 0x1aU) {
            continue;
        }
        found_cmd1a = true;
        std::uint32_t const wire_len =
            (static_cast<std::uint32_t>(written[i + 2]) << 16) |
            (static_cast<std::uint32_t>(written[i + 3]) << 8) |
            static_cast<std::uint32_t>(written[i + 4]);
        // Body is wire_len - 4 (the CRC trailer) bytes.
        std::size_t const body_size =
            static_cast<std::size_t>(wire_len) - st::transport::ets::kCrcSize;
        // First 35 bytes are the Boost envelope; next 4 are u32 LE
        // vector size = 1; next 4 are u32 LE string length = 17 (for
        // "show_cur no_comms"); then the bytes themselves.
        REQUIRE(body_size >= 35U + 4U + 4U + 17U);
        std::size_t const env_end = i + hs + 35U;
        // Vector size (u32 LE) = 1.
        CHECK(written[env_end + 0] == 0x01U);
        CHECK(written[env_end + 1] == 0x00U);
        CHECK(written[env_end + 2] == 0x00U);
        CHECK(written[env_end + 3] == 0x00U);
        // String length (u32 LE) = 17.
        CHECK(written[env_end + 4] == 0x11U);
        CHECK(written[env_end + 5] == 0x00U);
        CHECK(written[env_end + 6] == 0x00U);
        CHECK(written[env_end + 7] == 0x00U);
        // String bytes match the catalog token.
        std::string_view const task_str{
            reinterpret_cast<char const *>(written.data() + env_end + 8),
            17U};
        CHECK(task_str == "show_cur no_comms");
        break;
    }
    CHECK(found_cmd1a);
}

#endif // ST_HAVE_AP_WORKFLOW
