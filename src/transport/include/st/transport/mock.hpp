// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_TRANSPORT_MOCK_HPP
#define ST_TRANSPORT_MOCK_HPP

#include "st/transport.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::transport {

// Test double for ITransport. Queue (request, response) pairs ahead of time
// with expect_send_recv(); the transport pops them as the system-under-test
// calls send_recv(). Mismatches between the actual send payload and the
// queued request payload are reported by returning an Error rather than
// throwing, so callers can REQUIRE the result.
//
// Use it like:
//
//   MockTransport t;
//   t.expect_send_recv({0x80, 0x10, 0xF0, 0x05, 0xA8, ...},
//                      {0x80, 0xF0, 0x10, ...});
//   t.open({LinkKind::KLine, 10400});
//   auto r = ssm.read(0x1234, 4, 100ms);
//   t.close();
class MockTransport : public ITransport {
public:
    // Queue a (request, response) pair. When the system-under-test calls
    // send_recv with a matching payload, the response frame is returned.
    void expect_send_recv(std::vector<std::uint8_t> request, std::vector<std::uint8_t> response);

    // True if every queued exchange has been consumed.
    [[nodiscard]] bool exhausted() const;
    [[nodiscard]] std::size_t remaining() const;

    // Force the next operation to return this error code. Test the
    // SUT's error handling without a real transport failure.
    void inject_error(ErrorCode code, std::string message);

    // Inject a frame as if it had arrived in streaming mode. The
    // callback registered with start_streaming() will be invoked
    // synchronously on the calling thread (which is fine for tests).
    void emit_streaming_frame(std::vector<std::uint8_t> data);

    // ---- ITransport ----------------------------------------------------

    [[nodiscard]] Status open(LinkConfig const &cfg) override;
    [[nodiscard]] Status close() override;

    [[nodiscard]] Result<Frame> send_recv(std::span<std::uint8_t const> payload,
                                          std::chrono::milliseconds timeout) override;

    [[nodiscard]] Status send(std::span<std::uint8_t const> payload) override;

    [[nodiscard]] Status start_streaming(FrameCallback callback) override;
    [[nodiscard]] Status stop_streaming() override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "MockTransport";
    }
    [[nodiscard]] std::string_view firmware() const noexcept override {
        return "mock";
    }

    // Inspection helpers for tests.
    [[nodiscard]] bool is_open() const noexcept {
        return open_;
    }
    [[nodiscard]] LinkConfig const &config() const noexcept {
        return cfg_;
    }
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> const &send_log() const noexcept {
        return send_log_;
    }

private:
    struct Exchange {
        std::vector<std::uint8_t> request;
        std::vector<std::uint8_t> response;
    };

    struct InjectedError {
        ErrorCode code;
        std::string message;
    };

    mutable std::mutex mu_;
    bool open_{false};
    LinkConfig cfg_{};
    std::deque<Exchange> expectations_;
    std::vector<std::vector<std::uint8_t>> send_log_;
    std::optional<InjectedError> pending_error_;
    FrameCallback stream_cb_;
};

} // namespace st::transport

#endif // ST_TRANSPORT_MOCK_HPP
