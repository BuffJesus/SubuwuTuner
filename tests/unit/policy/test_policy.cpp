// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <catch2/catch_test_macros.hpp>

#include <st/policy.hpp>

using st::policy::Action;
using st::policy::Decision;
using st::policy::emissions_action;
using st::policy::engine_safety_on_flash;
using st::policy::parse_profile;
using st::policy::Profile;
using st::policy::profile_name;

TEST_CASE("emissions_action matches the docs/06 matrix", "[policy][emissions]") {
    // motorsport-only: silent in all stages
    auto const m = emissions_action(Profile::MotorsportOnly);
    REQUIRE(m.on_save == Action::Silent);
    REQUIRE(m.on_flash == Action::Silent);

    // alberta-ca: badge on save, silent on flash
    auto const a = emissions_action(Profile::AlbertaCa);
    REQUIRE(a.on_save == Action::Badge);
    REQUIRE(a.on_flash == Action::Silent);

    // eu-roadworthy: warn on save, confirm on flash
    auto const e = emissions_action(Profile::EuRoadworthy);
    REQUIRE(e.on_save == Action::Warn);
    REQUIRE(e.on_flash == Action::Confirm);

    // california-us: confirm+reason on both
    auto const c = emissions_action(Profile::CaliforniaUs);
    REQUIRE(c.on_save == Action::ConfirmWithReason);
    REQUIRE(c.on_flash == Action::ConfirmWithReason);
}

TEST_CASE("engine_safety_on_flash always Blocks regardless of profile", "[policy][engine_safety]") {
    REQUIRE(engine_safety_on_flash(Profile::MotorsportOnly) == Action::Block);
    REQUIRE(engine_safety_on_flash(Profile::AlbertaCa) == Action::Block);
    REQUIRE(engine_safety_on_flash(Profile::EuRoadworthy) == Action::Block);
    REQUIRE(engine_safety_on_flash(Profile::CaliforniaUs) == Action::Block);
}

TEST_CASE("profile_name <-> parse_profile round-trip", "[policy][serde]") {
    for (auto p : {Profile::MotorsportOnly, Profile::AlbertaCa, Profile::EuRoadworthy,
                   Profile::CaliforniaUs}) {
        auto const name = profile_name(p);
        auto const parsed = parse_profile(name);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == p);
    }
}

TEST_CASE("parse_profile rejects unknown strings cleanly", "[policy][serde]") {
    REQUIRE_FALSE(parse_profile("nonsense").has_value());
    REQUIRE_FALSE(parse_profile("").has_value());
    REQUIRE_FALSE(parse_profile("MotorsportOnly").has_value()); // wrong casing
}

TEST_CASE("Action ordering supports `take stricter of two`", "[policy][action]") {
    // Comparing two Actions with `<` lets callers escalate when multiple
    // concerns apply to the same edit. Verify the documented ordering.
    REQUIRE(Action::Silent < Action::Badge);
    REQUIRE(Action::Badge < Action::Warn);
    REQUIRE(Action::Warn < Action::Confirm);
    REQUIRE(Action::Confirm < Action::ConfirmWithReason);
    REQUIRE(Action::ConfirmWithReason < Action::Block);
}
