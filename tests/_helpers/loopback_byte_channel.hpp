// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::test::transport::LoopbackByteChannel — programmable in-memory
// IByteChannel double. Tests construct one with a script of expected
// request/response pairs (or queue responses directly) and assert the
// driver under test sent the right bytes and consumed the right ones.
//
// Why a dedicated double and not an existing fake:
//   * MockTransport sits one layer up (it speaks framed PDUs, not raw
//     bytes) and isn't reusable for raw-byte protocol tests.
//   * tests/unit/transport/test_native_transport.cpp has a local
//     FakeChannel, but it's file-scoped and tuned to native::Transport;
//     copy-pasting it ties two unrelated tests together. A clean
//     header-only helper is cheaper than the duplication.
//
// Design choices:
//   * write_bytes appends to `written_` so tests can assert exact
//     wire format after the call.
//   * read_bytes pops from `pending_reads_` (FIFO). Bytes within one
//     response may be split across multiple read_bytes calls if the
//     caller asks for fewer than the response carries.
//   * Empty pending queue: sleep for the requested `timeout`, then
//     return an empty vector (matches IByteChannel's "timeout, not an
//     error" contract). Sleeping (rather than returning instantly) is
//     load-bearing for consumers like `AsciiRelayBenchPower::read_until_
//     prompt`, whose outer loop relies on read_bytes consuming time
//     when no data arrives. An instant-return Loopback would let that
//     loop spin on CPU until the consumer's own deadline elapsed.
//     Tests should pass short timeouts (≤100ms) to keep CI fast.
//   * Programmable next-call failure via `inject_next_error` for tests
//     that exercise the driver's error-handling path.

#ifndef ST_TEST_LOOPBACK_BYTE_CHANNEL_HPP
#define ST_TEST_LOOPBACK_BYTE_CHANNEL_HPP

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace st::test::transport {

class LoopbackByteChannel final : public st::transport::IByteChannel {
public:
    LoopbackByteChannel() = default;

    [[nodiscard]] st::Status write_bytes(std::span<std::uint8_t const> bytes) override {
        if (auto pending = take_pending_error(); !pending.has_value()) {
            return pending;
        }
        written_.insert(written_.end(), bytes.begin(), bytes.end());
        return st::ok();
    }

    [[nodiscard]] st::Result<std::vector<std::uint8_t>>
    read_bytes(std::size_t max_bytes, std::chrono::milliseconds timeout) override {
        if (auto pending = take_pending_error(); !pending.has_value()) {
            return st::failure(pending.error());
        }
        if (pending_reads_.empty()) {
            // Sleep for the full timeout, then return empty. See header
            // doc for why this is load-bearing for the AsciiRelay
            // outer-loop spin-loop avoidance.
            if (timeout.count() > 0) {
                std::this_thread::sleep_for(timeout);
            }
            return std::vector<std::uint8_t>{}; // timeout, not an error
        }
        auto &front = pending_reads_.front();
        std::size_t const take = std::min(max_bytes, front.size());
        auto const take_diff = static_cast<std::vector<std::uint8_t>::difference_type>(take);
        std::vector<std::uint8_t> out(front.begin(), front.begin() + take_diff);
        front.erase(front.begin(), front.begin() + take_diff);
        if (front.empty()) {
            pending_reads_.pop_front();
        }
        return out;
    }

    // Queue a future read response. Multiple calls queue more responses;
    // each read_bytes call drains from the front. Bytes within one
    // response may be split across multiple read_bytes calls if the
    // caller asks for fewer than the response carries.
    void queue_read(std::span<std::uint8_t const> bytes) {
        pending_reads_.emplace_back(bytes.begin(), bytes.end());
    }
    void queue_read(std::string_view s) {
        std::vector<std::uint8_t> bytes(s.begin(), s.end());
        pending_reads_.emplace_back(std::move(bytes));
    }

    // Make the next write_bytes or read_bytes return this error.
    // Cleared after one use.
    void inject_next_error(st::ErrorCode code, std::string message) {
        pending_error_ = std::make_pair(code, std::move(message));
    }

    // Drain everything the SUT has written so far. Useful for
    // step-by-step assertions on each command in a sequence.
    [[nodiscard]] std::vector<std::uint8_t> drain_written() {
        return std::exchange(written_, {});
    }

    [[nodiscard]] std::string drain_written_as_string() {
        auto bytes = drain_written();
        return std::string{bytes.begin(), bytes.end()};
    }

    [[nodiscard]] std::vector<std::uint8_t> const &written() const noexcept {
        return written_;
    }

    [[nodiscard]] std::size_t pending_read_count() const noexcept {
        return pending_reads_.size();
    }

private:
    std::vector<std::uint8_t> written_;
    std::deque<std::vector<std::uint8_t>> pending_reads_;
    std::optional<std::pair<st::ErrorCode, std::string>> pending_error_;

    [[nodiscard]] st::Status take_pending_error() {
        if (!pending_error_) {
            return st::ok();
        }
        auto [code, msg] = std::move(*pending_error_);
        pending_error_.reset();
        return st::failure(code, std::move(msg));
    }
};

} // namespace st::test::transport

#endif // ST_TEST_LOOPBACK_BYTE_CHANNEL_HPP
