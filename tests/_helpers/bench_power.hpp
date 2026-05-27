// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::test::bench::IBenchPower — abstract interface for controlling
// power to a bench ECU.
//
// This header lives under tests/_helpers/ because the bench rig is a
// testing concern, not a shipping product. The shipping binary
// (subuwutuner-cli + subuwutuner-gui) does not link this; only the
// st_unit_tests target does. When a real relay board concrete is
// added (NumatoBenchPower, KMtronicBenchPower, etc.), it stays under
// tests/_helpers/ or tests/hil/.
//
// Why three channels:
//   * R1 — main power (B134-12 + B134-24). The switched-via-main-relay
//     12 V the ECU sees on the car. Cut this to simulate a key-off
//     event or a brick-pull.
//   * R2 — backup power (B134-23). Always-on in the car. Cut THIS
//     too to simulate a full disconnect (e.g. battery removed).
//   * R3 — ignition signal (B134-32). Cut to simulate key-off
//     without disturbing the main relay; tests the ECU's
//     self-shutoff sequencing path.
//
// Per docs/28-bench-rig-build.md Phase 6, the brick-recovery loop
// uses R1 (and sometimes R2) to drop power at controlled timing
// relative to a flash sub-phase. `pulse_off` is the primitive: cut
// for N ms, restore. Returning a `Status` lets a caller distinguish
// "we asked for a power pull and got it" from "we asked for a power
// pull and the relay board itself failed."

#ifndef ST_TEST_BENCH_POWER_HPP
#define ST_TEST_BENCH_POWER_HPP

#include "st/core/result.hpp"

#include <chrono>

namespace st::test::bench {

// Which 12 V rail / signal to control. One enum value per relay
// channel on a 3-channel board; a 4+-channel board can map extra
// channels to user-defined functions (rear-O2 heater, etc.) outside
// this interface.
enum class Rail {
    MainPower,   // R1 — B134-12 / B134-24 switched 12 V
    BackupPower, // R2 — B134-23 always-on 12 V
    Ignition,    // R3 — B134-32 ignition switch
};

class IBenchPower {
public:
    IBenchPower() = default;
    virtual ~IBenchPower() = default;
    IBenchPower(IBenchPower const &) = delete;
    IBenchPower &operator=(IBenchPower const &) = delete;
    IBenchPower(IBenchPower &&) noexcept = default;
    IBenchPower &operator=(IBenchPower &&) noexcept = default;

    // Set the given rail to `on` (true = closed contact, 12 V applied;
    // false = open contact, rail drops). Returns TransportUnavailable
    // if the relay board itself is gone (USB disconnect, etc.).
    [[nodiscard]] virtual st::Status set(Rail rail, bool on) = 0;

    // Convenience: query the current state of a rail without
    // changing it. Mocks track this internally; real boards may have
    // to query the relay's state machine, so this is not free.
    [[nodiscard]] virtual st::Result<bool> get(Rail rail) = 0;

    // Drop the given rail for `duration`, then restore it. Equivalent
    // to set(rail, false) → sleep → set(rail, true), but the concrete
    // may use a board-native pulse command for sub-millisecond timing
    // when available. Default behaviour (when a board lacks a native
    // pulse) is the set/sleep/set sequence.
    [[nodiscard]] virtual st::Status pulse_off(Rail rail,
                                               std::chrono::milliseconds duration) = 0;
};

} // namespace st::test::bench

#endif // ST_TEST_BENCH_POWER_HPP
