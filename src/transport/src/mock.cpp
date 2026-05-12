// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/mock.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace st::transport {

void MockTransport::expect_send_recv(std::vector<std::uint8_t> request,
                                     std::vector<std::uint8_t> response) {
    std::lock_guard const lk{mu_};
    expectations_.push_back({std::move(request), std::move(response)});
}

bool MockTransport::exhausted() const {
    std::lock_guard const lk{mu_};
    return expectations_.empty();
}

std::size_t MockTransport::remaining() const {
    std::lock_guard const lk{mu_};
    return expectations_.size();
}

void MockTransport::inject_error(ErrorCode code, std::string message) {
    std::lock_guard const lk{mu_};
    pending_error_ = InjectedError{code, std::move(message)};
}

void MockTransport::emit_streaming_frame(std::vector<std::uint8_t> data) {
    FrameCallback cb;
    {
        std::lock_guard const lk{mu_};
        cb = stream_cb_;
    }
    if (!cb) return;
    Frame f;
    f.data    = std::move(data);
    f.arrived = std::chrono::steady_clock::now();
    cb(f);
}

Status MockTransport::open(LinkConfig const &cfg) {
    std::lock_guard const lk{mu_};
    if (pending_error_.has_value()) {
        auto e         = std::move(*pending_error_);
        pending_error_ = std::nullopt;
        return failure(e.code, std::move(e.message));
    }
    open_ = true;
    cfg_  = cfg;
    return ok();
}

Status MockTransport::close() {
    std::lock_guard const lk{mu_};
    open_      = false;
    stream_cb_ = {};
    return ok();
}

Result<Frame> MockTransport::send_recv(std::span<std::uint8_t const> payload,
                                       std::chrono::milliseconds /*timeout*/) {
    std::lock_guard const lk{mu_};
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable, "mock is not open");
    }
    if (pending_error_.has_value()) {
        auto e         = std::move(*pending_error_);
        pending_error_ = std::nullopt;
        return failure(e.code, std::move(e.message));
    }
    send_log_.emplace_back(payload.begin(), payload.end());
    if (expectations_.empty()) {
        return failure(ErrorCode::Unknown, "no queued expectation for send_recv");
    }
    Exchange ex = std::move(expectations_.front());
    expectations_.pop_front();
    if (std::vector<std::uint8_t>(payload.begin(), payload.end()) != ex.request) {
        return failure(ErrorCode::InvalidArgument,
                       "send_recv payload did not match next queued expectation");
    }
    Frame f;
    f.data    = std::move(ex.response);
    f.arrived = std::chrono::steady_clock::now();
    return f;
}

Status MockTransport::send(std::span<std::uint8_t const> payload) {
    std::lock_guard const lk{mu_};
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable, "mock is not open");
    }
    if (pending_error_.has_value()) {
        auto e         = std::move(*pending_error_);
        pending_error_ = std::nullopt;
        return failure(e.code, std::move(e.message));
    }
    send_log_.emplace_back(payload.begin(), payload.end());
    return ok();
}

Status MockTransport::start_streaming(FrameCallback callback) {
    std::lock_guard const lk{mu_};
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable, "mock is not open");
    }
    stream_cb_ = std::move(callback);
    return ok();
}

Status MockTransport::stop_streaming() {
    std::lock_guard const lk{mu_};
    stream_cb_ = {};
    return ok();
}

} // namespace st::transport
