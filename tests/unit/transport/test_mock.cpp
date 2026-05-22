// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/transport.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("MockTransport rejects calls before open()", "[transport][mock]") {
    st::transport::MockTransport t;
    std::array<std::uint8_t, 1> const payload{0};
    auto const r = t.send_recv(payload, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}

TEST_CASE("MockTransport open + send_recv matches queued expectation", "[transport][mock]") {
    st::transport::MockTransport t;
    t.expect_send_recv({0x01, 0x02, 0x03}, {0x10, 0x20});

    REQUIRE(t.open({st::transport::LinkKind::KLine, 10400}).has_value());
    REQUIRE(t.is_open());
    REQUIRE(t.config().baud == 10400);

    std::array<std::uint8_t, 3> const req{0x01, 0x02, 0x03};
    auto const r = t.send_recv(req, 10ms);
    REQUIRE(r.has_value());
    REQUIRE(r->data == std::vector<std::uint8_t>{0x10, 0x20});
    REQUIRE(t.exhausted());
    REQUIRE(t.send_log().size() == 1);
}

TEST_CASE("MockTransport flags a request mismatch", "[transport][mock]") {
    st::transport::MockTransport t;
    t.expect_send_recv({0x01, 0x02}, {0xAA});
    REQUIRE(t.open({}).has_value());
    std::array<std::uint8_t, 2> const bad{0x99, 0x88};
    auto const r = t.send_recv(bad, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("MockTransport returns Unknown when no expectation is queued", "[transport][mock]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    std::array<std::uint8_t, 1> const req{0};
    auto const r = t.send_recv(req, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::Unknown);
}

TEST_CASE("MockTransport replays multiple queued exchanges in order", "[transport][mock]") {
    st::transport::MockTransport t;
    t.expect_send_recv({0x01}, {0xA});
    t.expect_send_recv({0x02}, {0xB});
    t.expect_send_recv({0x03}, {0xC});
    REQUIRE(t.open({}).has_value());
    REQUIRE(t.remaining() == 3);

    for (std::uint8_t i = 1; i <= 3; ++i) {
        std::array<std::uint8_t, 1> const req{i};
        auto const r = t.send_recv(req, 10ms);
        REQUIRE(r.has_value());
        REQUIRE(r->data.size() == 1);
    }
    REQUIRE(t.exhausted());
}

TEST_CASE("MockTransport::inject_error fires on the next call", "[transport][mock]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    t.inject_error(st::ErrorCode::TransportTimeout, "fake timeout");

    std::array<std::uint8_t, 1> const req{0};
    auto const r = t.send_recv(req, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportTimeout);

    // The injected error is one-shot; the next call falls through.
    t.expect_send_recv({0xAA}, {0xBB});
    std::array<std::uint8_t, 1> const req2{0xAA};
    REQUIRE(t.send_recv(req2, 10ms).has_value());
}

TEST_CASE("MockTransport streaming callback delivers emitted frames",
          "[transport][mock][streaming]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());

    std::atomic<int> frames_received{0};
    std::vector<std::uint8_t> last_payload;
    REQUIRE(t.start_streaming([&](st::transport::Frame const &f) {
                 ++frames_received;
                 last_payload = f.data;
             }).has_value());

    t.emit_streaming_frame({0xDE, 0xAD, 0xBE, 0xEF});
    REQUIRE(frames_received == 1);
    REQUIRE(last_payload == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});

    t.emit_streaming_frame({0x42});
    REQUIRE(frames_received == 2);

    REQUIRE(t.stop_streaming().has_value());
    t.emit_streaming_frame({0xFF});
    REQUIRE(frames_received == 2); // no-op after stop
}

TEST_CASE("MockTransport::close clears streaming and open state", "[transport][mock]") {
    st::transport::MockTransport t;
    REQUIRE(t.open({}).has_value());
    REQUIRE(t.start_streaming([](auto const &) {}).has_value());
    REQUIRE(t.close().has_value());
    REQUIRE_FALSE(t.is_open());

    std::array<std::uint8_t, 1> const req{0};
    auto const r = t.send_recv(req, 10ms);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);
}
