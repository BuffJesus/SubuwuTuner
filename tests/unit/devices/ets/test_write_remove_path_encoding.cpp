// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Path-encoding regression for `Client::write_file` (cmd 0x22) and
// `Client::remove_file` (cmd 0x25). Both use the same
// `strip_leading_slash(path)` convention as cmd 0x20 (already pinned in
// test_read_file_path_encoding.cpp via ef70018); without these tests a
// refactor could regress them independently and we'd only catch it
// when the AP started dazing on push / rm of subdirectory files.
//
// AP dazes (§4.2) require a physical replug to recover, so these
// regressions need to be impossible to ship.

#include "st/devices/ets/client.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/transport/ets_packet.hpp"
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
        static_cast<std::uint32_t>(body.size() + st::transport::ets::kCrcSize);
    std::vector<std::uint8_t> p;
    p.reserve(st::transport::ets::kHeaderSize + body.size() + st::transport::ets::kCrcSize);
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

std::span<std::uint8_t const>
find_cmd_packet(std::vector<std::uint8_t> const &written, std::uint8_t cmd) {
    std::size_t const hs = st::transport::ets::kHeaderSize;
    REQUIRE(written.size() >= hs);
    for (std::size_t i = 0; i + hs <= written.size(); ++i) {
        if (written[i] != st::transport::ets::kSyncByte0 ||
            written[i + 1] != st::transport::ets::kSyncByte1 ||
            written[i + 5] != 0x00U ||
            written[i + 6] != cmd) {
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
    REQUIRE(false);
    return {};
}

void prime_warmup(st::test::transport::LoopbackByteChannel &channel,
                  std::size_t extra_response_packets) {
    std::array<std::uint8_t, 1> tiny_body{0xAAU};
    auto warmup_pkt = make_packet(
        static_cast<std::uint8_t>(st::transport::ets::ResponseType::Info),
        tiny_body);
    // 6 for query_state warmup (cmd 0x28, 0x04, 0x03, 0x2e, 0x30, 0x31)
    // + caller's count.
    for (std::size_t i = 0; i < 6 + extra_response_packets; ++i) {
        channel.queue_read(warmup_pkt);
    }
}

st::devices::ets::FileInfo
decode_body(std::span<std::uint8_t const> packet) {
    std::size_t const hs = st::transport::ets::kHeaderSize;
    std::size_t const cs = st::transport::ets::kCrcSize;
    REQUIRE(packet.size() > hs + cs);
    std::span<std::uint8_t const> const body{packet.data() + hs,
                                             packet.size() - hs - cs};
    auto decoded = st::devices::ets::decode_file_info(body);
    REQUIRE(decoded.has_value());
    return *decoded;
}

} // namespace

TEST_CASE("ap3 write_file cmd 0x22 setup encodes path as full relative "
          "(maps/X.ptm), NOT directory (/maps/)",
          "[devices][ap3][regression][cmd22-path]") {
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, /*extra=*/2); // cmd 0x22 setup + cmd 0x23 data
    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    cfg.file_data_io_timeout = std::chrono::milliseconds{50};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    std::array<std::uint8_t, 4> const payload{0x01U, 0x02U, 0x03U, 0x04U};
    (void)client.write_file("/maps/MyTune.ptm",
                            std::span<std::uint8_t const>{payload});

    auto const written = channel.drain_written();
    auto const cmd22 = find_cmd_packet(written, 0x22U);
    auto const info = decode_body(cmd22);
    CHECK(info.name == "MyTune.ptm");
    CHECK(info.path == "maps/MyTune.ptm");
    CHECK(info.path != "/maps/");
}

TEST_CASE("ap3 remove_file cmd 0x25 encodes path as full relative "
          "(maps/X.ptm), NOT directory (/maps/)",
          "[devices][ap3][regression][cmd25-path]") {
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, /*extra=*/1); // cmd 0x25 ack
    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    cfg.file_data_io_timeout = std::chrono::milliseconds{50};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    (void)client.remove_file("/maps/DoomedTune.ptm");

    auto const written = channel.drain_written();
    auto const cmd25 = find_cmd_packet(written, 0x25U);
    auto const info = decode_body(cmd25);
    CHECK(info.name == "DoomedTune.ptm");
    CHECK(info.path == "maps/DoomedTune.ptm");
    CHECK(info.path != "/maps/");
}

TEST_CASE("ap3 write_file root-level path uses bare-token",
          "[devices][ap3][regression][cmd22-path]") {
    // Root-level writes (rare but valid for some AP scratch files):
    // path = "scratch.bin", not "/scratch.bin".
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel, /*extra=*/2);
    st::devices::ets::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{50};
    cfg.file_data_io_timeout = std::chrono::milliseconds{50};
    st::devices::ets::Client client{channel, cfg};
    REQUIRE(client.query_state().has_value());
    (void)channel.drain_written();

    std::array<std::uint8_t, 1> const payload{0xAAU};
    (void)client.write_file("/scratch.bin",
                            std::span<std::uint8_t const>{payload});

    auto const written = channel.drain_written();
    auto const cmd22 = find_cmd_packet(written, 0x22U);
    auto const info = decode_body(cmd22);
    CHECK(info.name == "scratch.bin");
    CHECK(info.path == "scratch.bin");
}
