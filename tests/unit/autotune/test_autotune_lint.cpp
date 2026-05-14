// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace at = st::autotune;

namespace {

// Build a MafTuneResult with the given proposed values. All cells
// otherwise default — these tests inspect the proposal shape, not the
// upstream stats fields.
at::MafTuneResult build_result(std::vector<double> const &current,
                               std::vector<double> const &proposed) {
    at::MafTuneResult r;
    r.cells.reserve(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) {
        at::CellProposal c;
        c.cell_index     = i;
        c.current_value  = current[i];
        c.proposed_value = proposed[i];
        c.samples_used   = 100;
        c.confidence     = 1.0;
        r.cells.push_back(c);
    }
    return r;
}

}  // namespace

TEST_CASE("lint_maf_proposal: clean proposal -> no violations",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0, 3.0, 4.0};
    std::vector<double> current  {2.0, 4.0, 6.0, 8.0};
    std::vector<double> proposed {2.1, 4.1, 6.1, 8.1};  // +5% across

    auto const r = build_result(current, proposed);
    auto const v = at::lint_maf_proposal(axis, current, r);
    CHECK(v.empty());
}

TEST_CASE("lint_maf_proposal: non-monotonic flagged",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0, 3.0};
    std::vector<double> current  {1.0, 2.0, 3.0};
    // Cell 1 (proposed 1.5) > Cell 2 (proposed 1.0) — backwards.
    std::vector<double> proposed {0.5, 1.5, 1.0};

    auto const r = build_result(current, proposed);
    auto const v = at::lint_maf_proposal(axis, current, r);
    REQUIRE_FALSE(v.empty());
    bool found = false;
    for (auto const &x : v) {
        if (x.kind == at::LintViolationKind::NonMonotonic && x.cell_index == 1) {
            found = true;
            CHECK(x.message.find("falls") != std::string::npos);
        }
    }
    CHECK(found);
}

TEST_CASE("lint_maf_proposal: monotonic check ignores tiny FP noise",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0};
    std::vector<double> current  {2.0, 2.0};
    // Proposed values equal within floating-point noise.
    std::vector<double> proposed {2.0, 2.0 - 1e-12};

    auto const r = build_result(current, proposed);
    at::LintOptions opts;
    opts.enforce_smoothness = false;   // isolate the monotonic check
    auto const v = at::lint_maf_proposal(axis, current, r, opts);
    CHECK(v.empty());
}

TEST_CASE("lint_maf_proposal: step discontinuity flagged",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0, 3.0};
    // Original calibration: steady steps of 1.0 between cells.
    std::vector<double> current  {1.0, 2.0, 3.0};
    // Proposed: cell 1 unchanged, cell 2 jumped to 5.0 — step 1→2 is
    // now 3.0 vs original 1.0, a 200% deviation.
    std::vector<double> proposed {1.0, 2.0, 5.0};

    auto const r = build_result(current, proposed);
    auto const v = at::lint_maf_proposal(axis, current, r);
    REQUIRE_FALSE(v.empty());
    bool found = false;
    for (auto const &x : v) {
        if (x.kind == at::LintViolationKind::StepDiscontinuity
            && x.cell_index == 1) {
            found = true;
            CHECK(x.message.find("deviates") != std::string::npos);
        }
    }
    CHECK(found);
}

TEST_CASE("lint_maf_proposal: smoothness check tolerates proportional scaling",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0, 3.0, 4.0};
    std::vector<double> current  {1.0, 2.0, 3.0, 4.0};
    // Every cell scaled by 1.05; every step also scaled by 1.05 (5%
    // change from original step). Well under the default 50% bound.
    std::vector<double> proposed {1.05, 2.10, 3.15, 4.20};

    auto const r = build_result(current, proposed);
    auto const v = at::lint_maf_proposal(axis, current, r);
    CHECK(v.empty());
}

TEST_CASE("lint_maf_proposal: enforce flags can disable each check",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0, 3.0};
    std::vector<double> current  {1.0, 2.0, 3.0};
    std::vector<double> proposed {0.5, 1.5, 1.0};   // both backwards AND a step jump

    auto const r = build_result(current, proposed);

    SECTION("monotonic only") {
        at::LintOptions opts;
        opts.enforce_smoothness = false;
        auto const v = at::lint_maf_proposal(axis, current, r, opts);
        REQUIRE_FALSE(v.empty());
        for (auto const &x : v) {
            CHECK(x.kind == at::LintViolationKind::NonMonotonic);
        }
    }
    SECTION("smoothness only") {
        at::LintOptions opts;
        opts.enforce_monotonic = false;
        auto const v = at::lint_maf_proposal(axis, current, r, opts);
        REQUIRE_FALSE(v.empty());
        for (auto const &x : v) {
            CHECK(x.kind == at::LintViolationKind::StepDiscontinuity);
        }
    }
    SECTION("both disabled") {
        at::LintOptions opts;
        opts.enforce_monotonic  = false;
        opts.enforce_smoothness = false;
        auto const v = at::lint_maf_proposal(axis, current, r, opts);
        CHECK(v.empty());
    }
}

TEST_CASE("lint_maf_proposal: empty / single-cell proposals are clean",
          "[autotune][lint]") {
    std::vector<double> empty_axis;
    std::vector<double> empty_current;
    auto const r0 = build_result(empty_current, empty_current);
    CHECK(at::lint_maf_proposal(empty_axis, empty_current, r0).empty());

    std::vector<double> one_axis{2.0};
    std::vector<double> one_cur {3.0};
    std::vector<double> one_prop{3.5};
    auto const r1 = build_result(one_cur, one_prop);
    CHECK(at::lint_maf_proposal(one_axis, one_cur, r1).empty());
}

TEST_CASE("lint_maf_proposal: flat original region tolerates small "
          "proposed step",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0};
    std::vector<double> current  {2.0, 2.0};       // flat
    std::vector<double> proposed {2.0, 2.0 + 1e-9}; // negligible change

    auto const r = build_result(current, proposed);
    auto const v = at::lint_maf_proposal(axis, current, r);
    CHECK(v.empty());
}

TEST_CASE("lint_maf_proposal: flat original region flags non-trivial "
          "proposed step",
          "[autotune][lint]") {
    std::vector<double> axis     {1.0, 2.0};
    std::vector<double> current  {2.0, 2.0};       // flat
    std::vector<double> proposed {2.0, 5.0};       // 3.0 jump out of nothing

    auto const r = build_result(current, proposed);
    at::LintOptions opts;
    opts.enforce_monotonic = false;   // proposal IS monotonic; isolate smoothness
    auto const v = at::lint_maf_proposal(axis, current, r, opts);
    REQUIRE_FALSE(v.empty());
    CHECK(v.front().kind == at::LintViolationKind::StepDiscontinuity);
}

TEST_CASE("lint_kind_name: stable labels for each kind",
          "[autotune][lint]") {
    CHECK(std::string{at::lint_kind_name(
              at::LintViolationKind::NonMonotonic)} == "non-monotonic");
    CHECK(std::string{at::lint_kind_name(
              at::LintViolationKind::StepDiscontinuity)}
          == "step discontinuity");
}

// ---------------------------------------------------------------------
// lint_knock_proposal
// ---------------------------------------------------------------------

namespace {

// Build a KnockPullResult from a flat row-major proposed-timing
// vector. Per-cell `current_value` defaults to `proposed_value + 1.0`
// so the helper trivially registers as "pulled by 1.0" — the lint
// doesn't care about pulled/not, only the proposed surface shape.
at::KnockPullResult build_knock_result(std::size_t                rows,
                                        std::size_t                cols,
                                        std::vector<double> const &proposed) {
    at::KnockPullResult r;
    r.rows = rows;
    r.cols = cols;
    r.cells.reserve(proposed.size());
    for (std::size_t i = 0; i < proposed.size(); ++i) {
        at::KnockCellProposal c;
        c.cell_index     = i;
        c.current_value  = proposed[i] + 1.0;
        c.proposed_value = proposed[i];
        c.samples_used   = 50;
        c.pulled         = true;
        r.cells.push_back(c);
    }
    return r;
}

}  // namespace

TEST_CASE("lint_knock_proposal: smooth surface -> no violations",
          "[autotune][lint][knock]") {
    std::vector<double> rpm_axis {2000.0, 3000.0, 4000.0};
    std::vector<double> load_axis{1.5, 2.5};
    // 2 rows (load) × 3 cols (rpm). Gentle gradient: 1° step.
    std::vector<double> timing {
        22.0, 21.0, 20.0,
        20.0, 19.0, 18.0,
    };

    auto const r = build_knock_result(2, 3, timing);
    auto const v = at::lint_knock_proposal(rpm_axis, load_axis, r);
    CHECK(v.empty());
}

TEST_CASE("lint_knock_proposal: RPM-axis discontinuity flagged",
          "[autotune][lint][knock]") {
    std::vector<double> rpm_axis {2000.0, 3000.0, 4000.0};
    std::vector<double> load_axis{2.5};
    // Single load row. Cell at rpm=3000 dropped by 5° vs neighbors —
    // bigger than the default 3° threshold.
    std::vector<double> timing {20.0, 15.0, 20.0};

    auto const r = build_knock_result(1, 3, timing);
    auto const v = at::lint_knock_proposal(rpm_axis, load_axis, r);
    REQUIRE(v.size() == 2);   // (0→1) and (1→2)
    for (auto const &x : v) {
        CHECK(x.kind == at::LintViolationKind::StepDiscontinuity);
        CHECK(x.message.find("RPM") != std::string::npos);
    }
}

TEST_CASE("lint_knock_proposal: load-axis discontinuity flagged",
          "[autotune][lint][knock]") {
    std::vector<double> rpm_axis {3000.0};
    std::vector<double> load_axis{1.0, 2.0, 3.0};
    // Single RPM column. Cell at load=2.0 dropped by 5° — flags both
    // (load 1→2) and (load 2→3).
    std::vector<double> timing {20.0, 15.0, 20.0};

    auto const r = build_knock_result(3, 1, timing);
    auto const v = at::lint_knock_proposal(rpm_axis, load_axis, r);
    REQUIRE(v.size() == 2);
    for (auto const &x : v) {
        CHECK(x.kind == at::LintViolationKind::StepDiscontinuity);
        CHECK(x.message.find("load") != std::string::npos);
    }
}

TEST_CASE("lint_knock_proposal: enforce flag disables the check",
          "[autotune][lint][knock]") {
    std::vector<double> rpm_axis {2000.0, 3000.0};
    std::vector<double> load_axis{2.5};
    std::vector<double> timing {20.0, 10.0};   // 10° step

    auto const r = build_knock_result(1, 2, timing);
    at::KnockLintOptions opts;
    opts.enforce_neighbor_smoothness = false;
    CHECK(at::lint_knock_proposal(rpm_axis, load_axis, r, opts).empty());
}

TEST_CASE("lint_knock_proposal: threshold is configurable",
          "[autotune][lint][knock]") {
    std::vector<double> rpm_axis {2000.0, 3000.0};
    std::vector<double> load_axis{2.5};
    std::vector<double> timing {20.0, 16.0};   // 4° step

    auto const r = build_knock_result(1, 2, timing);
    at::KnockLintOptions opts;
    // Default 3° → flagged.
    CHECK_FALSE(at::lint_knock_proposal(rpm_axis, load_axis, r, opts).empty());
    // Raised to 5° → clean.
    opts.max_neighbor_step_degrees = 5.0;
    CHECK(at::lint_knock_proposal(rpm_axis, load_axis, r, opts).empty());
}

TEST_CASE("lint_knock_proposal: empty result is clean",
          "[autotune][lint][knock]") {
    std::vector<double> empty;
    at::KnockPullResult r0;
    r0.rows = 0;
    r0.cols = 0;
    CHECK(at::lint_knock_proposal(empty, empty, r0).empty());
}

TEST_CASE("lint_knock_proposal: mismatched cells.size() emits a "
          "synthetic violation",
          "[autotune][lint][knock]") {
    std::vector<double> empty;
    at::KnockPullResult r;
    r.rows = 2;
    r.cols = 2;
    r.cells.resize(3);   // expected 4 — kernel bug or hand-crafted input

    auto const v = at::lint_knock_proposal(empty, empty, r);
    REQUIRE(v.size() == 1);
    CHECK(v.front().kind == at::LintViolationKind::StepDiscontinuity);
    CHECK(v.front().message.find("malformed") != std::string::npos);
}
