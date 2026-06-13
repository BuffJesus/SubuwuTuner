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

TEST_CASE("subaru_variant_supported: SSM-III/IV cover all five tunable variants",
          "[flash][subaru_sh_can_flash][variant_matrix]") {
    using st::flash::SubaruEcuFamily;
    using st::flash::SubaruInitVariant;
    using st::flash::subaru_variant_supported;
    for (auto fam : {SubaruEcuFamily::SH7058, SubaruEcuFamily::SH2A}) {
        CHECK(subaru_variant_supported(fam, SubaruInitVariant::Factory));
        CHECK(subaru_variant_supported(fam, SubaruInitVariant::CobbFlash));
        CHECK(subaru_variant_supported(fam, SubaruInitVariant::CobbMafSd));
        CHECK(subaru_variant_supported(fam, SubaruInitVariant::Aftermarket));
        CHECK(subaru_variant_supported(fam, SubaruInitVariant::EcuTek));
        // SSM-V factory is meaningful only on RH850-era — refuse on SH.
        CHECK_FALSE(subaru_variant_supported(fam, SubaruInitVariant::SsmvFactory));
    }
}

TEST_CASE("subaru_variant_supported: SSM-II is factory-only",
          "[flash][subaru_sh_can_flash][variant_matrix]") {
    using st::flash::SubaruEcuFamily;
    using st::flash::SubaruInitVariant;
    using st::flash::subaru_variant_supported;
    CHECK(subaru_variant_supported(SubaruEcuFamily::SH7055,
                                    SubaruInitVariant::Factory));
    // No aftermarket fleet on SSM-II per docs/38.
    CHECK_FALSE(subaru_variant_supported(SubaruEcuFamily::SH7055,
                                          SubaruInitVariant::CobbFlash));
    CHECK_FALSE(subaru_variant_supported(SubaruEcuFamily::SH7055,
                                          SubaruInitVariant::Aftermarket));
    CHECK_FALSE(subaru_variant_supported(SubaruEcuFamily::SH7055,
                                          SubaruInitVariant::EcuTek));
}

TEST_CASE("subaru_variant_supported: RH850 has no EcuTek; SSM-VI has no factory",
          "[flash][subaru_sh_can_flash][variant_matrix]") {
    using st::flash::SubaruEcuFamily;
    using st::flash::SubaruInitVariant;
    using st::flash::subaru_variant_supported;
    // SSM-V: factory + SsmvFactory + COBB + aftermarket; no EcuTek yet.
    CHECK(subaru_variant_supported(SubaruEcuFamily::Rh850V,
                                    SubaruInitVariant::Factory));
    CHECK(subaru_variant_supported(SubaruEcuFamily::Rh850V,
                                    SubaruInitVariant::SsmvFactory));
    CHECK(subaru_variant_supported(SubaruEcuFamily::Rh850V,
                                    SubaruInitVariant::CobbFlash));
    CHECK_FALSE(subaru_variant_supported(SubaruEcuFamily::Rh850V,
                                          SubaruInitVariant::EcuTek));
    // SSM-VI: no factory-key recovery on file yet.
    CHECK_FALSE(subaru_variant_supported(SubaruEcuFamily::Rh850Vi,
                                          SubaruInitVariant::Factory));
    CHECK(subaru_variant_supported(SubaruEcuFamily::Rh850Vi,
                                    SubaruInitVariant::CobbFlash));
    CHECK(subaru_variant_supported(SubaruEcuFamily::Rh850Vi,
                                    SubaruInitVariant::Aftermarket));
}

TEST_CASE("subaru_flash_init_type maps (family, variant) to e_FlashInitTypes",
          "[flash][subaru_sh_can_flash][variant_matrix]") {
    // ζ3: the reference architecture's e_FlashInitTypes enum is
    // 0..5 with values driven by Init class taxonomy. Our finer-
    // grained (family, variant) split collapses to the 6 wire
    // values via this helper.
    using st::flash::SubaruEcuFamily;
    using st::flash::SubaruInitVariant;
    using st::flash::SubaruFlashInitType;
    using st::flash::subaru_flash_init_type;

    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH7055,
                                  SubaruInitVariant::Factory) ==
          SubaruFlashInitType::SSMII);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH7058,
                                  SubaruInitVariant::Factory) ==
          SubaruFlashInitType::SSMIII_Factory);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH7058,
                                  SubaruInitVariant::CobbFlash) ==
          SubaruFlashInitType::SSMIII_COBB);
    // MAF-SD collapses to the same SSMIII_COBB wire value — the
    // distinction is per-install-state, not per-flash-init class.
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH7058,
                                  SubaruInitVariant::CobbMafSd) ==
          SubaruFlashInitType::SSMIII_COBB);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH2A,
                                  SubaruInitVariant::Factory) ==
          SubaruFlashInitType::SSMIV_Factory);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH2A,
                                  SubaruInitVariant::CobbFlash) ==
          SubaruFlashInitType::SSMIV_COBB);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::Rh850V,
                                  SubaruInitVariant::CobbFlash) ==
          SubaruFlashInitType::SSMVI_COBB);
    // Combos with no wire mapping fall through to Unknown.
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH2A,
                                  SubaruInitVariant::EcuTek) ==
          SubaruFlashInitType::Unknown);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::SH2A,
                                  SubaruInitVariant::Aftermarket) ==
          SubaruFlashInitType::Unknown);
    CHECK(subaru_flash_init_type(SubaruEcuFamily::Rh850V,
                                  SubaruInitVariant::SsmvFactory) ==
          SubaruFlashInitType::Unknown);
}

TEST_CASE("SubaruChallengeMode + SubaruFmatsMode enums are wire-typed",
          "[flash][subaru_sh_can_flash][variant_matrix]") {
    // ζ3 pinned ChallengeMode as binary {Construct=0, Direct=1}.
    // FMATSMode is a 1-byte payload with per-mode values TBD pending
    // bench-rig observation — pin the placeholder so it doesn't
    // accidentally drift to a different bit-width.
    using st::flash::SubaruChallengeMode;
    using st::flash::SubaruFmatsMode;
    REQUIRE(static_cast<std::uint8_t>(SubaruChallengeMode::Construct) == 0);
    REQUIRE(static_cast<std::uint8_t>(SubaruChallengeMode::Direct) == 1);
    REQUIRE(static_cast<std::uint8_t>(SubaruFmatsMode::Unknown) == 0xFF);
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
