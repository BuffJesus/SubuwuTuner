// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/coldstart.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <vector>

using namespace st::log::coldstart;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<double> flatten(
    std::initializer_list<std::initializer_list<double>> rows) {
    std::vector<double> out;
    for (auto const &row : rows) {
        for (double v : row) out.push_back(v);
    }
    return out;
}

// CSV layout: ts, ect, iat, rpm, obs_lambda, cmd_lambda
PidMapping basic_mapping() {
    PidMapping m{};
    m.timestamp_idx        = 0;
    m.ect_idx              = 1;
    m.iat_idx              = 2;
    m.rpm_idx              = 3;
    m.observed_lambda_idx  = 4;
    m.commanded_lambda_idx = 5;
    return m;
}

WindowConfig default_cfg() {
    WindowConfig c{};
    c.cold_threshold_c       = 55.0;
    c.ect_bin_width_c        = 5.0;
    c.cranking_max_rpm       = 400.0;
    c.initial_firing_max_rpm = 700.0;
    c.high_idle_max_rpm      = 1500.0;
    c.min_samples_per_bin    = 2;
    c.timestamp_unit         = TimestampUnit::UnixSeconds;
    return c;
}

Methodology default_methodology() {
    Methodology m{};
    m.target_lambda_vs_ect.points = {
        { -20.0, 0.80},
        {   0.0, 0.85},
        {  20.0, 0.90},
        {  40.0, 0.95},
        {  55.0, 1.00},
    };
    return m;
}

}  // namespace

// ---- TargetLambdaCurve --------------------------------------------------

TEST_CASE("TargetLambdaCurve::sample returns 1.0 on empty curve",
          "[coldstart][curve]") {
    TargetLambdaCurve c;
    REQUIRE_THAT(c.sample(   0.0), WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(c.sample(-100.0), WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(c.sample(+100.0), WithinAbs(1.0, 1e-9));
}

TEST_CASE("TargetLambdaCurve::sample clamps below first / above last point",
          "[coldstart][curve]") {
    TargetLambdaCurve c;
    c.points = {{0.0, 0.85}, {50.0, 1.00}};
    REQUIRE_THAT(c.sample(-10.0), WithinAbs(0.85, 1e-9));
    REQUIRE_THAT(c.sample(  0.0), WithinAbs(0.85, 1e-9));
    REQUIRE_THAT(c.sample( 50.0), WithinAbs(1.00, 1e-9));
    REQUIRE_THAT(c.sample(999.0), WithinAbs(1.00, 1e-9));
}

TEST_CASE("TargetLambdaCurve::sample linearly interpolates between points",
          "[coldstart][curve]") {
    TargetLambdaCurve c;
    c.points = {{0.0, 0.80}, {20.0, 0.90}};
    // halfway -> mean
    REQUIRE_THAT(c.sample(10.0), WithinAbs(0.85, 1e-9));
    // quarter
    REQUIRE_THAT(c.sample(5.0),  WithinAbs(0.825, 1e-9));
}

// ---- Phase classifier ---------------------------------------------------

TEST_CASE("snapshot_from_samples classifies samples by RPM + ECT",
          "[coldstart][snapshot][phase]") {
    // ts, ect, iat, rpm, obs, cmd
    auto const samples = flatten({
        // PreCrank: rpm 0
        {0.0,  20.0, 20.0,    0.0, 0.0, 0.0},
        // Cranking: 0 < rpm < 400
        {1.0,  20.0, 20.0,  250.0, 0.0, 0.0},
        // InitialFiring: 400 <= rpm < 700
        {2.0,  21.0, 20.0,  550.0, 0.85, 0.88},
        // HighIdle: 700 <= rpm < 1500, ECT < 55
        {3.0,  25.0, 20.0,  900.0, 0.88, 0.90},
        // Warmup: rpm >= 1500, ECT < 55
        {4.0,  30.0, 20.0, 2000.0, 0.92, 0.92},
        // ClosedLoop: ECT >= 55 regardless of rpm
        {5.0,  60.0, 20.0,  800.0, 0.97, 1.00},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE(s.samples_considered == 6);
    REQUIRE(s.phase_counts[static_cast<std::size_t>(TestPhase::PreCrank)]      == 1u);
    REQUIRE(s.phase_counts[static_cast<std::size_t>(TestPhase::Cranking)]      == 1u);
    REQUIRE(s.phase_counts[static_cast<std::size_t>(TestPhase::InitialFiring)] == 1u);
    REQUIRE(s.phase_counts[static_cast<std::size_t>(TestPhase::HighIdle)]      == 1u);
    REQUIRE(s.phase_counts[static_cast<std::size_t>(TestPhase::Warmup)]        == 1u);
    REQUIRE(s.phase_counts[static_cast<std::size_t>(TestPhase::ClosedLoop)]    == 1u);
}

TEST_CASE("snapshot_from_samples tracks phase_seconds from timestamp deltas",
          "[coldstart][snapshot][duration]") {
    // 4 samples of cranking 1.0 s apart, then 4 samples of high-idle 1.0 s
    // apart. Phase duration accounting attributes each delta to the
    // PREVIOUS sample's phase. So cranking gets 3 s (deltas 0->1, 1->2,
    // 2->3) and high-idle gets 3 s (3->4, 4->5, 5->6 — last cranking
    // sample's transition to first high-idle gets attributed to cranking,
    // then three high-idle->high-idle deltas).
    auto const samples = flatten({
        {0.0, 20.0, 20.0,  300.0, 0.0, 0.0},   // cranking
        {1.0, 20.0, 20.0,  300.0, 0.0, 0.0},   // cranking
        {2.0, 20.0, 20.0,  300.0, 0.0, 0.0},   // cranking
        {3.0, 20.0, 20.0,  300.0, 0.0, 0.0},   // cranking
        {4.0, 25.0, 20.0,  900.0, 0.0, 0.0},   // high-idle
        {5.0, 25.0, 20.0,  900.0, 0.0, 0.0},   // high-idle
        {6.0, 25.0, 20.0,  900.0, 0.0, 0.0},   // high-idle
        {7.0, 25.0, 20.0,  900.0, 0.0, 0.0},   // high-idle
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    // 3 cranking-internal deltas + 1 cranking->high-idle transition = 4 s.
    REQUIRE_THAT(s.phase_seconds[static_cast<std::size_t>(TestPhase::Cranking)],
                  WithinAbs(4.0, 1e-9));
    // 3 high-idle-internal deltas = 3 s.
    REQUIRE_THAT(s.phase_seconds[static_cast<std::size_t>(TestPhase::HighIdle)],
                  WithinAbs(3.0, 1e-9));
}

TEST_CASE("snapshot_from_samples drops huge timestamp gaps",
          "[coldstart][snapshot][duration]") {
    // 10-second gap between two cranking samples — log paused, drop it.
    auto const samples = flatten({
        {  0.0, 20.0, 20.0, 300.0, 0.0, 0.0},
        { 10.0, 20.0, 20.0, 300.0, 0.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE_THAT(s.phase_seconds[static_cast<std::size_t>(TestPhase::Cranking)],
                  WithinAbs(0.0, 1e-9));
}

// ---- ECT binning --------------------------------------------------------

TEST_CASE("snapshot_from_samples bins cold-regime samples by ECT",
          "[coldstart][snapshot][bin]") {
    // 4 samples in ECT bin [20,25), 3 samples in [30,35).
    auto const samples = flatten({
        {0.0, 20.0, 20.0,  900.0, 0.84, 0.88},
        {0.1, 21.0, 20.0,  900.0, 0.86, 0.88},
        {0.2, 22.0, 20.0,  900.0, 0.85, 0.88},
        {0.3, 24.0, 20.0,  900.0, 0.85, 0.88},
        {0.4, 30.0, 20.0, 1000.0, 0.92, 0.92},
        {0.5, 32.0, 20.0, 1000.0, 0.91, 0.92},
        {0.6, 33.0, 20.0, 1000.0, 0.93, 0.92},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE(s.ect_bins.size() == 2);
    REQUIRE_THAT(s.ect_bins[0].ect_center_c, WithinAbs(22.5, 1e-9));
    REQUIRE(s.ect_bins[0].count == 4);
    REQUIRE_THAT(s.ect_bins[0].observed_lambda_mean, WithinAbs(0.85, 1e-9));
    REQUIRE_THAT(s.ect_bins[1].ect_center_c, WithinAbs(32.5, 1e-9));
    REQUIRE(s.ect_bins[1].count == 3);
}

TEST_CASE("snapshot_from_samples excludes warm samples from bins",
          "[coldstart][snapshot][bin]") {
    auto const samples = flatten({
        {0.0, 30.0, 20.0,  900.0, 0.90, 0.92},
        {0.1, 30.0, 20.0,  900.0, 0.91, 0.92},
        // 70 °C — above cold_threshold 55, must NOT bucket
        {0.2, 70.0, 20.0,  900.0, 1.00, 1.00},
        {0.3, 71.0, 20.0,  900.0, 0.99, 1.00},
        {0.4, 72.0, 20.0,  900.0, 1.01, 1.00},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE(s.ect_bins.size() == 1);
    REQUIRE_THAT(s.ect_bins[0].ect_center_c, WithinAbs(32.5, 1e-9));
}

TEST_CASE("snapshot_from_samples computes deviation_from_target per bin",
          "[coldstart][snapshot][compliance]") {
    Methodology m;
    m.target_lambda_vs_ect.points = {
        {0.0, 0.85}, {50.0, 1.00},
    };  // at ECT=22.5, target = 0.85 + (22.5/50)*(1.0-0.85) = 0.85 + 0.0675 = 0.9175
    auto const samples = flatten({
        {0.0, 22.0, 20.0, 900.0, 0.85, 0.92},
        {0.1, 23.0, 20.0, 900.0, 0.85, 0.92},
        {0.2, 22.5, 20.0, 900.0, 0.85, 0.92},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), m);
    REQUIRE(s.ect_bins.size() == 1);
    // observed=0.85, target≈0.9175 → |dev| ≈ 0.0675
    REQUIRE_THAT(s.ect_bins[0].deviation_from_target,
                  WithinAbs(0.0675, 1e-3));
}

TEST_CASE("snapshot_from_samples filters out bins below min_samples_per_bin",
          "[coldstart][snapshot][filter]") {
    // bin [20,25): 1 sample (drops). bin [30,35): 3 samples (keeps).
    auto const samples = flatten({
        {0.0, 22.0, 20.0,  900.0, 0.85, 0.88},
        {0.1, 30.0, 20.0,  900.0, 0.90, 0.92},
        {0.2, 32.0, 20.0,  900.0, 0.91, 0.92},
        {0.3, 33.0, 20.0,  900.0, 0.92, 0.92},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE(s.ect_bins.size() == 1);
    REQUIRE(s.ect_bins[0].count == 3);
}

TEST_CASE("snapshot_from_samples populates coldest/warmest ECT fields",
          "[coldstart][snapshot][span]") {
    auto const samples = flatten({
        {0.0, -10.0, 20.0, 0.0, 0.0, 0.0},
        {1.0,  50.0, 20.0, 0.0, 0.0, 0.0},
        {2.0,   5.0, 20.0, 0.0, 0.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE_THAT(s.coldest_ect_c, WithinAbs(-10.0, 1e-9));
    REQUIRE_THAT(s.warmest_ect_c, WithinAbs( 50.0, 1e-9));
}

TEST_CASE("snapshot_from_samples returns empty on empty input",
          "[coldstart][snapshot]") {
    auto const s = snapshot_from_samples({}, 6, basic_mapping(),
                                         default_cfg(), default_methodology());
    REQUIRE(s.samples_considered == 0);
    REQUIRE(s.samples.empty());
    REQUIRE(s.ect_bins.empty());
}

// ---- CSV path -----------------------------------------------------------

TEST_CASE("snapshot_from_csv reports FileNotFound on missing path",
          "[coldstart][csv]") {
    auto const r = snapshot_from_csv("/definitely/missing.csv",
                                     basic_mapping(), default_cfg(),
                                     default_methodology());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("snapshot_from_csv rejects mapping without mandatory columns",
          "[coldstart][csv][validate]") {
    PidMapping m  = basic_mapping();
    m.ect_idx     = kNoColumn;
    auto const r  = snapshot_from_csv("/tmp/whatever.csv", m,
                                      default_cfg(), default_methodology());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("snapshot_from_csv reads a small log",
          "[coldstart][csv][roundtrip]") {
    auto const path = std::filesystem::temp_directory_path()
                      / "subuwutuner_test_coldstart.csv";
    {
        std::ofstream of{path};
        of << "ts,ect,iat,rpm,obs,cmd\n"
              "0,22,20,900,0.85,0.88\n"
              "1,23,20,900,0.85,0.88\n"
              "2,24,20,900,0.86,0.88\n";
    }
    auto const r = snapshot_from_csv(path.string(), basic_mapping(),
                                     default_cfg(), default_methodology());
    REQUIRE(r.has_value());
    REQUIRE(r->samples_considered == 3);
    REQUIRE(r->ect_bins.size() == 1);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
