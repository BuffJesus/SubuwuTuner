// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "brick_recovery_fixture.hpp"

namespace st::test::bench {

CycleReport BrickRecoveryFixture::run_cycle(PullTiming timing, FlashFn const &do_flash,
                                            ResumeFn const &do_resume,
                                            VerifyFn const &do_verify) {
    CycleReport rep{};

    // Step 1 — power the ECU on. Main + backup high; ignition follows
    // since the brick-recovery loop matches the in-car ignition-on
    // boot sequence rather than a key-off resume.
    if (auto s = power_.set(Rail::MainPower, true); !s.has_value()) {
        return rep;
    }
    if (auto s = power_.set(Rail::BackupPower, true); !s.has_value()) {
        return rep;
    }
    if (auto s = power_.set(Rail::Ignition, true); !s.has_value()) {
        return rep;
    }
    rep.power_on_ok = true;

    // Step 2 — pre-flash pull (BeforeFlash only). Cuts main power
    // before the SUT gets to talk to the ECU; the flash callback
    // will observe TransportUnavailable on its first PDU. If the
    // relay-board call itself fails we capture the error on rep and
    // bail — proceeding would just confuse the report.
    if (timing == PullTiming::BeforeFlash) {
        rep.pull_status = power_.set(Rail::MainPower, false);
        if (!rep.pull_status->has_value()) {
            return rep;
        }
    }

    // Step 3 — run the flash callback. Capture its status either way;
    // a returned error is the test signaling "the flash side did
    // what we expected" (often a deliberate failure).
    rep.flash_status = do_flash();

    // Step 4 — post-flash pull (AfterFlash). Drops power after the
    // flash callback returns but before resume runs. Same relay-side
    // error handling as the BeforeFlash branch.
    if (timing == PullTiming::AfterFlash) {
        rep.pull_status = power_.set(Rail::MainPower, false);
        if (!rep.pull_status->has_value()) {
            return rep;
        }
    }

    // Step 5 — re-power. We only re-power if a pull actually dropped
    // the rail this cycle (pull_status engaged AND inner ok). The
    // DuringFlash branch (not yet exercised in the scaffolding)
    // would assign rep.pull_status inside the flash callback, so we
    // cover that too.
    if (rep.pull_status.has_value() && rep.pull_status->has_value()) {
        rep.repower_status = power_.set(Rail::MainPower, true);
        if (!rep.repower_status->has_value()) {
            return rep;
        }
    }

    // Step 6 — run the resume callback. Even if the flash callback
    // succeeded, the test may want the resume callback to be a no-op
    // that returns ok. We always invoke it so the test gets a
    // consistent contract.
    rep.resume_status = do_resume();

    // Step 7 — verify. Always called even if resume failed; the test
    // may want to assert that verify also fails (correlated failure
    // is a sign of a real brick, not a recoverable one).
    rep.verify_status = do_verify();

    return rep;
}

BrickRecoveryFixture::BatchReport
BrickRecoveryFixture::run_batch(std::size_t n, PullTiming timing, FlashFn const &do_flash,
                                ResumeFn const &do_resume, VerifyFn const &do_verify) {
    BatchReport batch{};
    for (std::size_t i = 0; i < n; ++i) {
        batch.cycles_attempted = i + 1;
        batch.last = run_cycle(timing, do_flash, do_resume, do_verify);
        if (!batch.last.succeeded()) {
            return batch; // bail on first failure
        }
        ++batch.cycles_succeeded;
    }
    return batch;
}

} // namespace st::test::bench
