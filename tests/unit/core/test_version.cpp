// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Version reports project name and a non-empty version string",
          "[core][version]") {
    REQUIRE(st::Version::name() == "SubuwuTuner");

    auto const ver = st::Version::string();
    REQUIRE_FALSE(ver.empty());
    REQUIRE(ver.find('.') != std::string_view::npos);
}

TEST_CASE("Version major/minor/patch are non-negative and parseable",
          "[core][version]") {
    REQUIRE(st::Version::Major >= 0);
    REQUIRE(st::Version::Minor >= 0);
    REQUIRE(st::Version::Patch >= 0);
}
