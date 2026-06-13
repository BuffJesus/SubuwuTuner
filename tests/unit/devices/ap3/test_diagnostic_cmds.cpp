// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the diagnostic-cmd env-var gate (workstream G) — cmd
// 0x05 OnFilesystem::remountUser and cmd 0x1f OnCapabilities, both
// hidden behind ST_AP3_ENABLE_DIAGNOSTIC_CMDS=1. Off by default
// because the firmware-version uncertainty applies (RE wave 3
// dispatch table is from binary v1.7.6.0-28785; live behavior may
// vary across AP builds).

#include "st/devices/ap3/client.hpp"
#include "st/transport/cobb_ap_packet.hpp"
#include "../../../_helpers/loopback_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

// RAII wrapper: set the diagnostic env-var on construction, clear on
// destruction. Catch2 sections share process state, so the wrapper
// guarantees the env-var doesn't leak between tests even if a REQUIRE
// fails mid-case.
struct DiagnosticEnvGuard {
    DiagnosticEnvGuard() {
#if defined(_WIN32)
        _putenv("ST_AP3_ENABLE_DIAGNOSTIC_CMDS=1");
#else
        setenv("ST_AP3_ENABLE_DIAGNOSTIC_CMDS", "1", 1);
#endif
    }
    ~DiagnosticEnvGuard() {
#if defined(_WIN32)
        _putenv("ST_AP3_ENABLE_DIAGNOSTIC_CMDS=");
#else
        unsetenv("ST_AP3_ENABLE_DIAGNOSTIC_CMDS");
#endif
    }
};

void prime_warmup(st::test::transport::LoopbackByteChannel &channel) {
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info);
    std::array<std::uint8_t, 1> tiny{0xAAU};
    for (int i = 0; i < 6; ++i) {
        channel.queue_read(make_packet(info_type, tiny));
    }
}

} // namespace

TEST_CASE("ap3 remount_user_filesystem refuses without env-var",
          "[devices][ap3][diagnostic][cmd05]") {
    // No DiagnosticEnvGuard — env-var stays unset.
    st::test::transport::LoopbackByteChannel channel;
    st::devices::ap3::Client client{channel};
    auto const r = client.remount_user_filesystem();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);
    // Refuse fires before any wire I/O.
    REQUIRE(channel.drain_written().empty());
}

TEST_CASE("ap3 remount_user_filesystem with env-var sends cmd 0x05 (block bypass)",
          "[devices][ap3][diagnostic][cmd05]") {
    DiagnosticEnvGuard guard;
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel); // ensure_session_warmup probes
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info);
    std::array<std::uint8_t, 1> tiny{0xAAU};
    channel.queue_read(make_packet(info_type, tiny)); // cmd 0x05 ack

    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{200};
    st::devices::ap3::Client client{channel, cfg};
    auto const r = client.remount_user_filesystem();
    REQUIRE(r.has_value());

    // Verify a cmd 0x05 packet hit the wire — would normally be
    // refused by is_blocked_command, so the bypass is load-bearing.
    auto const written = channel.drain_written();
    bool found_cmd05 = false;
    std::size_t const hs = st::transport::ap3::kHeaderSize;
    for (std::size_t i = 0; i + hs <= written.size(); ++i) {
        if (written[i] == st::transport::ap3::kSyncByte0 &&
            written[i + 1] == st::transport::ap3::kSyncByte1 &&
            written[i + 5] == 0x00U && written[i + 6] == 0x05U) {
            found_cmd05 = true;
            break;
        }
    }
    REQUIRE(found_cmd05);
}

TEST_CASE("ap3 get_capabilities refuses without env-var",
          "[devices][ap3][diagnostic][cmd1f]") {
    st::test::transport::LoopbackByteChannel channel;
    st::devices::ap3::Client client{channel};
    auto const r = client.get_capabilities();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);
    REQUIRE(channel.drain_written().empty());
}

TEST_CASE("ap3 get_capabilities with env-var returns the raw body",
          "[devices][ap3][diagnostic][cmd1f]") {
    DiagnosticEnvGuard guard;
    st::test::transport::LoopbackByteChannel channel;
    prime_warmup(channel);
    auto const info_type =
        static_cast<std::uint8_t>(st::transport::ap3::ResponseType::Info);
    // Synthetic capability blob — body shape is un-RE'd; the API
    // contract is "raw bytes returned for caller to inspect."
    std::array<std::uint8_t, 8> body{0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD};
    channel.queue_read(make_packet(info_type, body));

    st::devices::ap3::ClientConfig cfg{};
    cfg.io_timeout = std::chrono::milliseconds{200};
    st::devices::ap3::Client client{channel, cfg};
    auto const r = client.get_capabilities();
    REQUIRE(r.has_value());
    REQUIRE(r->size() == body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        REQUIRE((*r)[i] == body[i]);
    }
}
