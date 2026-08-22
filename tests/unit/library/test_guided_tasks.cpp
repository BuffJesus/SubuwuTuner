// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/guided_tasks.hpp"

#include <catch2/catch_test_macros.hpp>

namespace gt = st::library::guided_tasks;

TEST_CASE("guided task state round trips notes and completion", "[library][guided_tasks]") {
    gt::State input{{{"identity-baseline", true, "Stock CRC checked"},
                     {"fueling-maf", false, "Need a clean third-gear log"}}};
    auto encoded = gt::serialize(input);
    REQUIRE(encoded.has_value());
    auto decoded = gt::parse(*encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->tasks.size() == 2);
    CHECK(decoded->tasks[0].id == "identity-baseline");
    CHECK(decoded->tasks[0].complete);
    CHECK(decoded->tasks[0].note == "Stock CRC checked");
    CHECK_FALSE(decoded->tasks[1].complete);
    CHECK(decoded->tasks[1].note == "Need a clean third-gear log");
}

TEST_CASE("guided task state rejects invalid documents", "[library][guided_tasks]") {
    CHECK_FALSE(gt::parse("schema = \"future.v9\"").has_value());
    CHECK_FALSE(gt::parse("schema = \"subuwutuner.guided-tasks.v1\"\ntask = [{}]")
                    .has_value());
    gt::State empty_id{{{"", false, ""}}};
    CHECK_FALSE(gt::serialize(empty_id).has_value());
}
