// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unit-level tests for BrickRecoveryFixture + MockBenchPower. These
// always run in CI — they validate the FIXTURE mechanism using
// MockTransport + MockBenchPower without any USB / relay hardware.
//
// The actual junkyard-ECU HIL tests live under tests/hil/ and gate
// on ST_BUILD_HIL_TESTS=ON. Both share the same fixture, so a green
// run of these unit tests is strong evidence that the fixture will
// behave correctly the first time a real bench plus a real relay
// are wired up.

#include "../../_helpers/bench_power.hpp"
#include "../../_helpers/brick_recovery_fixture.hpp"
#include "../../_helpers/mock_bench_power.hpp"

#include "st/core/error.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace bench = st::test::bench;

TEST_CASE("BrickRecoveryFixture: AfterFlash cycle without transport disruption runs every phase",
          "[flash][bench][brick_recovery]") {
    // No transport disruption — every callback returns ok. The pull
    // still happens (AfterFlash always pulls), but with no
    // transport_to_disrupt_ wired in it's a harmless rail toggle.
    bench::MockBenchPower power;
    bench::BrickRecoveryFixture fixture{power};

    int flash_calls = 0, resume_calls = 0, verify_calls = 0;
    auto const r = fixture.run_cycle(
        bench::PullTiming::AfterFlash,
        [&] { ++flash_calls; return st::ok(); },
        [&] { ++resume_calls; return st::ok(); },
        [&] { ++verify_calls; return st::ok(); });

    REQUIRE(r.power_on_ok);
    REQUIRE(r.flash_status.has_value());        // flash ran
    REQUIRE(r.flash_status->has_value());       //   ... and was ok
    REQUIRE(r.pull_status.has_value());         // pull attempted
    REQUIRE(r.pull_status->has_value());        //   ... and the relay obeyed
    REQUIRE(r.repower_status.has_value());      // repower attempted
    REQUIRE(r.repower_status->has_value());     //   ... and the relay obeyed
    REQUIRE(r.resume_status.has_value());       // resume ran
    REQUIRE(r.resume_status->has_value());      //   ... and was ok
    REQUIRE(r.verify_status.has_value());       // verify ran
    REQUIRE(r.verify_status->has_value());      //   ... and was ok
    REQUIRE(r.succeeded());
    REQUIRE(flash_calls == 1);
    REQUIRE(resume_calls == 1);
    REQUIRE(verify_calls == 1);
}

TEST_CASE("BrickRecoveryFixture: BeforeFlash pull cuts power before flash callback runs",
          "[flash][bench][brick_recovery]") {
    st::transport::MockTransport mt;
    bench::MockBenchPower power{&mt};
    bench::BrickRecoveryFixture fixture{power};

    // Flash callback observes the transport going dark: it tries an
    // open(), gets the injected TransportUnavailable, and returns
    // that error. The resume callback then represents the recovery
    // path — re-open against a fresh transport — and the verify
    // callback confirms recovery succeeded.
    auto const r = fixture.run_cycle(
        bench::PullTiming::BeforeFlash,
        [&] {
            auto status = mt.open({}); // first call after the pull → error
            return status;
        },
        [&] { return st::ok(); },
        [&] { return st::ok(); });

    REQUIRE(r.power_on_ok);
    REQUIRE(r.pull_status.has_value());
    REQUIRE(r.pull_status->has_value());
    REQUIRE(r.repower_status.has_value());
    REQUIRE(r.repower_status->has_value());
    REQUIRE(r.flash_status.has_value());
    // Flash side saw the injected TransportUnavailable.
    REQUIRE_FALSE(r.flash_status->has_value());
    REQUIRE(r.flash_status->error().code() == st::ErrorCode::TransportUnavailable);
    // Resume + verify proceeded; cycle "succeeded" by the recovery
    // definition (verify returned ok).
    REQUIRE(r.resume_status.has_value());
    REQUIRE(r.verify_status.has_value());
    REQUIRE(r.succeeded());
}

TEST_CASE("BrickRecoveryFixture: AfterFlash pull triggers TransportUnavailable on next op",
          "[flash][bench][brick_recovery]") {
    st::transport::MockTransport mt;
    bench::MockBenchPower power{&mt};
    bench::BrickRecoveryFixture fixture{power};

    // Flash callback succeeds (no pull until after it returns).
    // Then the post-flash pull injects TransportUnavailable. The
    // resume callback observes that on its first transport op.
    auto const r = fixture.run_cycle(
        bench::PullTiming::AfterFlash,
        [&] { return st::ok(); },
        [&] {
            // First transport op after the pull → error.
            return mt.open({});
        },
        [&] { return st::ok(); });

    REQUIRE(r.flash_status.has_value());
    REQUIRE(r.flash_status->has_value()); // flash side was clean
    REQUIRE(r.pull_status.has_value());
    REQUIRE(r.pull_status->has_value());
    REQUIRE(r.repower_status.has_value());
    REQUIRE(r.repower_status->has_value());
    REQUIRE(r.resume_status.has_value());
    REQUIRE_FALSE(r.resume_status->has_value());
    REQUIRE(r.resume_status->error().code() == st::ErrorCode::TransportUnavailable);
    // verify still ran and was ok in this synthetic case — the test
    // contract is "fixture runs every phase," not "every phase has to
    // semantically succeed".
    REQUIRE(r.verify_status.has_value());
}

TEST_CASE("BrickRecoveryFixture: events log captures the rail / state / pulse args",
          "[flash][bench][brick_recovery][events]") {
    bench::MockBenchPower power;
    bench::BrickRecoveryFixture fixture{power};

    (void)fixture.run_cycle(
        bench::PullTiming::AfterFlash,
        [&] { return st::ok(); },
        [&] { return st::ok(); },
        [&] { return st::ok(); });

    // Expected sequence for an AfterFlash cycle:
    //   set(Main, true), set(Backup, true), set(Ignition, true),
    //   (flash callback),
    //   set(Main, false),  // the pull
    //   set(Main, true).   // re-power
    auto const &ev = power.events();
    REQUIRE(ev.size() == 5);
    REQUIRE(ev[0].kind == bench::MockBenchPower::Event::Kind::Set);
    REQUIRE(ev[0].rail == bench::Rail::MainPower);
    REQUIRE(ev[0].requested_state == true);
    REQUIRE(ev[1].rail == bench::Rail::BackupPower);
    REQUIRE(ev[1].requested_state == true);
    REQUIRE(ev[2].rail == bench::Rail::Ignition);
    REQUIRE(ev[2].requested_state == true);
    REQUIRE(ev[3].rail == bench::Rail::MainPower);
    REQUIRE(ev[3].requested_state == false); // the pull
    REQUIRE(ev[4].rail == bench::Rail::MainPower);
    REQUIRE(ev[4].requested_state == true); // re-power
}

TEST_CASE("BrickRecoveryFixture: run_batch stops at the first failed verify",
          "[flash][bench][brick_recovery][batch]") {
    bench::MockBenchPower power;
    bench::BrickRecoveryFixture fixture{power};

    int call_count = 0;
    auto const batch = fixture.run_batch(
        10, bench::PullTiming::AfterFlash,
        [&] { return st::ok(); },
        [&] { return st::ok(); },
        // Explicit return type — the failure / ok branches deduce to
        // different std::expected<>-shaped types otherwise.
        [&]() -> st::Status {
            ++call_count;
            // Fail verify on cycle 3 (1-indexed).
            if (call_count == 3) {
                return st::failure(st::ErrorCode::BadChecksum,
                                   "synthetic verify failure on cycle 3");
            }
            return st::ok();
        });

    REQUIRE(batch.cycles_attempted == 3);
    REQUIRE(batch.cycles_succeeded == 2);
    REQUIRE_FALSE(batch.last.succeeded());
    REQUIRE(batch.last.verify_status.has_value());
    REQUIRE(batch.last.verify_status->error().code() == st::ErrorCode::BadChecksum);
}

TEST_CASE("MockBenchPower: inject_next_error surfaces on the very next call",
          "[flash][bench][mock_bench_power]") {
    bench::MockBenchPower power;
    power.inject_next_error(st::ErrorCode::TransportUnavailable, "relay USB dropped");

    auto const r = power.set(bench::Rail::MainPower, true);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::TransportUnavailable);

    // Second call must NOT fail — injection is one-shot.
    auto const r2 = power.set(bench::Rail::MainPower, true);
    REQUIRE(r2.has_value());
}

TEST_CASE("MockBenchPower: get() returns the most-recently-set state per rail",
          "[flash][bench][mock_bench_power]") {
    bench::MockBenchPower power;
    REQUIRE(power.set(bench::Rail::MainPower, true).has_value());
    REQUIRE(power.set(bench::Rail::BackupPower, false).has_value());
    REQUIRE(power.set(bench::Rail::Ignition, true).has_value());

    auto const a = power.get(bench::Rail::MainPower);
    auto const b = power.get(bench::Rail::BackupPower);
    auto const c = power.get(bench::Rail::Ignition);
    REQUIRE(a.has_value());
    REQUIRE(*a == true);
    REQUIRE(b.has_value());
    REQUIRE(*b == false);
    REQUIRE(c.has_value());
    REQUIRE(*c == true);
}

TEST_CASE("MockBenchPower: pulse_off captures duration in events log",
          "[flash][bench][mock_bench_power]") {
    bench::MockBenchPower power;
    REQUIRE(power.set(bench::Rail::MainPower, true).has_value());
    power.clear_events();

    REQUIRE(power.pulse_off(bench::Rail::MainPower, std::chrono::milliseconds{42}).has_value());

    // pulse_off internally calls set(false) then set(true), preceded
    // by the PulseOff event itself.
    auto const &ev = power.events();
    REQUIRE(ev.size() == 3);
    REQUIRE(ev[0].kind == bench::MockBenchPower::Event::Kind::PulseOff);
    REQUIRE(ev[0].rail == bench::Rail::MainPower);
    REQUIRE(ev[0].pulse_duration == std::chrono::milliseconds{42});
    REQUIRE(ev[1].kind == bench::MockBenchPower::Event::Kind::Set);
    REQUIRE(ev[1].requested_state == false);
    REQUIRE(ev[2].kind == bench::MockBenchPower::Event::Kind::Set);
    REQUIRE(ev[2].requested_state == true);
}
