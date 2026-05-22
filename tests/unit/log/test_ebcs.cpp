// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/ebcs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

using namespace st::log::ebcs;
using Catch::Matchers::WithinAbs;

namespace {

// CSV layout: ts, target_boost, actual_boost, wgdc, throttle, rpm
PidMapping basic_mapping() {
    PidMapping m{};
    m.timestamp_idx = 0;
    m.target_boost_idx = 1;
    m.actual_boost_idx = 2;
    m.wgdc_idx = 3;
    m.throttle_idx = 4;
    m.rpm_idx = 5;
    return m;
}

DetectorConfig default_cfg() {
    DetectorConfig c{};
    c.throttle_step_threshold_pct = 30.0;
    c.target_boost_step_threshold = 2.0;
    c.min_event_duration_s = 0.5;
    c.max_event_duration_s = 6.0;
    c.spike_overshoot_threshold_pct = 50.0;
    c.noise_cv_threshold = 0.30;
    c.overshoot_warn_pct = 15.0;
    c.slow_rise_threshold_s = 1.5;
    c.steady_state_error_threshold = 1.0;
    c.timestamp_unit = TimestampUnit::UnixSeconds;
    return c;
}

// Synthesize a clean step response: at t0=1.0s throttle steps 20% -> 90%,
// target boost steps 0 -> 15. Actual boost ramps from 0 toward 15 with
// `peak_overshoot` of overshoot, settling to 15 at the tail.
std::vector<double> synth_clean_tip_in(double peak_overshoot = 1.5) {
    std::vector<double> rows;
    auto add = [&](double t, double tgt, double act, double thr) {
        rows.push_back(t);    // ts
        rows.push_back(tgt);  // target
        rows.push_back(act);  // actual
        rows.push_back(50.0); // wgdc
        rows.push_back(thr);  // throttle
        rows.push_back(3000); // rpm
    };
    // Idle / cruise lead-in
    for (double t = 0.0; t < 1.0; t += 0.1)
        add(t, 0.0, 0.0, 20.0);
    // The step
    add(1.0, 15.0, 0.0, 90.0);
    add(1.1, 15.0, 2.0, 90.0);
    add(1.2, 15.0, 5.0, 90.0);
    add(1.3, 15.0, 9.0, 90.0);
    add(1.4, 15.0, 12.0, 90.0);
    add(1.5, 15.0, 14.0, 90.0);
    add(1.6, 15.0, 15.0 + peak_overshoot, 90.0); // peak
    add(1.7, 15.0, 15.5, 90.0);
    add(1.8, 15.0, 15.1, 90.0);
    add(1.9, 15.0, 15.0, 90.0);
    add(2.0, 15.0, 15.0, 90.0);
    add(2.1, 15.0, 15.0, 90.0);
    add(2.2, 15.0, 15.0, 90.0);
    add(2.3, 15.0, 15.0, 90.0);
    add(2.4, 15.0, 15.0, 90.0);
    add(2.5, 15.0, 15.0, 90.0);
    add(2.6, 15.0, 15.0, 90.0);
    add(2.7, 15.0, 15.0, 90.0);
    add(2.8, 15.0, 15.0, 90.0);
    add(2.9, 15.0, 15.0, 90.0);
    add(3.0, 15.0, 15.0, 90.0);
    return rows;
}

} // namespace

TEST_CASE("snapshot_from_samples returns empty on empty input", "[ebcs][snapshot]") {
    auto const s = snapshot_from_samples({}, 6, basic_mapping(), default_cfg());
    REQUIRE(s.total_event_count == 0);
    REQUIRE(s.events.empty());
    REQUIRE(s.samples_considered == 0);
}

TEST_CASE("snapshot_from_samples detects a clean tip-in", "[ebcs][snapshot][detect]") {
    auto const samples = synth_clean_tip_in();
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(), default_cfg());
    REQUIRE(s.total_event_count == 1);
    REQUIRE(s.good_event_count == 1);
    auto const &ev = s.events.front();
    REQUIRE_THAT(ev.target_boost, WithinAbs(15.0, 0.5));
    REQUIRE(ev.peak_boost > ev.target_boost); // overshoot present
    REQUIRE(ev.rise_time_s > 0.0);
    REQUIRE(ev.rise_time_s < 1.0); // fast clean step
}

TEST_CASE("snapshot_from_samples computes overshoot percentage", "[ebcs][snapshot][overshoot]") {
    // peak_overshoot=4.5 over a step of 15 -> ~30% overshoot.
    auto const samples = synth_clean_tip_in(/*peak_overshoot=*/4.5);
    DetectorConfig cfg = default_cfg();
    cfg.spike_overshoot_threshold_pct = 80.0; // don't trip Spiked
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(), cfg);
    REQUIRE(s.total_event_count == 1);
    REQUIRE_THAT(s.events.front().overshoot_pct, WithinAbs(30.0, 5.0));
}

TEST_CASE("snapshot_from_samples flags large overshoot as Spiked", "[ebcs][snapshot][spiked]") {
    // peak_overshoot = 12 over step 15 -> 80% — above default 50% spike threshold.
    auto const samples = synth_clean_tip_in(12.0);
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(), default_cfg());
    REQUIRE(s.total_event_count == 1);
    REQUIRE(s.good_event_count == 0);
    REQUIRE(s.events.front().quality == StepQuality::Spiked);
}

TEST_CASE("snapshot_from_samples returns no events when no tip-in", "[ebcs][snapshot][nodetect]") {
    // Steady cruise — no throttle step.
    std::vector<double> samples;
    auto add = [&](double t) { samples.insert(samples.end(), {t, 5.0, 5.0, 30.0, 25.0, 2500.0}); };
    for (double t = 0.0; t < 5.0; t += 0.1)
        add(t);
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(), default_cfg());
    REQUIRE(s.total_event_count == 0);
    REQUIRE(s.gain_suggestions.size() == 1); // "no events" advisory
}

TEST_CASE("snapshot_from_samples surfaces overshoot suggestion", "[ebcs][snapshot][suggest]") {
    auto const samples = synth_clean_tip_in(/*peak_overshoot=*/4.5); // ~30% OS
    DetectorConfig cfg = default_cfg();
    cfg.spike_overshoot_threshold_pct = 80.0;
    cfg.overshoot_warn_pct = 15.0;
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(), cfg);
    REQUIRE(s.good_event_count > 0);
    bool found_overshoot_advice = false;
    for (auto const &line : s.gain_suggestions) {
        if (line.find("Overshoot") != std::string::npos) {
            found_overshoot_advice = true;
        }
    }
    REQUIRE(found_overshoot_advice);
}

TEST_CASE("snapshot_from_csv reports FileNotFound on missing path", "[ebcs][csv]") {
    auto const r = snapshot_from_csv("/definitely/missing.csv", basic_mapping(), default_cfg());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("snapshot_from_csv rejects mapping with missing mandatory columns",
          "[ebcs][csv][validate]") {
    PidMapping m = basic_mapping();
    m.target_boost_idx = kNoColumn;
    auto const r = snapshot_from_csv("/tmp/whatever.csv", m, default_cfg());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("snapshot_from_csv reads a small log", "[ebcs][csv][roundtrip]") {
    auto const path = std::filesystem::temp_directory_path() / "subuwutuner_test_ebcs.csv";
    {
        std::ofstream of{path};
        of << "ts,tgt,act,wgdc,thr,rpm\n"
              "0.0,0,0,30,20,2500\n"
              "0.5,0,0,30,20,2500\n"
              "1.0,15,0,30,90,3000\n"
              "1.1,15,3,80,90,3200\n"
              "1.2,15,8,80,90,3500\n"
              "1.3,15,12,80,90,3800\n"
              "1.4,15,14,80,90,4000\n"
              "1.5,15,15.2,80,90,4200\n"
              "1.6,15,15,80,90,4400\n"
              "1.7,15,15,80,90,4600\n"
              "1.8,15,15,80,90,4800\n"
              "1.9,15,15,80,90,5000\n"
              "2.0,15,15,80,90,5200\n";
    }
    auto const r = snapshot_from_csv(path.string(), basic_mapping(), default_cfg());
    REQUIRE(r.has_value());
    REQUIRE(r->total_event_count == 1);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
