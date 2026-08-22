// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/datalog_csv.hpp"
#include "st/library/log_analysis.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace la = st::library::log_analysis;
namespace dl = st::library::datalog_csv;

namespace {

la::LogFinding const *find(la::LogAnalysis const &a, std::string_view id) {
    for (auto const &f : a.findings) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

std::size_t count_role(la::LogAnalysis const &a, la::Role role) {
    return static_cast<std::size_t>(
        std::count_if(a.channels.begin(), a.channels.end(),
                      [role](la::ResolvedChannel const &c) { return c.role == role; }));
}

} // namespace

TEST_CASE("resolve_channels maps Subaru logger header names to roles",
          "[library][log_analysis]") {
    auto const d = dl::parse(
        "rpm,load,fbkc1,fbkc2,flkc1,dam,target_boost,actual_boost,cmd,obs\n"
        "3000,2.0,0.0,0.0,0.0,1.0,15.0,15.0,14.7,14.7\n");
    auto const a = la::analyze(d);
    CHECK(count_role(a, la::Role::Rpm) == 1);
    CHECK(count_role(a, la::Role::Load) == 1);
    CHECK(count_role(a, la::Role::FeedbackKnock) == 2); // fbkc1 + fbkc2
    CHECK(count_role(a, la::Role::FineKnock) == 1);
    CHECK(count_role(a, la::Role::Dam) == 1);
    CHECK(count_role(a, la::Role::TargetBoost) == 1);
    CHECK(count_role(a, la::Role::ActualBoost) == 1);
    CHECK(count_role(a, la::Role::CommandedAfr) == 1);
    CHECK(count_role(a, la::Role::ObservedAfr) == 1);
}

TEST_CASE("analyze flags an active knock-retard event with context",
          "[library][log_analysis]") {
    auto const d = dl::parse("rpm,load,fbkc1,flkc1\n"
                             "2000,3.0,0.0,0.0\n"
                             "4000,4.5,-2.1,-3.75\n"
                             "5000,4.8,0.0,-1.0\n");
    auto const a = la::analyze(d);

    auto const *fb = find(a, "knock.feedback");
    REQUIRE(fb != nullptr);
    CHECK(fb->severity == la::Severity::High); // worst <= -1.0
    CHECK(fb->event_count == 1);
    CHECK(fb->worst_value == Catch::Approx(-2.1));
    REQUIRE(fb->worst_row.has_value());
    CHECK(*fb->worst_row == 1);
    REQUIRE(fb->at_rpm.has_value());
    CHECK(*fb->at_rpm == Catch::Approx(4000.0));
    REQUIRE(fb->at_load.has_value());
    CHECK(*fb->at_load == Catch::Approx(4.5));

    auto const *fine = find(a, "knock.fine");
    REQUIRE(fine != nullptr);
    CHECK(fine->severity == la::Severity::High); // worst -3.75 <= -3.0
    CHECK(fine->worst_value == Catch::Approx(-3.75));
}

TEST_CASE("analyze reports a reassuring finding on a clean log",
          "[library][log_analysis]") {
    auto const d = dl::parse("rpm,load,fbkc1,flkc1,dam\n"
                             "2000,3.0,0.0,0.0,1.0\n"
                             "4000,4.5,0.0,0.0,1.0\n");
    auto const a = la::analyze(d);

    auto const *fb = find(a, "knock.feedback");
    REQUIRE(fb != nullptr);
    CHECK(fb->severity == la::Severity::Info);
    CHECK(fb->event_count == 0);
    CHECK(find(a, "knock.fine") == nullptr); // no learned pull -> not reported

    auto const *dam = find(a, "dam");
    REQUIRE(dam != nullptr);
    CHECK(dam->severity == la::Severity::Info);

    CHECK(a.summary.find("Clean") != std::string::npos);
}

TEST_CASE("analyze flags DAM below 1.0", "[library][log_analysis]") {
    auto const d = dl::parse("rpm,dam\n2000,1.0\n4000,0.75\n");
    auto const a = la::analyze(d);
    auto const *dam = find(a, "dam");
    REQUIRE(dam != nullptr);
    CHECK(dam->severity == la::Severity::High);
    CHECK(dam->worst_value == Catch::Approx(0.75));
}

TEST_CASE("analyze flags overboost", "[library][log_analysis]") {
    auto const d = dl::parse("rpm,target_boost,actual_boost\n"
                             "4000,15.0,15.5\n"
                             "5000,18.0,21.0\n"); // +16.7% over target
    auto const a = la::analyze(d);
    auto const *b = find(a, "boost.tracking");
    REQUIRE(b != nullptr);
    CHECK(b->severity == la::Severity::Medium); // >10% but <20%
    CHECK(b->worst_value > 0.10);
}

TEST_CASE("analyze flags lean under load", "[library][log_analysis]") {
    auto const d = dl::parse("rpm,load,cmd,obs\n"
                             "2000,1.0,14.7,14.7\n"
                             "5000,5.0,11.0,12.0\n"); // +9% leaner at high load
    auto const a = la::analyze(d);
    auto const *lean = find(a, "afr.lean");
    REQUIRE(lean != nullptr);
    CHECK(lean->severity == la::Severity::High); // >8%
    CHECK(lean->worst_value > 0.04);
}

TEST_CASE("analyze ranks high-severity findings first",
          "[library][log_analysis]") {
    auto const d = dl::parse("rpm,fbkc1,dam\n"
                             "2000,0.0,1.0\n"
                             "4000,-2.5,1.0\n"); // High knock + Info DAM
    auto const a = la::analyze(d);
    REQUIRE(a.findings.size() >= 2);
    CHECK(a.findings.front().severity == la::Severity::High);
}

TEST_CASE("analyze summarizes an empty / unrecognized log",
          "[library][log_analysis]") {
    auto const d = dl::parse("foo,bar\n1,2\n");
    auto const a = la::analyze(d);
    CHECK(a.channels.empty());
    CHECK(a.summary.find("No recognized") != std::string::npos);
}
