// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "mock_bench_power.hpp"

#include "st/core/error.hpp"
#include "st/transport/mock.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace st::test::bench {

st::Status MockBenchPower::take_pending_error() {
    if (!pending_error_) {
        return st::ok();
    }
    auto [code, msg] = std::move(*pending_error_);
    pending_error_.reset();
    return st::failure(code, std::move(msg));
}

st::Status MockBenchPower::set(Rail rail, bool on) {
    events_.push_back(Event{Event::Kind::Set, rail, on, std::chrono::milliseconds{0}});

    if (auto pending = take_pending_error(); !pending.has_value()) {
        return pending;
    }

    bool const was_on = state_[index_of(rail)];
    state_[index_of(rail)] = on;

    // Falling edge on a power rail simulates a real-world brick-pull
    // event. We inject TransportUnavailable on the SUT's MockTransport
    // so the next read/write fails the way a real "ECU went dark"
    // would fail.
    bool const falling = was_on && !on;
    bool const is_power_rail = (rail == Rail::MainPower) || (rail == Rail::BackupPower);
    if (falling && is_power_rail && transport_to_disrupt_ != nullptr) {
        transport_to_disrupt_->inject_error(
            st::ErrorCode::TransportUnavailable,
            "mock_bench_power: power was pulled mid-operation");
    }
    return st::ok();
}

st::Result<bool> MockBenchPower::get(Rail rail) {
    events_.push_back(Event{Event::Kind::Get, rail, false, std::chrono::milliseconds{0}});
    if (auto pending = take_pending_error(); !pending.has_value()) {
        return st::failure(pending.error());
    }
    return state_[index_of(rail)];
}

st::Status MockBenchPower::pulse_off(Rail rail, std::chrono::milliseconds duration) {
    events_.push_back(Event{Event::Kind::PulseOff, rail, false, duration});
    if (auto pending = take_pending_error(); !pending.has_value()) {
        return pending;
    }

    // Simulate the pulse: rail goes false, then true. In real
    // hardware this is a board-native pulse command with sub-ms
    // timing; for the mock we just do the two state transitions
    // without an actual sleep (callers that need deterministic
    // timing should drive set() directly).
    if (auto s = set(rail, false); !s.has_value()) {
        return s;
    }
    // No actual sleep — keeps the unit tests deterministic + fast.
    // The duration is captured in the Event log so the test can
    // verify the fixture asked for the right pulse width.
    (void)duration;
    return set(rail, true);
}

void MockBenchPower::inject_next_error(st::ErrorCode code, std::string message) {
    pending_error_ = std::make_pair(code, std::move(message));
}

} // namespace st::test::bench
