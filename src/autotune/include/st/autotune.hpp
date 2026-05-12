// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_AUTOTUNE_HPP
#define ST_AUTOTUNE_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <span>
#include <vector>

// =====================================================================
// st::autotune — proposals over calibration tables driven by log data.
// =====================================================================
//
// Auto-tune is a function:
//     f : (Samples, Definition, Tables, Options)  →  Proposals
//
// It has no dependency on the GUI, on hardware, or on an ECU
// connection. The first algorithm in this module is MAF auto-tune
// (docs/12 §"MAF auto-tune"), which proposes per-cell corrections to a
// 1D MAF voltage→g/s scaling table from logged (voltage, actual AFR,
// commanded AFR) tuples. Knock-based ignition pull, closed-loop trim
// integration, boost auto-trim, and idle target trim are separate
// algorithms that share this module's posture: data-quality gates
// first, conservative deltas, every proposal a draft for human review.
//
// What auto-tune NEVER does:
//   * Flash the ECU. Proposals are drafts. The user reviews and
//     commits manually.
//   * Disable the engine-safety linter — see docs/06-legal-ethics.md.
//     Linting is layered on top of the proposals returned here.

namespace st::autotune {

// One log sample's worth of inputs to MAF auto-tune. Pre-decimated
// from the raw LogStream — extracting these from a log is a separate
// CLI/library slice that converts log channels into samples by name.
//
// `rpm_rate` and `throttle_rate` are derived (per-second) deltas; the
// caller computes them from successive samples. The boolean flags are
// the data-quality flags the algorithm uses to reject samples.
struct MafSample {
    double maf_voltage{0.0};       // V — the table's independent axis
    double actual_afr{0.0};        // wideband AFR
    double commanded_afr{0.0};     // ECU's AFR target
    double rpm{0.0};
    double rpm_rate{0.0};          // RPM/s
    double throttle_pct{0.0};
    double throttle_rate{0.0};     // %/s
    double coolant_c{0.0};
    double iat_c{0.0};
    bool   closed_loop{false};
    bool   knock_in_window{false}; // any knock event in the last 250 ms
    bool   limp_mode{false};       // ECU in fail / limp mode
};

// Tunable knobs for MAF auto-tune. Defaults are deliberately
// conservative — see docs/12 §"Data-quality gates".
struct MafTuneOptions {
    // Algorithm shape.
    double      gain                 = 0.5;    // multiplies the observed error
    double      max_delta_pct        = 0.08;   // ±8% per pass
    std::size_t min_samples_per_cell = 50;
    double      trim_fraction        = 0.10;   // trimmed-mean tail to drop

    // Data-quality gates.
    double      min_coolant_c        = 80.0;
    double      min_iat_c            = -20.0;
    double      max_iat_c            = 80.0;
    double      max_rpm_rate         = 500.0;  // RPM/s
    double      max_throttle_rate    = 25.0;   // %/s
    bool        reject_knock_window  = true;
    bool        reject_limp_mode     = true;
    bool        require_open_loop    = false;  // include both modes by default
};

// One cell's worth of result. `samples_used` is the count after gates
// AND after bucket assignment to this cell; it can be zero even when
// `total_samples` is large. `confidence` is in [0, 1] and ramps up
// with sample count.
struct CellProposal {
    std::size_t cell_index{0};
    double      current_value{0.0};
    double      proposed_value{0.0};
    double      mean_error{1.0};     // trimmed mean of (actual/commanded)
    std::size_t samples_used{0};
    double      confidence{0.0};
};

struct MafTuneResult {
    std::vector<CellProposal> cells;             // one per axis breakpoint
    std::size_t               total_samples{0};
    std::size_t               samples_after_gates{0};
};

// Check whether a sample passes the configured gates. Exposed so tests
// (and a future CLI summary) can show which samples were rejected and
// why without having to re-implement the logic.
[[nodiscard]] bool sample_passes_gates(MafSample const     &s,
                                       MafTuneOptions const &opts) noexcept;

// Pick the cell-index whose axis breakpoint is closest to `voltage`.
// Ties go to the lower index. The axis is assumed to be sorted
// ascending; an empty axis returns 0.
[[nodiscard]] std::size_t nearest_cell(double                  voltage,
                                       std::span<double const> axis) noexcept;

// Trimmed mean: discard `trim_fraction` of the lowest and the highest
// values, then average the rest. With `trim_fraction = 0` reduces to
// the arithmetic mean. With an empty span returns 0.0. Allocates a
// sorted scratch copy internally; takes the span by value.
[[nodiscard]] double trimmed_mean(std::span<double const> values,
                                  double                  trim_fraction);

// The canonical MAF auto-tune entry point. Walks every sample, gates,
// buckets to the nearest axis cell, computes a trimmed-mean error
// ratio per cell, applies gain and clamp, and emits proposals. Cells
// with fewer than `opts.min_samples_per_cell` retained samples are
// left unchanged at their current value and reported with
// confidence = 0.
//
// `axis` and `current_scaling` must have the same length. The result
// has one CellProposal per axis breakpoint, in order.
[[nodiscard]] Result<MafTuneResult> tune_maf(
    std::span<double const>    axis,
    std::span<double const>    current_scaling,
    std::span<MafSample const> samples,
    MafTuneOptions const      &opts = {});

// Confidence-weighted neighbor smoothing pass per docs/12 §"MAF
// auto-tune". Each cell's smoothed proposal is the weighted average of
// (own proposal × own confidence) and (each neighbor's proposal ×
// neighbor's confidence × `neighbor_weight`). High-confidence cells
// pull low-confidence neighbors toward themselves rather than toward
// the original `current_value`. Edge cells have only one neighbor.
//
// After smoothing, every cell's proposal is re-clamped to within
// `±max_delta_pct` of its own `current_value` so the per-pass safety
// bound from `tune_maf` is preserved across the second pass.
//
// If all neighbors and the cell itself have confidence 0, the cell is
// left unchanged. `neighbor_weight` defaults to 0.25 — high-confidence
// cells get the dominant say at their own positions but neighbors
// still tug.
//
// Pure: returns a new `MafTuneResult`. `current_value`, `samples_used`,
// `mean_error`, `cell_index`, and `confidence` are carried over from
// the input; only `proposed_value` changes. Total-samples and
// samples-after-gates fields pass through unchanged.
[[nodiscard]] MafTuneResult smooth_proposals(
    MafTuneResult const &input,
    double               max_delta_pct,
    double               neighbor_weight = 0.25);

} // namespace st::autotune

#endif // ST_AUTOTUNE_HPP
