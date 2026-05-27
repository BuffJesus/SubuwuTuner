// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// HIL smoke test — placeholder until the real bench rig + relay are
// wired. Gated on ST_BUILD_HIL_TESTS=ON via tests/CMakeLists.txt.
//
// Today (2026-05-26) this file is a sentinel: it exists so the build
// system has at least one source under tests/hil/ to anchor the
// CMake gate against. When the junkyard ECU + relay land, replace
// the SUCCEED() with real bench coverage. The brick-recovery fixture
// it'll use (BrickRecoveryFixture + a concrete IBenchPower) is
// already shipped under tests/_helpers/.
//
// To run only HIL tests:
//   build/hil/bin/st_unit_tests.exe "[hil]"
//
// Adding a new HIL test:
//   1. Tag it `[hil][<topic>]` so it shows up under the [hil] filter.
//   2. Use SKIP() if the test needs hardware that isn't present —
//      a partially-wired bench should still run the subset of tests
//      that work with what's connected.

#include <catch2/catch_test_macros.hpp>

TEST_CASE("HIL: bench smoke — placeholder until real hardware arrives",
          "[hil][smoke]") {
    // When the junkyard ECU + relay are connected:
    //   1. open OBDX on COM5
    //   2. open NumatoBenchPower (or whatever concrete) on COMx
    //   3. construct a BrickRecoveryFixture
    //   4. run a 1-cycle batch with do_flash = Flasher::execute,
    //      do_resume = re-open transport + plan_resume + execute,
    //      do_verify = rom-pull + compare against intended image
    //   5. REQUIRE(batch.cycles_succeeded == 1)
    SUCCEED("HIL gate compiles cleanly; bench bring-up pending — see docs/28");
}
