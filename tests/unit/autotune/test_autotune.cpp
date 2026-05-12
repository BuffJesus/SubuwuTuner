// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"
#include "st/core/error.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace at = st::autotune;

namespace {

// A "warm and stable" baseline sample: passes every default gate. Tests
// mutate one or two fields and feed many copies through tune_maf.
at::MafSample warm_baseline_sample() {
    at::MafSample s;
    s.maf_voltage     = 1.5;
    s.actual_afr      = 14.7;
    s.commanded_afr   = 14.7;
    s.rpm             = 2500.0;
    s.rpm_rate        = 0.0;
    s.throttle_pct    = 25.0;
    s.throttle_rate   = 0.0;
    s.coolant_c       = 90.0;
    s.iat_c           = 25.0;
    s.closed_loop     = false;
    s.knock_in_window = false;
    s.limp_mode       = false;
    return s;
}

// Convenience: build N identical samples at a given voltage and AFR.
std::vector<at::MafSample> uniform_samples(std::size_t n,
                                             double voltage,
                                             double actual_afr,
                                             double commanded_afr) {
    auto const base = warm_baseline_sample();
    std::vector<at::MafSample> out(n, base);
    for (auto &s : out) {
        s.maf_voltage   = voltage;
        s.actual_afr    = actual_afr;
        s.commanded_afr = commanded_afr;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------
// Helpers under test
// ---------------------------------------------------------------------

TEST_CASE("nearest_cell picks the closest axis breakpoint",
          "[autotune][helpers]") {
    std::vector<double> axis{0.0, 0.5, 1.0, 1.5, 2.0};
    REQUIRE(at::nearest_cell(0.0,  axis) == 0);
    REQUIRE(at::nearest_cell(0.24, axis) == 0);
    REQUIRE(at::nearest_cell(0.26, axis) == 1);
    REQUIRE(at::nearest_cell(1.5,  axis) == 3);
    REQUIRE(at::nearest_cell(2.5,  axis) == 4);  // past-end clamps to last
    // Tie goes to the lower index per the documented contract.
    REQUIRE(at::nearest_cell(0.25, axis) == 0);
}

TEST_CASE("trimmed_mean drops the configured tails", "[autotune][helpers]") {
    // 10 values: [0, 1, 2, ..., 9]. trim=0.10 drops 1 from each side
    // (floor(10*0.10)=1), averaging [1..8] = 4.5.
    std::vector<double> vs{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    REQUIRE(at::trimmed_mean(vs, 0.10) == Catch::Approx(4.5));
    // trim=0 reduces to arithmetic mean.
    REQUIRE(at::trimmed_mean(vs, 0.0) == Catch::Approx(4.5));
}

TEST_CASE("trimmed_mean ignores extreme outliers when they cross the tail",
          "[autotune][helpers]") {
    // 12 values where the lowest and the highest are outliers far from
    // the rest. floor(12 * 0.1) = 1 → drop 1 each side, mean of the
    // remaining 10.
    std::vector<double> vs{-100, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 100};
    REQUIRE(at::trimmed_mean(vs, 0.10) == Catch::Approx(1.0));
}

TEST_CASE("sample_passes_gates rejects cold-coolant samples",
          "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto               s = warm_baseline_sample();
    s.coolant_c          = 60.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
}

TEST_CASE("sample_passes_gates rejects degenerate AFR ratios",
          "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto               s = warm_baseline_sample();
    s.commanded_afr      = 0.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
    s = warm_baseline_sample();
    s.actual_afr = std::nan("");
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
}

TEST_CASE("sample_passes_gates rejects mid-knock samples by default",
          "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto               s  = warm_baseline_sample();
    s.knock_in_window     = true;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
    opts.reject_knock_window = false;
    REQUIRE(at::sample_passes_gates(s, opts));
}

TEST_CASE("sample_passes_gates rejects large RPM / throttle transients",
          "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto               s  = warm_baseline_sample();
    s.rpm_rate            = 1000.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
    s          = warm_baseline_sample();
    s.throttle_rate = 100.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
}

// ---------------------------------------------------------------------
// tune_maf — happy paths
// ---------------------------------------------------------------------

TEST_CASE("tune_maf scales up a cell whose samples ran lean",
          "[autotune][maf]") {
    std::vector<double> axis{0.0, 0.5, 1.0, 1.5, 2.0};
    std::vector<double> current{1.0, 1.0, 1.0, 1.0, 1.0};
    // 200 samples all at MAF=1.5 V (-> cell 3), 5% lean: 15.4 vs 14.7.
    auto const samples = uniform_samples(200, /*v=*/1.5,
                                          /*actual=*/15.4,
                                          /*commanded=*/14.7);
    at::MafTuneOptions opts;
    opts.gain                 = 0.5;
    opts.max_delta_pct        = 0.08;
    opts.min_samples_per_cell = 50;

    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 200);
    REQUIRE(r->cells.size() == axis.size());
    // Cells without samples are unchanged with confidence 0.
    for (std::size_t i = 0; i < r->cells.size(); ++i) {
        if (i == 3) continue;
        REQUIRE(r->cells[i].proposed_value == 1.0);
        REQUIRE(r->cells[i].confidence == 0.0);
        REQUIRE(r->cells[i].samples_used == 0);
    }
    // Cell 3 picks up the 5% lean error scaled by gain=0.5 →
    // delta_pct = (15.4/14.7 - 1) * 0.5 = 0.02380952...
    auto const &c3 = r->cells[3];
    REQUIRE(c3.samples_used == 200);
    REQUIRE(c3.mean_error == Catch::Approx(15.4 / 14.7));
    double const expected_delta = (15.4 / 14.7 - 1.0) * 0.5;
    REQUIRE(c3.proposed_value
            == Catch::Approx(1.0 * (1.0 + expected_delta)));
    REQUIRE(c3.confidence > 0.5);
}

TEST_CASE("tune_maf scales down a cell whose samples ran rich",
          "[autotune][maf]") {
    std::vector<double> axis{0.5, 1.0, 1.5};
    std::vector<double> current{2.0, 2.0, 2.0};
    auto const samples = uniform_samples(100, 1.0, 13.0, 14.7);  // rich
    auto const r       = at::tune_maf(axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[1].samples_used == 100);
    REQUIRE(r->cells[1].proposed_value < current[1]);
}

// ---------------------------------------------------------------------
// tune_maf — gates and sufficiency
// ---------------------------------------------------------------------

TEST_CASE("tune_maf leaves cells unchanged when below min samples",
          "[autotune][maf][min-samples]") {
    std::vector<double>     axis{0.5, 1.0, 1.5};
    std::vector<double>     current{1.0, 1.0, 1.0};
    auto const              samples = uniform_samples(10, 1.0, 15.4, 14.7);
    at::MafTuneOptions      opts;
    opts.min_samples_per_cell = 50;

    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 10);
    REQUIRE(r->cells[1].samples_used == 10);
    REQUIRE(r->cells[1].proposed_value == 1.0);
    REQUIRE(r->cells[1].confidence == 0.0);
}

TEST_CASE("tune_maf filters out cold-coolant samples entirely",
          "[autotune][maf][gates]") {
    std::vector<double> axis{0.5, 1.0, 1.5};
    std::vector<double> current{1.0, 1.0, 1.0};
    auto                samples = uniform_samples(200, 1.0, 15.4, 14.7);
    for (auto &s : samples) s.coolant_c = 60.0;  // cold engine
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 0);
    REQUIRE(r->cells[1].samples_used == 0);
    REQUIRE(r->cells[1].proposed_value == 1.0);
}

// ---------------------------------------------------------------------
// tune_maf — clamping
// ---------------------------------------------------------------------

TEST_CASE("tune_maf clamps proposals to max_delta_pct",
          "[autotune][maf][clamp]") {
    std::vector<double> axis{1.0};
    std::vector<double> current{1.0};
    // Severely lean: actual=22 vs commanded=14.7 (~49% high). With
    // gain=0.5 the unclamped delta would be ~0.25; clamped at 0.08.
    auto const samples = uniform_samples(200, 1.0, 22.0, 14.7);
    at::MafTuneOptions opts;
    opts.gain          = 0.5;
    opts.max_delta_pct = 0.08;
    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[0].proposed_value == Catch::Approx(1.0 * 1.08));
}

TEST_CASE("tune_maf clamping is symmetric on the negative side",
          "[autotune][maf][clamp]") {
    std::vector<double> axis{1.0};
    std::vector<double> current{1.0};
    auto const samples = uniform_samples(200, 1.0, 8.0, 14.7);  // very rich
    at::MafTuneOptions opts;
    opts.gain          = 0.5;
    opts.max_delta_pct = 0.05;
    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[0].proposed_value == Catch::Approx(1.0 * 0.95));
}

// ---------------------------------------------------------------------
// tune_maf — argument validation
// ---------------------------------------------------------------------

TEST_CASE("tune_maf rejects an empty axis",
          "[autotune][maf][error]") {
    std::vector<double>        axis;
    std::vector<double>        current;
    std::vector<at::MafSample> samples;
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("tune_maf rejects axis / scaling length mismatch",
          "[autotune][maf][error]") {
    std::vector<double>        axis{0.0, 1.0};
    std::vector<double>        current{1.0, 1.0, 1.0};
    std::vector<at::MafSample> samples;
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("tune_maf rejects an unsorted axis",
          "[autotune][maf][error]") {
    std::vector<double>        axis{0.0, 1.0, 0.5};
    std::vector<double>        current{1.0, 1.0, 1.0};
    std::vector<at::MafSample> samples;
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}
