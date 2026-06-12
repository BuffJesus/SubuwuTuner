// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tier-A skeleton tests for st::flash::SubaruShCanFlash. The class
// has two compile-time configurations:
//
//   * ST_SUBARU_ECU_FLASH_ENABLED=1 — methods return NotImplemented
//     (Tier-A skeleton; Tier-B will fill in the UDS sequence body).
//   * Default — methods return PolicyDenied + a docs/37 pointer.
//
// Each test case asserts the expected return path for whichever
// configuration the suite was built with. CI runs both; locally the
// default build exercises only the OFF path.

#include "st/flash/subaru_sh_can_flash.hpp"
#include "st/transport/mock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>

TEST_CASE("SubaruShCanFlash: build flag gates the entire surface",
          "[flash][subaru_sh_can_flash][gating]") {
    st::transport::MockTransport mock;
    REQUIRE(mock.open({}).has_value());

    st::flash::SubaruEcuFlashParams params;
    params.family = st::flash::SubaruEcuFamily::SH2A;
    params.variant = st::flash::SubaruInitVariant::CobbFlash;
    st::flash::SubaruShCanFlash f{mock, params};

    auto const open_r = f.open();
#if defined(ST_SUBARU_ECU_FLASH_ENABLED)
    REQUIRE_FALSE(open_r.has_value());
    REQUIRE(open_r.error().code() == st::ErrorCode::NotImplemented);
#else
    REQUIRE_FALSE(open_r.has_value());
    REQUIRE(open_r.error().code() == st::ErrorCode::PolicyDenied);
#endif
}

TEST_CASE("SubaruShCanFlash: every public method returns the same gated error",
          "[flash][subaru_sh_can_flash][gating]") {
    st::transport::MockTransport mock;
    REQUIRE(mock.open({}).has_value());
    st::flash::SubaruShCanFlash f{mock, st::flash::SubaruEcuFlashParams{}};

#if defined(ST_SUBARU_ECU_FLASH_ENABLED)
    constexpr auto kExpected = st::ErrorCode::NotImplemented;
#else
    constexpr auto kExpected = st::ErrorCode::PolicyDenied;
#endif

    auto const a = f.enable_flash_mode();
    REQUIRE_FALSE(a.has_value());
    REQUIRE(a.error().code() == kExpected);

    auto const b = f.erase_block(0);
    REQUIRE_FALSE(b.has_value());
    REQUIRE(b.error().code() == kExpected);

    std::array<std::uint8_t, 4> payload{0, 1, 2, 3};
    auto const c = f.flash_block(0, std::span<std::uint8_t const>{payload});
    REQUIRE_FALSE(c.has_value());
    REQUIRE(c.error().code() == kExpected);

    auto const d = f.checksum(0, 1);
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == kExpected);

    auto const e = f.dump_range(0, 1);
    REQUIRE_FALSE(e.has_value());
    REQUIRE(e.error().code() == kExpected);

    auto const g = f.close();
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().code() == kExpected);
}

TEST_CASE("SubaruShCanFlash: flash_full_rom is parseable + gated",
          "[flash][subaru_sh_can_flash][gating]") {
    // The composite flash_full_rom orchestrator's signature is the
    // load-bearing surface for callers — exercise it through the
    // entry point so a future renaming or signature change has to
    // touch a test, not just the impl.
    st::transport::MockTransport mock;
    REQUIRE(mock.open({}).has_value());
    st::flash::SubaruShCanFlash f{mock, st::flash::SubaruEcuFlashParams{}};

    std::array<std::uint8_t, 16> image{};
    int progress_calls = 0;
    auto const r = f.flash_full_rom(
        std::span<std::uint8_t const>{image},
        [&](std::uint32_t, std::uint32_t) { ++progress_calls; });
    REQUIRE_FALSE(r.has_value());
#if defined(ST_SUBARU_ECU_FLASH_ENABLED)
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
#else
    REQUIRE(r.error().code() == st::ErrorCode::PolicyDenied);
#endif
    // Gate fires before any I/O, so the progress callback shouldn't
    // have been called.
    REQUIRE(progress_calls == 0);
}
