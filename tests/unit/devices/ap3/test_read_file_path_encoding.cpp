// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Regression test for commit ef70018 — `Client::read_file` cmd 0x20
// setup body MUST encode the FileInfo2.path field as the full relative
// path with NO leading slash (e.g. "maps/Stage1.ptm"), not the directory
// (e.g. "/maps/"). The earlier convention triggered a degenerate setup
// ACK from the AP (name=".", size=0) which caused cmd 0x21 to return
// zero body bytes — diagnosed by running the reference Python tool's
// successful pull side-by-side and diffing OUT bytes.
//
// This test pins the OUT-bytes shape against the cmd 0x20 portion of a
// realistic read_file() call so a future refactor can't silently
// regress the path field back to the directory-only form.

#include "st/devices/ap3/client.hpp"
#include "st/devices/ap3/file_info.hpp"
#include "st/transport/cobb_ap_packet.hpp"
#include "../../../_helpers/loopback_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace {

// Mirror of test_zero_crc_tolerance.cpp's helper. Builds a complete
// AP3 response wire frame so the LoopbackByteChannel can hand it back
// to receive_packet_body for warmup + setup-ACK reads.
std::vector<std::uint8_t> make_packet(std::uint8_t type,
                                      std::span<std::uint8_t const> body,
                                      std::uint32_t crc = 0U) {
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

// Find the first cmd-0x20 OUT packet in a byte stream. Returns the
// span over `[header .. end-of-CRC]`. Throws via REQUIRE failure if
// not found.
std::span<std::uint8_t const>
find_cmd20_packet(std::vector<std::uint8_t> const &written) {
    std::size_t const hs = st::transport::ap3::kHeaderSize;
    REQUIRE(written.size() >= hs);
    for (std::size_t i = 0; i + hs <= written.size(); ++i) {
        if (written[i] != st::transport::ap3::kSyncByte0 ||
            written[i + 1] != st::transport::ap3::kSyncByte1 ||
            written[i + 5] != 0x00U ||
            written[i + 6] != 0x20U) {
            continue;
        }
        std::uint32_t const wire_len =
            (static_cast<std::uint32_t>(written[i + 2]) << 16) |
            (static_cast<std::uint32_t>(written[i + 3]) << 8) |
            static_cast<std::uint32_t>(written[i + 4]);
        std::size_t const end = i + hs + wire_len;
        REQUIRE(end <= written.size());
        return std::span<std::uint8_t const>{written.data() + i, end - i};
    }
    REQUIRE(false); // no cmd 0x20 packet found
    return {};
}

} // namespace

TEST_CASE("ap3 read_file cmd 0x20 setup encodes path as full relative "
          "(maps/Foo.ptm), NOT directory (/maps/)",
          "[devices][ap3][regression][cmd20-path]") {
    st::test::transport::LoopbackByteChannel channel;
    // Warmup: query_state issues 6 packets (cmd 0x28, 0x04, 0x03,
    // 0x2e, 0x30, 0x31) and needs 6 IN responses. Body content is
    // irrelevant — the parsers degrade gracefully to nullopt fields
    // when the shape doesn't match.
    std::array<std::uint8_t, 1> tiny_body{0xAAU};
    auto warmup_pkt = make_packet(
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info),
        tiny_body);
    for (int i = 0; i < 6; ++i) {
        channel.queue_read(warmup_pkt);
    }
    // cmd 0x20 setup ACK: a 1-byte body that won't decode as
    // FileInfo2 (so decode_setup_ack_response_shape returns nullopt
    // and the degenerate-ACK fast-fail is skipped — letting read_file
    // proceed to cmd 0x21). cmd 0x21 then reads an empty queue and
    // times out, which is fine — we're only asserting on the OUT
    // bytes already in `written_`.
    channel.queue_read(warmup_pkt);

    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    cfg.file_data_io_timeout = std::chrono::milliseconds{50};
    st::devices::ap3::Client client{channel, cfg};
    auto state = client.query_state();
    REQUIRE(state.has_value());

    // Clear the warmup OUT noise so the next drain_written gives us
    // just the read_file packets.
    (void)channel.drain_written();

    // Trigger read_file. The cmd 0x21 read will eventually fail or
    // time out — we don't care; the cmd 0x20 setup OUT bytes are
    // already captured by the time the failure surfaces.
    (void)client.read_file("/maps/Stage1.ptm");

    auto const written = channel.drain_written();
    auto const cmd20 = find_cmd20_packet(written);

    // Extract the body (between 7-byte header and 4-byte CRC) and
    // decode it as FileInfo2.
    std::size_t const hs = st::transport::ap3::kHeaderSize;
    std::size_t const cs = st::transport::ap3::kCrcSize;
    REQUIRE(cmd20.size() > hs + cs);
    std::span<std::uint8_t const> const body{cmd20.data() + hs,
                                             cmd20.size() - hs - cs};

    auto decoded = st::devices::ap3::decode_file_info(body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->name == "Stage1.ptm");
    CHECK(decoded->path == "maps/Stage1.ptm");
    // The smoking-gun regression check: path MUST NOT be the
    // directory-only form that triggered the original §4 bug.
    CHECK(decoded->path != "/maps/");
}

TEST_CASE("ap3 read_file cmd 0x20 setup for root-level pseudo-file "
          "(backupcksum) uses bare-token path",
          "[devices][ap3][regression][cmd20-path]") {
    // Root-level files like /backupcksum want path = "backupcksum"
    // (basename without slashes). The fix preserves this special case
    // implicitly because strip_leading_slash("/backupcksum") =
    // "backupcksum".
    st::test::transport::LoopbackByteChannel channel;
    std::array<std::uint8_t, 1> tiny_body{0xAAU};
    auto warmup_pkt = make_packet(
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info),
        tiny_body);
    // 6 warmup IN responses + 1 setup-ACK for cmd 0x20.
    for (int i = 0; i < 7; ++i) {
        channel.queue_read(warmup_pkt);
    }

    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    cfg.file_data_io_timeout = std::chrono::milliseconds{50};
    st::devices::ap3::Client client{channel, cfg};
    auto state = client.query_state();
    REQUIRE(state.has_value());
    (void)channel.drain_written();

    (void)client.read_file("/backupcksum");

    auto const written = channel.drain_written();
    auto const cmd20 = find_cmd20_packet(written);
    std::size_t const hs = st::transport::ap3::kHeaderSize;
    std::size_t const cs = st::transport::ap3::kCrcSize;
    std::span<std::uint8_t const> const body{cmd20.data() + hs,
                                             cmd20.size() - hs - cs};
    auto decoded = st::devices::ap3::decode_file_info(body);
    REQUIRE(decoded.has_value());
    CHECK(decoded->name == "backupcksum");
    CHECK(decoded->path == "backupcksum");
}
