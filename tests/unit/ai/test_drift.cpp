// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tier 1 drift classifier — rule coverage via synthetic snapshots.
// Each test case constructs a HistorySnapshot directly (no CSV / no
// file I/O) and asserts the diagnosis the rule table in docs/20
// promises. Synthetic input also pins the *evidence* the classifier
// includes, so a future rule edit that swaps a recommended-check
// line catches a test break instead of silently changing the GUI
// surface.

#include "st/ai/drift.hpp"
#include "st/log/adaptive_history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace ai = st::ai::drift;
namespace ad = st::log::adaptive;

namespace {

// Build a SignalSeries with the given mean + drift, marking has_data
// true. Buckets are synthetic — we don't need them for the rule
// engine, only the aggregates.
ad::SignalSeries make_series(ad::SignalKind kind, double mean, double drift_per_second) {
    ad::SignalSeries s;
    s.kind = kind;
    s.overall_mean = mean;
    s.overall_min = mean - 1.0;
    s.overall_max = mean + 1.0;
    s.drift_per_second = drift_per_second;
    s.has_data = true;
    // One synthetic bucket so callers that consume `points` don't
    // hit empty-vector logic; the classifier ignores it but the
    // shape stays realistic.
    s.points.push_back({0.0, mean, mean - 1.0, mean + 1.0, 1});
    return s;
}

ad::SignalSeries absent_series(ad::SignalKind kind) {
    ad::SignalSeries s;
    s.kind = kind;
    s.has_data = false;
    return s;
}

ad::HistorySnapshot make_snapshot(double span_days,
                                  std::uint64_t total_samples,
                                  ad::SignalSeries ltft,
                                  ad::SignalSeries dam,
                                  ad::SignalSeries idle) {
    ad::HistorySnapshot h;
    h.series[0] = std::move(ltft);
    h.series[1] = std::move(dam);
    h.series[2] = std::move(idle);
    h.total_samples = total_samples;
    h.earliest_timestamp = 0.0;
    h.latest_timestamp = span_days * 86400.0;
    h.time_span_seconds = span_days * 86400.0;
    return h;
}

bool has_evidence(ai::DriftDiagnosis const &d, std::string_view needle) {
    return std::any_of(d.evidence.begin(), d.evidence.end(),
                       [&](std::string const &e) {
                           return e.find(needle) != std::string::npos;
                       });
}

bool has_alt(ai::DriftDiagnosis const &d, std::string_view name) {
    return std::any_of(d.alternatives.begin(), d.alternatives.end(),
                       [&](std::string const &a) { return a == name; });
}

} // namespace

TEST_CASE("drift::classify on empty snapshot returns NoSignal",
          "[ai][drift][no-signal]") {
    ad::HistorySnapshot h;
    auto const d = ai::classify(h);
    REQUIRE(d.confidence == ai::Confidence::NoSignal);
    REQUIRE(d.cause == "no_signal");
    REQUIRE(has_evidence(d, "No samples"));
}

TEST_CASE("drift::classify rejects sub-minimum span as NoSignal",
          "[ai][drift][no-signal]") {
    // 1-day span < default 3-day minimum.
    auto const h = make_snapshot(
        1.0, 5000,
        make_series(ad::SignalKind::Ltft, -8.0, -0.5 / 86400.0),
        absent_series(ad::SignalKind::Dam),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.confidence == ai::Confidence::NoSignal);
    REQUIRE(has_evidence(d, "shorter than minimum"));
}

TEST_CASE("drift::classify Rule 1 — DAM well below baseline → knock_correction",
          "[ai][drift][knock]") {
    auto const h = make_snapshot(
        7.0, 25000,
        absent_series(ad::SignalKind::Ltft),
        make_series(ad::SignalKind::Dam, 0.90, 0.0),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "knock_correction");
    REQUIRE(d.confidence == ai::Confidence::Likely);
    REQUIRE(has_evidence(d, "DAM mean"));
    REQUIRE_FALSE(d.recommended_checks.empty());
}

TEST_CASE("drift::classify Rule 1 borderline → Possible",
          "[ai][drift][knock]") {
    // DAM at 0.93 — within knock-trigger band but not deeply below.
    auto const h = make_snapshot(
        7.0, 25000,
        absent_series(ad::SignalKind::Ltft),
        make_series(ad::SignalKind::Dam, 0.93, 0.0),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "knock_correction");
    REQUIRE(d.confidence == ai::Confidence::Possible);
}

TEST_CASE("drift::classify Rule 1 + positive LTFT lists fuel_quality_drift alt",
          "[ai][drift][knock][fuel-quality]") {
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, +7.0, 0.0),
        make_series(ad::SignalKind::Dam, 0.88, 0.0),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "knock_correction");
    REQUIRE(has_alt(d, "fuel_quality_drift"));
}

TEST_CASE("drift::classify Rule 2 — positive LTFT trending positive → fuel_quality",
          "[ai][drift][fuel-quality]") {
    // LTFT +8% mean, +0.6%/day drift over 14 days.
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, +8.0, 0.6 / 86400.0),
        absent_series(ad::SignalKind::Dam),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "fuel_quality_drift");
    REQUIRE(d.confidence == ai::Confidence::Likely);
    REQUIRE(has_alt(d, "injector_overflow_or_leak"));
}

TEST_CASE("drift::classify Rule 3 — negative LTFT trending negative + idle drift → vacuum_leak alt",
          "[ai][drift][fuel-system][vacuum]") {
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, -7.0, -0.6 / 86400.0),
        absent_series(ad::SignalKind::Dam),
        make_series(ad::SignalKind::IdleAdapt, 8.0, 1.0 / 86400.0));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "fuel_system_drift");
    REQUIRE(d.confidence == ai::Confidence::Likely);
    REQUIRE(has_alt(d, "vacuum_leak"));
    REQUIRE(has_alt(d, "evap_leak"));
}

TEST_CASE("drift::classify Rule 3 — negative LTFT drift without idle corroboration → Ambiguous",
          "[ai][drift][fuel-system][ambiguous]") {
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, -7.0, -0.6 / 86400.0),
        absent_series(ad::SignalKind::Dam),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "fuel_system_drift");
    REQUIRE(d.confidence == ai::Confidence::Ambiguous);
    REQUIRE(has_alt(d, "vacuum_leak"));
    REQUIRE(has_alt(d, "injector_aging"));
    REQUIRE(has_alt(d, "maf_aging"));
}

TEST_CASE("drift::classify Rule 4 — single-signal LTFT off-baseline → Possible",
          "[ai][drift][partial]") {
    // LTFT slightly negative, no drift, no other signals.
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, -6.0, 0.0),
        absent_series(ad::SignalKind::Dam),
        absent_series(ad::SignalKind::IdleAdapt));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "fuel_system_drift");
    REQUIRE(d.confidence == ai::Confidence::Possible);
    // Evidence should mention the absent signals so the user knows
    // why confidence isn't higher.
    REQUIRE(has_evidence(d, "DAM signal absent"));
    REQUIRE(has_evidence(d, "Idle-adapt signal absent"));
}

TEST_CASE("drift::classify Rule 5 — idle-adapt only → idle_air_drift",
          "[ai][drift][idle]") {
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, 0.5, 0.0),
        absent_series(ad::SignalKind::Dam),
        make_series(ad::SignalKind::IdleAdapt, 7.0, 0.0));
    auto const d = ai::classify(h);
    REQUIRE(d.cause == "idle_air_drift");
    REQUIRE(d.confidence == ai::Confidence::Possible);
    REQUIRE(has_alt(d, "post_disconnect_relearn"));
    REQUIRE(has_alt(d, "throttle_body_carbon"));
}

TEST_CASE("drift::classify no-rule-fires when all signals near baseline → NoSignal",
          "[ai][drift][no-signal]") {
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, 1.0, 0.0),
        make_series(ad::SignalKind::Dam, 1.0, 0.0),
        make_series(ad::SignalKind::IdleAdapt, 0.5, 0.0));
    auto const d = ai::classify(h);
    REQUIRE(d.confidence == ai::Confidence::NoSignal);
    REQUIRE(d.cause == "no_signal");
    // All-clear path lists each present signal's mean as evidence so
    // the user can see WHAT the classifier read before concluding
    // "nothing wrong".
    REQUIRE(has_evidence(d, "LTFT mean"));
    REQUIRE(has_evidence(d, "DAM mean"));
    REQUIRE(has_evidence(d, "Idle adapt mean"));
}

TEST_CASE("drift::confidence_name covers every enum value",
          "[ai][drift][names]") {
    using C = ai::Confidence;
    REQUIRE(ai::confidence_name(C::Likely) == "likely");
    REQUIRE(ai::confidence_name(C::Possible) == "possible");
    REQUIRE(ai::confidence_name(C::Ambiguous) == "ambiguous");
    REQUIRE(ai::confidence_name(C::NoSignal) == "no_signal");
}

TEST_CASE("drift::classify_with custom thresholds suppresses default-marginal cases",
          "[ai][drift][thresholds]") {
    // Default threshold would flag LTFT -6% as Rule 4 (possible);
    // raising the threshold to 10% should drop it to no_signal.
    auto const h = make_snapshot(
        14.0, 100000,
        make_series(ad::SignalKind::Ltft, -6.0, 0.0),
        absent_series(ad::SignalKind::Dam),
        absent_series(ad::SignalKind::IdleAdapt));
    ai::Thresholds t;
    t.ltft_offset_pct = 10.0;
    auto const d = ai::classify_with(h, t);
    REQUIRE(d.confidence == ai::Confidence::NoSignal);
}
