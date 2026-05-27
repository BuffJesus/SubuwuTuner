// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::test::bench::BrickRecoveryFixture — orchestrates one
// "execute → pull power → re-power → resume → verify" cycle.
//
// The Phase-4 ship gate (docs/08-testing-strategy.md Tier 4 and
// docs/28-bench-rig-build.md Phase 6) requires 100 consecutive cycles
// of this loop against a junkyard ECU. This class is the engine that
// drives that loop, whether the underlying power source is a real
// USB relay (NumatoBenchPower etc., not yet shipped) or a
// MockBenchPower-backed MockTransport (unit tests, lands in CI today).
//
// Design intent: the fixture knows about the CYCLE but not about
// Flasher. The caller supplies callbacks that run the actual flash
// and resume operations. Real-hardware tests under tests/hil/ pass
// callbacks that call Flasher::execute / plan_resume; unit-level
// tests pass callbacks that record what they were asked to do.
//
// Why callbacks rather than holding a Flasher directly:
//
//   * Flasher's constructor takes a transport reference. After a
//     power pull, the real recovery flow closes the existing
//     transport and opens a fresh one (the ECU has rebooted). Owning
//     Flasher in the fixture would force a single transport for the
//     whole cycle, which doesn't match the real-world shape.
//
//   * Different test plans want different flash sequences (read-only,
//     read+verify, write+verify, write-with-deliberate-corruption-
//     mid-flash). Callbacks let one fixture drive all of them
//     without parameterising every option.

#ifndef ST_TEST_BRICK_RECOVERY_FIXTURE_HPP
#define ST_TEST_BRICK_RECOVERY_FIXTURE_HPP

#include "bench_power.hpp"

#include "st/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace st::test::bench {

// When in the cycle the power pull happens. The fixture either:
//   * pulls power BEFORE the flash callback runs (BeforeFlash) — tests
//     the SUT's behaviour when the ECU was already dark on entry;
//   * pulls power AFTER the flash callback returns (AfterFlash) —
//     tests resume against a fully-completed write that didn't get
//     to commit-its-manifest;
//   * pulls power MID-flash (DuringFlash) — the realistic case. The
//     callback itself is responsible for tripping the power pull at
//     a chosen moment (typically by reaching a specific PDU and
//     calling power.set(MainPower, false) before continuing).
//
// For the scaffolding lands today we only validate BeforeFlash +
// AfterFlash against MockTransport; DuringFlash requires a more
// elaborate transport stub that exposes a "pause here and pull"
// callback. That stub lands when the real bench arrives.
enum class PullTiming {
    BeforeFlash,
    AfterFlash,
    DuringFlash, // reserved — caller must trigger inside `do_flash`
};

// Per-cycle outcome. The fixture reports each sub-phase so the test
// can assert exactly which steps ran vs were skipped.
//
// All *_status fields are std::optional: an engaged optional means
// the step was attempted; a disengaged optional means it was skipped
// (because an earlier step failed). To check "step ran AND succeeded"
// the consumer does `field.has_value() && field->has_value()` —
// outer has_value() = "ran", inner has_value() = "ok status".
struct CycleReport {
    bool power_on_ok{false};
    // Engaged iff the fixture attempted to drop a power rail. The
    // inner Status carries the relay-board side outcome — a
    // disengaged outer optional means no pull was scheduled for
    // this PullTiming; an engaged optional with an error inside
    // means we tried to pull and the relay board itself failed.
    std::optional<st::Status> pull_status;
    // Engaged iff the fixture attempted to restore the dropped rail.
    // Only present when pull_status is engaged AND the pull itself
    // succeeded — there's nothing to restore if the rail never dropped.
    std::optional<st::Status> repower_status;
    // Engaged iff the flash / resume / verify callback was invoked.
    // The inner Status is whatever the callback returned.
    std::optional<st::Status> flash_status;
    std::optional<st::Status> resume_status;
    std::optional<st::Status> verify_status;

    // Convenience: did the whole cycle complete the way we wanted?
    // Definition: the fixture reached the verify step AND verify
    // returned ok. Failures earlier in the cycle propagate as
    // disengaged or error-bearing status fields.
    [[nodiscard]] bool succeeded() const noexcept {
        return verify_status.has_value() && verify_status->has_value();
    }
};

class BrickRecoveryFixture {
public:
    // Callback signatures:
    //   * Flash:  do whatever the SUT does with the ECU before the
    //             pull. Typically Flasher::execute against the
    //             primed transport.
    //   * Resume: do whatever recovery looks like after the pull.
    //             Typically: re-open transport, read_manifest,
    //             plan_resume, Flasher::execute on the resumed plan.
    //   * Verify: confirm the post-recovery ROM matches the intended
    //             post-write image. Whatever the test means by
    //             "correct" — bytewise ROM diff, manifest CRC match,
    //             etc.
    using FlashFn = std::function<st::Status()>;
    using ResumeFn = std::function<st::Status()>;
    using VerifyFn = std::function<st::Status()>;

    explicit BrickRecoveryFixture(IBenchPower &power) noexcept : power_{power} {}

    // Run one cycle with the given timing + callbacks. Always returns
    // a CycleReport; status fields carry per-step error info.
    [[nodiscard]] CycleReport run_cycle(PullTiming timing, FlashFn const &do_flash,
                                        ResumeFn const &do_resume, VerifyFn const &do_verify);

    // Run a batch of N cycles in a row. Stops on the first cycle that
    // doesn't succeed and returns the index of the first failure (and
    // its CycleReport). Caller decides whether to continue or stop;
    // typical usage is "stop and dump the report for forensics."
    struct BatchReport {
        std::size_t cycles_attempted{0};
        std::size_t cycles_succeeded{0};
        CycleReport last;
    };
    [[nodiscard]] BatchReport run_batch(std::size_t n, PullTiming timing, FlashFn const &do_flash,
                                        ResumeFn const &do_resume, VerifyFn const &do_verify);

private:
    IBenchPower &power_;
};

} // namespace st::test::bench

#endif // ST_TEST_BRICK_RECOVERY_FIXTURE_HPP
