// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::test::bench::MockBenchPower — fully-in-memory IBenchPower
// concrete. Used by the unit-level brick-recovery fixture tests to
// drive the IBenchPower contract against a MockTransport without
// any USB / relay hardware.
//
// Power-pull integration with MockTransport: when constructed with a
// non-null `transport_to_disrupt`, dropping MainPower or BackupPower
// calls `transport_to_disrupt->inject_error(TransportUnavailable, ...)`
// so the SUT's next transport operation fails the way a real ECU
// going dark would fail. Restoring power does NOT clear a pending
// injection — the SUT must observe the failure, then a fresh
// "ECU is alive again" transport step happens (typically a new
// MockTransport with a fresh queue, mirroring how real recovery
// works: open() fails, we close, re-open, retry).
//
// MockBenchPower also offers programmable error injection on its
// OWN interface (`inject_set_error`, `inject_get_error`) so the
// fixture can exercise its handling of relay-board failures.

#ifndef ST_TEST_MOCK_BENCH_POWER_HPP
#define ST_TEST_MOCK_BENCH_POWER_HPP

#include "bench_power.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::transport {
class MockTransport; // forward — pulled in transitively via fixture sources
}

namespace st::test::bench {

class MockBenchPower : public IBenchPower {
public:
    MockBenchPower() = default;

    // If set, dropping MainPower or BackupPower triggers an injected
    // TransportUnavailable on this MockTransport so the SUT's next
    // operation fails as if the ECU went dark.
    explicit MockBenchPower(st::transport::MockTransport *transport_to_disrupt) noexcept
        : transport_to_disrupt_{transport_to_disrupt} {}

    [[nodiscard]] st::Status set(Rail rail, bool on) override;
    [[nodiscard]] st::Result<bool> get(Rail rail) override;
    [[nodiscard]] st::Status pulse_off(Rail rail, std::chrono::milliseconds duration) override;

    // Programmable injection: the next set()/get() / pulse_off() call
    // returns this error. Cleared after one use. Lets the fixture
    // test its handling of relay-board USB disconnects, etc.
    void inject_next_error(st::ErrorCode code, std::string message);

    // Test introspection — what the SUT actually called.
    struct Event {
        enum class Kind { Set, Get, PulseOff };
        Kind kind;
        Rail rail;
        bool requested_state; // for Set: the new state. for Get/PulseOff: ignored.
        std::chrono::milliseconds pulse_duration{0};
    };
    [[nodiscard]] std::vector<Event> const &events() const noexcept {
        return events_;
    }
    void clear_events() noexcept {
        events_.clear();
    }

private:
    // Three rails, each tracked independently. Default is OFF
    // (powered-down) so a fresh fixture matches the "bench is dark"
    // initial state.
    std::array<bool, 3> state_{false, false, false};
    st::transport::MockTransport *transport_to_disrupt_{nullptr};
    std::optional<std::pair<st::ErrorCode, std::string>> pending_error_;
    std::vector<Event> events_;

    [[nodiscard]] std::size_t index_of(Rail r) const noexcept {
        return static_cast<std::size_t>(r);
    }

    // Pop a pending injection if any; returns ok() otherwise.
    // Not noexcept: st::failure constructs an Error that owns a
    // std::string, which may allocate.
    [[nodiscard]] st::Status take_pending_error();
};

} // namespace st::test::bench

#endif // ST_TEST_MOCK_BENCH_POWER_HPP
