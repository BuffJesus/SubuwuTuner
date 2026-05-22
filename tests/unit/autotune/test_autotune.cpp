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
    s.maf_voltage = 1.5;
    s.actual_afr = 14.7;
    s.commanded_afr = 14.7;
    s.rpm = 2500.0;
    s.rpm_rate = 0.0;
    s.throttle_pct = 25.0;
    s.throttle_rate = 0.0;
    s.coolant_c = 90.0;
    s.iat_c = 25.0;
    s.closed_loop = false;
    s.knock_in_window = false;
    s.limp_mode = false;
    return s;
}

// Convenience: build N identical samples at a given voltage and AFR.
std::vector<at::MafSample> uniform_samples(std::size_t n, double voltage, double actual_afr,
                                           double commanded_afr) {
    auto const base = warm_baseline_sample();
    std::vector<at::MafSample> out(n, base);
    for (auto &s : out) {
        s.maf_voltage = voltage;
        s.actual_afr = actual_afr;
        s.commanded_afr = commanded_afr;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------
// Helpers under test
// ---------------------------------------------------------------------

TEST_CASE("nearest_cell picks the closest axis breakpoint", "[autotune][helpers]") {
    std::vector<double> axis{0.0, 0.5, 1.0, 1.5, 2.0};
    REQUIRE(at::nearest_cell(0.0, axis) == 0);
    REQUIRE(at::nearest_cell(0.24, axis) == 0);
    REQUIRE(at::nearest_cell(0.26, axis) == 1);
    REQUIRE(at::nearest_cell(1.5, axis) == 3);
    REQUIRE(at::nearest_cell(2.5, axis) == 4); // past-end clamps to last
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

TEST_CASE("trimmed_mean ignores extreme outliers when they cross the tail", "[autotune][helpers]") {
    // 12 values where the lowest and the highest are outliers far from
    // the rest. floor(12 * 0.1) = 1 -> drop 1 each side, mean of the
    // remaining 10.
    std::vector<double> vs{-100, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 100};
    REQUIRE(at::trimmed_mean(vs, 0.10) == Catch::Approx(1.0));
}

TEST_CASE("sample_passes_gates rejects cold-coolant samples", "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto s = warm_baseline_sample();
    s.coolant_c = 60.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
}

TEST_CASE("sample_passes_gates rejects degenerate AFR ratios", "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto s = warm_baseline_sample();
    s.commanded_afr = 0.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
    s = warm_baseline_sample();
    s.actual_afr = std::nan("");
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
}

TEST_CASE("sample_passes_gates rejects mid-knock samples by default", "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto s = warm_baseline_sample();
    s.knock_in_window = true;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
    opts.reject_knock_window = false;
    REQUIRE(at::sample_passes_gates(s, opts));
}

TEST_CASE("sample_passes_gates rejects large RPM / throttle transients", "[autotune][gates]") {
    at::MafTuneOptions opts;
    auto s = warm_baseline_sample();
    s.rpm_rate = 1000.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
    s = warm_baseline_sample();
    s.throttle_rate = 100.0;
    REQUIRE_FALSE(at::sample_passes_gates(s, opts));
}

// ---------------------------------------------------------------------
// tune_maf — happy paths
// ---------------------------------------------------------------------

TEST_CASE("tune_maf scales up a cell whose samples ran lean", "[autotune][maf]") {
    std::vector<double> axis{0.0, 0.5, 1.0, 1.5, 2.0};
    std::vector<double> current{1.0, 1.0, 1.0, 1.0, 1.0};
    // 200 samples all at MAF=1.5 V (-> cell 3), 5% lean: 15.4 vs 14.7.
    auto const samples = uniform_samples(200, /*v=*/1.5,
                                         /*actual=*/15.4,
                                         /*commanded=*/14.7);
    at::MafTuneOptions opts;
    opts.gain = 0.5;
    opts.max_delta_pct = 0.08;
    opts.min_samples_per_cell = 50;

    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 200);
    REQUIRE(r->cells.size() == axis.size());
    // Cells without samples are unchanged with confidence 0.
    for (std::size_t i = 0; i < r->cells.size(); ++i) {
        if (i == 3)
            continue;
        REQUIRE(r->cells[i].proposed_value == 1.0);
        REQUIRE(r->cells[i].confidence == 0.0);
        REQUIRE(r->cells[i].samples_used == 0);
    }
    // Cell 3 picks up the 5% lean error scaled by gain=0.5 ->
    // delta_pct = (15.4/14.7 - 1) * 0.5 = 0.02380952...
    auto const &c3 = r->cells[3];
    REQUIRE(c3.samples_used == 200);
    REQUIRE(c3.mean_error == Catch::Approx(15.4 / 14.7));
    double const expected_delta = (15.4 / 14.7 - 1.0) * 0.5;
    REQUIRE(c3.proposed_value == Catch::Approx(1.0 * (1.0 + expected_delta)));
    REQUIRE(c3.confidence > 0.5);
}

TEST_CASE("tune_maf scales down a cell whose samples ran rich", "[autotune][maf]") {
    std::vector<double> axis{0.5, 1.0, 1.5};
    std::vector<double> current{2.0, 2.0, 2.0};
    auto const samples = uniform_samples(100, 1.0, 13.0, 14.7); // rich
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[1].samples_used == 100);
    REQUIRE(r->cells[1].proposed_value < current[1]);
}

// ---------------------------------------------------------------------
// tune_maf — gates and sufficiency
// ---------------------------------------------------------------------

TEST_CASE("tune_maf leaves cells unchanged when below min samples",
          "[autotune][maf][min-samples]") {
    std::vector<double> axis{0.5, 1.0, 1.5};
    std::vector<double> current{1.0, 1.0, 1.0};
    auto const samples = uniform_samples(10, 1.0, 15.4, 14.7);
    at::MafTuneOptions opts;
    opts.min_samples_per_cell = 50;

    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 10);
    REQUIRE(r->cells[1].samples_used == 10);
    REQUIRE(r->cells[1].proposed_value == 1.0);
    REQUIRE(r->cells[1].confidence == 0.0);
}

TEST_CASE("tune_maf filters out cold-coolant samples entirely", "[autotune][maf][gates]") {
    std::vector<double> axis{0.5, 1.0, 1.5};
    std::vector<double> current{1.0, 1.0, 1.0};
    auto samples = uniform_samples(200, 1.0, 15.4, 14.7);
    for (auto &s : samples)
        s.coolant_c = 60.0; // cold engine
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 0);
    REQUIRE(r->cells[1].samples_used == 0);
    REQUIRE(r->cells[1].proposed_value == 1.0);
}

// ---------------------------------------------------------------------
// tune_maf — clamping
// ---------------------------------------------------------------------

TEST_CASE("tune_maf clamps proposals to max_delta_pct", "[autotune][maf][clamp]") {
    std::vector<double> axis{1.0};
    std::vector<double> current{1.0};
    // Severely lean: actual=22 vs commanded=14.7 (~49% high). With
    // gain=0.5 the unclamped delta would be ~0.25; clamped at 0.08.
    auto const samples = uniform_samples(200, 1.0, 22.0, 14.7);
    at::MafTuneOptions opts;
    opts.gain = 0.5;
    opts.max_delta_pct = 0.08;
    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[0].proposed_value == Catch::Approx(1.0 * 1.08));
}

TEST_CASE("tune_maf clamping is symmetric on the negative side", "[autotune][maf][clamp]") {
    std::vector<double> axis{1.0};
    std::vector<double> current{1.0};
    auto const samples = uniform_samples(200, 1.0, 8.0, 14.7); // very rich
    at::MafTuneOptions opts;
    opts.gain = 0.5;
    opts.max_delta_pct = 0.05;
    auto const r = at::tune_maf(axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[0].proposed_value == Catch::Approx(1.0 * 0.95));
}

// ---------------------------------------------------------------------
// tune_maf — argument validation
// ---------------------------------------------------------------------

TEST_CASE("tune_maf rejects an empty axis", "[autotune][maf][error]") {
    std::vector<double> axis;
    std::vector<double> current;
    std::vector<at::MafSample> samples;
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("tune_maf rejects axis / scaling length mismatch", "[autotune][maf][error]") {
    std::vector<double> axis{0.0, 1.0};
    std::vector<double> current{1.0, 1.0, 1.0};
    std::vector<at::MafSample> samples;
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("tune_maf rejects an unsorted axis", "[autotune][maf][error]") {
    std::vector<double> axis{0.0, 1.0, 0.5};
    std::vector<double> current{1.0, 1.0, 1.0};
    std::vector<at::MafSample> samples;
    auto const r = at::tune_maf(axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------
// smooth_proposals
// ---------------------------------------------------------------------

namespace {

// Hand-construct a MafTuneResult so the smoothing tests don't depend
// on tune_maf's full pipeline.
at::CellProposal make_cell(std::size_t idx, double current, double proposed, double confidence,
                           std::size_t samples = 100, double mean_error = 1.0) {
    at::CellProposal c;
    c.cell_index = idx;
    c.current_value = current;
    c.proposed_value = proposed;
    c.mean_error = mean_error;
    c.samples_used = samples;
    c.confidence = confidence;
    return c;
}

} // namespace

TEST_CASE("smooth_proposals pulls a low-confidence cell toward a "
          "high-confidence neighbor",
          "[autotune][smooth]") {
    at::MafTuneResult in;
    in.cells.push_back(make_cell(0, /*cur=*/1.0, /*prop=*/1.05,
                                 /*conf=*/1.0)); // strong "lean"
    in.cells.push_back(make_cell(1, /*cur=*/1.0, /*prop=*/1.00,
                                 /*conf=*/0.0)); // no data
    // With self_w = 0 and neighbor confidence = 1.0,
    // smoothed[1] = (0.25 * 1.05) / (0.25) = 1.05.
    auto const out = at::smooth_proposals(in, /*max_delta_pct=*/0.10,
                                          /*neighbor_weight=*/0.25);
    REQUIRE(out.cells[1].proposed_value == Catch::Approx(1.05));
    // The high-confidence cell stays put.
    REQUIRE(out.cells[0].proposed_value == Catch::Approx(1.05));
    // Confidence is preserved across smoothing.
    REQUIRE(out.cells[1].confidence == 0.0);
}

TEST_CASE("smooth_proposals re-clamps to max_delta_pct after blending",
          "[autotune][smooth][clamp]") {
    // Neighbor pull would push this cell's proposed_value to 1.10, but
    // max_delta_pct=0.05 caps the change to ±5% from current=1.0.
    at::MafTuneResult in;
    in.cells.push_back(make_cell(0, 1.0, 1.10, 1.0));
    in.cells.push_back(make_cell(1, 1.0, 1.00, 0.0));
    auto const out = at::smooth_proposals(in, /*max_delta_pct=*/0.05, 0.25);
    REQUIRE(out.cells[1].proposed_value <= 1.05);
    REQUIRE(out.cells[1].proposed_value >= 0.95);
}

TEST_CASE("smooth_proposals keeps a high-confidence cell near its own value",
          "[autotune][smooth]") {
    at::MafTuneResult in;
    in.cells.push_back(make_cell(0, 1.0, 1.00, 1.0));
    in.cells.push_back(make_cell(1, 1.0, 1.05, 1.0)); // middle, lean
    in.cells.push_back(make_cell(2, 1.0, 1.00, 1.0));
    auto const out = at::smooth_proposals(in, 0.10, 0.25);
    // num = 1.0*1.05 + 0.25*1.00 + 0.25*1.00 = 1.55
    // den = 1.0 + 0.25 + 0.25                = 1.50
    // smoothed = 1.55 / 1.50                 ≈ 1.0333
    // The cell loses about 0.017 to neighbor pull but stays well
    // above 1.00 because its own confidence dominated the weighted sum.
    REQUIRE(out.cells[1].proposed_value > 1.0);
    REQUIRE(out.cells[1].proposed_value < 1.05);
    REQUIRE(out.cells[1].proposed_value == Catch::Approx(1.55 / 1.50));
}

TEST_CASE("smooth_proposals handles edge cells (only one neighbor)", "[autotune][smooth][edge]") {
    at::MafTuneResult in;
    in.cells.push_back(make_cell(0, 1.0, 1.00, 0.0)); // edge, no data
    in.cells.push_back(make_cell(1, 1.0, 1.05, 1.0)); // confident
    auto const out = at::smooth_proposals(in, 0.10, 0.25);
    // Cell 0: self_w=0, right neighbor weight = 0.25, neighbor proposed=1.05.
    // smoothed[0] = 0.25*1.05 / 0.25 = 1.05.
    REQUIRE(out.cells[0].proposed_value == Catch::Approx(1.05));
}

TEST_CASE("smooth_proposals leaves all-zero-confidence cells unchanged", "[autotune][smooth]") {
    at::MafTuneResult in;
    in.cells.push_back(make_cell(0, 1.0, 1.00, 0.0));
    in.cells.push_back(make_cell(1, 1.0, 1.00, 0.0));
    in.cells.push_back(make_cell(2, 1.0, 1.00, 0.0));
    auto const out = at::smooth_proposals(in, 0.10, 0.25);
    REQUIRE(out.cells[0].proposed_value == 1.00);
    REQUIRE(out.cells[1].proposed_value == 1.00);
    REQUIRE(out.cells[2].proposed_value == 1.00);
}

TEST_CASE("smooth_proposals on a 1-cell input returns it unchanged", "[autotune][smooth][edge]") {
    at::MafTuneResult in;
    in.cells.push_back(make_cell(0, 1.0, 1.07, 1.0));
    auto const out = at::smooth_proposals(in, 0.10, 0.25);
    REQUIRE(out.cells.size() == 1);
    REQUIRE(out.cells[0].proposed_value == 1.07);
}

TEST_CASE("smooth_proposals preserves carryover fields", "[autotune][smooth]") {
    at::MafTuneResult in;
    in.total_samples = 12345;
    in.samples_after_gates = 4321;
    in.cells.push_back(make_cell(0, /*cur=*/2.5, /*prop=*/2.55,
                                 /*conf=*/0.5,
                                 /*samples=*/77,
                                 /*mean_error=*/1.02));
    in.cells.push_back(make_cell(1, 3.0, 3.0, 0.0, 0, 1.0));
    auto const out = at::smooth_proposals(in, 0.10, 0.25);

    REQUIRE(out.total_samples == 12345);
    REQUIRE(out.samples_after_gates == 4321);
    REQUIRE(out.cells.size() == 2);
    REQUIRE(out.cells[0].current_value == 2.5);
    REQUIRE(out.cells[0].samples_used == 77);
    REQUIRE(out.cells[0].confidence == 0.5);
    REQUIRE(out.cells[0].mean_error == 1.02);
    REQUIRE(out.cells[0].cell_index == 0);
}

// ---------------------------------------------------------------------
// Knock-based ignition pull
// ---------------------------------------------------------------------

namespace {

at::KnockSample warm_knock_sample() {
    at::KnockSample s;
    s.rpm = 4000.0;
    s.load = 2.0;
    s.feedback_knock = 0.0;
    s.coolant_c = 90.0;
    s.iat_c = 25.0;
    s.limp_mode = false;
    return s;
}

std::vector<at::KnockSample> uniform_knock_samples(std::size_t n, double rpm, double load,
                                                   double fbk) {
    auto const base = warm_knock_sample();
    std::vector<at::KnockSample> out(n, base);
    for (auto &s : out) {
        s.rpm = rpm;
        s.load = load;
        s.feedback_knock = fbk;
    }
    return out;
}

} // namespace

TEST_CASE("tune_knock_pull subtracts pull_step when mean knock crosses trigger",
          "[autotune][knock]") {
    // 2x2 table: rpm=[2000,4000], load=[1.0,2.0]. Hot cell is (load=2,
    // rpm=4000) -> flat index 1*2+1 = 3.
    std::vector<double> rpm_axis{2000.0, 4000.0};
    std::vector<double> load_axis{1.0, 2.0};
    std::vector<double> current{20.0, 22.0, 18.0, 24.0}; // row-major
    auto const samples = uniform_knock_samples(/*n=*/100, /*rpm=*/4000.0,
                                               /*load=*/2.0, /*fbk=*/-2.0);
    at::KnockPullOptions opts;
    opts.trigger_degrees = 1.5;
    opts.pull_step_degrees = 0.75;
    opts.min_samples_per_cell = 30;
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, samples, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->rows == 2);
    REQUIRE(r->cols == 2);
    REQUIRE(r->cells.size() == 4);
    REQUIRE(r->samples_after_gates == 100);

    auto const &hot = r->cells[3];
    REQUIRE(hot.samples_used == 100);
    REQUIRE(hot.pulled);
    REQUIRE(hot.proposed_value == Catch::Approx(24.0 - 0.75));
    REQUIRE(hot.mean_feedback_knock == Catch::Approx(-2.0));

    // The other three cells got no samples and stay put.
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE_FALSE(r->cells[i].pulled);
        REQUIRE(r->cells[i].proposed_value == current[i]);
        REQUIRE(r->cells[i].samples_used == 0);
    }
}

TEST_CASE("tune_knock_pull leaves cells alone when knock doesn't cross trigger",
          "[autotune][knock]") {
    std::vector<double> rpm_axis{2000.0};
    std::vector<double> load_axis{2.0};
    std::vector<double> current{20.0};
    // mean fbk = -1.0; trigger 1.5; threshold to fire is fbk < -1.5.
    auto const samples = uniform_knock_samples(100, 2000.0, 2.0, -1.0);
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->cells[0].pulled);
    REQUIRE(r->cells[0].proposed_value == 20.0);
}

TEST_CASE("tune_knock_pull respects min_samples_per_cell", "[autotune][knock]") {
    std::vector<double> rpm_axis{2000.0};
    std::vector<double> load_axis{2.0};
    std::vector<double> current{20.0};
    // 10 samples deep in knock; opts.min=30 -> not pulled.
    auto const samples = uniform_knock_samples(10, 2000.0, 2.0, -3.0);
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->cells[0].pulled);
    REQUIRE(r->cells[0].proposed_value == 20.0);
    REQUIRE(r->cells[0].samples_used == 10);
}

TEST_CASE("tune_knock_pull rejects cold-coolant samples", "[autotune][knock][gates]") {
    std::vector<double> rpm_axis{2000.0};
    std::vector<double> load_axis{2.0};
    std::vector<double> current{20.0};
    auto samples = uniform_knock_samples(100, 2000.0, 2.0, -2.0);
    for (auto &s : samples)
        s.coolant_c = 60.0;
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 0);
    REQUIRE_FALSE(r->cells[0].pulled);
}

TEST_CASE("tune_knock_pull rejects limp-mode samples by default", "[autotune][knock][gates]") {
    std::vector<double> rpm_axis{2000.0};
    std::vector<double> load_axis{2.0};
    std::vector<double> current{20.0};
    auto samples = uniform_knock_samples(100, 2000.0, 2.0, -2.0);
    for (auto &s : samples)
        s.limp_mode = true;
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, samples);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_after_gates == 0);
}

TEST_CASE("tune_knock_pull isolates multi-cell decisions", "[autotune][knock]") {
    std::vector<double> rpm_axis{2000.0, 4000.0};
    std::vector<double> load_axis{2.0};
    std::vector<double> current{20.0, 22.0};
    auto const a = uniform_knock_samples(50, 2000.0, 2.0, /*fbk=*/-2.0);
    auto const b = uniform_knock_samples(50, 4000.0, 2.0, /*fbk=*/0.0);
    std::vector<at::KnockSample> all;
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, all);
    REQUIRE(r.has_value());
    REQUIRE(r->cells[0].pulled);       // 2000 RPM -> pulled
    REQUIRE_FALSE(r->cells[1].pulled); // 4000 RPM -> clean
    REQUIRE(r->cells[0].proposed_value < 20.0);
    REQUIRE(r->cells[1].proposed_value == 22.0);
}

TEST_CASE("tune_knock_pull only ever subtracts (monotonic invariant)", "[autotune][knock]") {
    // Synthetic 5x4 table with a mix of knocking and clean cells; verify
    // that NO cell's proposed value exceeds its current value.
    std::vector<double> rpm_axis{1000.0, 2000.0, 3000.0, 4000.0, 5000.0};
    std::vector<double> load_axis{1.0, 2.0, 3.0, 4.0};
    std::vector<double> current(20);
    for (std::size_t i = 0; i < current.size(); ++i) {
        current[i] = 15.0 + static_cast<double>(i) * 0.5;
    }
    std::vector<at::KnockSample> all;
    // Knocking samples at every cell; with trigger 1.5 and fbk -3,
    // every cell should fire.
    for (auto load : load_axis) {
        for (auto rpm : rpm_axis) {
            auto chunk = uniform_knock_samples(50, rpm, load, -3.0);
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
    }
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, all);
    REQUIRE(r.has_value());
    for (auto const &c : r->cells) {
        REQUIRE(c.pulled);
        REQUIRE(c.proposed_value <= c.current_value);
    }
}

TEST_CASE("tune_knock_pull rejects axis / current_timing length mismatch",
          "[autotune][knock][error]") {
    std::vector<double> rpm_axis{2000.0, 4000.0};
    std::vector<double> load_axis{1.0, 2.0};
    std::vector<double> current{20.0, 22.0, 18.0}; // 3 vs 2*2=4
    std::vector<at::KnockSample> samples;
    auto const r = at::tune_knock_pull(rpm_axis, load_axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("tune_knock_pull rejects empty axes", "[autotune][knock][error]") {
    std::vector<double> empty;
    std::vector<double> load_axis{1.0};
    std::vector<double> current;
    std::vector<at::KnockSample> samples;
    auto const r = at::tune_knock_pull(empty, load_axis, current, samples);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------
// apply_knock_add_back
// ---------------------------------------------------------------------

namespace {

// Build a 3-cell-wide single-row KnockPullResult with the given per-
// cell (current, proposed, mean_knock, samples_used, pulled) tuples.
// Convenient for exercising add-back in isolation.
struct AddBackCell {
    double current{0.0};
    double proposed{0.0};
    double mean_knock{0.0};
    std::size_t samples_used{0};
    bool pulled{false};
};

at::KnockPullResult build_pull_result(std::vector<AddBackCell> const &cells) {
    at::KnockPullResult r;
    r.rows = 1;
    r.cols = cells.size();
    for (std::size_t i = 0; i < cells.size(); ++i) {
        at::KnockCellProposal c;
        c.cell_index = i;
        c.current_value = cells[i].current;
        c.proposed_value = cells[i].proposed;
        c.mean_feedback_knock = cells[i].mean_knock;
        c.samples_used = cells[i].samples_used;
        c.pulled = cells[i].pulled;
        r.cells.push_back(c);
    }
    return r;
}

} // namespace

TEST_CASE("apply_knock_add_back: disabled -> passthrough", "[autotune][knock][addback]") {
    auto const in = build_pull_result({
        {20.0, 20.0, 0.0, 100, false},
        {22.0, 21.25, -2.0, 80, true},
    });
    at::KnockAddBackOptions opts; // enabled = false by default
    auto const out = at::apply_knock_add_back(in, opts);
    REQUIRE(out.cells.size() == 2);
    CHECK(out.cells[0].proposed_value == in.cells[0].proposed_value);
    CHECK(out.cells[1].proposed_value == in.cells[1].proposed_value);
}

TEST_CASE("apply_knock_add_back: adds to clean unpulled cells", "[autotune][knock][addback]") {
    auto const in = build_pull_result({
        {20.0, 20.0, 0.0, 100, false},  // clean, eligible
        {22.0, 21.25, -2.0, 80, true},  // pulled — skip
        {18.0, 18.0, 0.02, 100, false}, // clean within tolerance
    });
    at::KnockAddBackOptions opts;
    opts.enabled = true;
    auto const out = at::apply_knock_add_back(in, opts);
    REQUIRE(out.cells.size() == 3);
    CHECK(out.cells[0].proposed_value == 20.0 + opts.add_step_degrees);
    CHECK_FALSE(out.cells[0].pulled);
    CHECK(out.cells[1].proposed_value == 21.25);
    CHECK(out.cells[1].pulled);
    CHECK(out.cells[2].proposed_value == 18.0 + opts.add_step_degrees);
}

TEST_CASE("apply_knock_add_back: under-sampled cells are skipped", "[autotune][knock][addback]") {
    auto const in = build_pull_result({
        {20.0, 20.0, 0.0, 10, false}, // clean but underpowered
    });
    at::KnockAddBackOptions opts;
    opts.enabled = true;
    opts.min_clean_samples_per_cell = 50;
    auto const out = at::apply_knock_add_back(in, opts);
    CHECK(out.cells[0].proposed_value == 20.0);
}

TEST_CASE("apply_knock_add_back: cells with sustained knock are skipped",
          "[autotune][knock][addback]") {
    auto const in = build_pull_result({
        {20.0, 20.0, -0.5, 100, false}, // ECU was pulling, even if
                                        // the algorithm didn't reach
                                        // its own trigger
    });
    at::KnockAddBackOptions opts;
    opts.enabled = true;
    auto const out = at::apply_knock_add_back(in, opts);
    CHECK(out.cells[0].proposed_value == 20.0);
}

TEST_CASE("apply_knock_add_back: threshold and step are configurable",
          "[autotune][knock][addback]") {
    auto const in = build_pull_result({
        {20.0, 20.0, -0.10, 100, false},
    });
    at::KnockAddBackOptions opts;
    opts.enabled = true;
    opts.clean_threshold_degrees = 0.05;
    auto const out_strict = at::apply_knock_add_back(in, opts);
    CHECK(out_strict.cells[0].proposed_value == 20.0);

    opts.clean_threshold_degrees = 0.20;
    opts.add_step_degrees = 0.25;
    auto const out_lenient = at::apply_knock_add_back(in, opts);
    CHECK(out_lenient.cells[0].proposed_value == 20.25);
}
