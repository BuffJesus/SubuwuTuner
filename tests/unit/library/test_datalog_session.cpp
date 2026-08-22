// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/datalog_session.hpp"

#include <catch2/catch_test_macros.hpp>

namespace ds = st::library::datalog_session;
namespace dc = st::library::datalog_csv;

TEST_CASE("datalog session round trips all user state", "[library][datalog]") {
    ds::Session input;
    input.source_path = R"(logs\pull "three".csv)";
    input.visible_headers = {"Engine Speed [rpm]", "Boost"};
    input.x_axis = "Time (s)";
    input.notes = "Third-gear pull\nWatch the boost spike.";
    input.range_first = 3;
    input.range_last = 44;
    input.markers = {{4, "tip-in"}, {27, "lift"}};
    input.derived_channels = {{"Boost error", "psi", "Target Boost", "Boost",
                               dc::DerivedOperation::Subtract},
                              {"MAF ratio", "", "MAF", "MAF target",
                               dc::DerivedOperation::Ratio}};

    auto encoded = ds::serialize(input);
    REQUIRE(encoded.has_value());
    auto decoded = ds::parse(*encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->source_path == input.source_path);
    CHECK(decoded->visible_headers == input.visible_headers);
    CHECK(decoded->x_axis == input.x_axis);
    CHECK(decoded->notes == input.notes);
    CHECK(decoded->range_first == 3);
    CHECK(decoded->range_last == 44);
    REQUIRE(decoded->markers.size() == 2);
    CHECK(decoded->markers[0].row == 4);
    CHECK(decoded->markers[0].label == "tip-in");
    REQUIRE(decoded->derived_channels.size() == 2);
    CHECK(decoded->derived_channels[0].operation == dc::DerivedOperation::Subtract);
    CHECK(decoded->derived_channels[1].operation == dc::DerivedOperation::Ratio);
}

TEST_CASE("datalog session accepts omitted optional collections", "[library][datalog]") {
    auto parsed = ds::parse(R"(schema = "subuwutuner.log-session.v1"
source_path = "pull.csv"
)" );
    REQUIRE(parsed.has_value());
    CHECK(parsed->source_path == "pull.csv");
    CHECK(parsed->x_axis == ds::kAutoAxis);
    CHECK(parsed->visible_headers.empty());
    CHECK(parsed->markers.empty());
}

TEST_CASE("datalog session rejects malformed and incompatible documents", "[library][datalog]") {
    CHECK_FALSE(ds::parse("not = [valid").has_value());
    auto incompatible = ds::parse("schema = \"subuwutuner.log-session.v99\"");
    REQUIRE_FALSE(incompatible.has_value());
    CHECK(incompatible.error().code() == st::ErrorCode::UnsupportedVersion);

    auto bad_marker = ds::parse(R"(schema = "subuwutuner.log-session.v1"
marker = [{ row = -1, label = "bad" }]
)" );
    REQUIRE_FALSE(bad_marker.has_value());
    CHECK(bad_marker.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("user signal profiles round trip exact channel headers", "[library][datalog]") {
    std::vector<ds::SignalProfile> profiles{{"My boost pull", {"RPM", "Boost [psi]", "WGDC"}},
                                            {"Idle", {"Time (s)", "RPM"}}};
    auto encoded = ds::serialize_profiles(profiles);
    REQUIRE(encoded.has_value());
    auto decoded = ds::parse_profiles(*encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 2);
    CHECK((*decoded)[0].name == "My boost pull");
    CHECK((*decoded)[0].visible_headers == profiles[0].visible_headers);
    CHECK_FALSE(ds::parse_profiles("schema = \"future\"").has_value());
}
