// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_AUTOTUNE_HPP
#define ST_AUTOTUNE_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <span>
#include <string_view>
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

// =====================================================================
// Knock-based ignition pull
// =====================================================================
//
// Per docs/12 §"Knock-based ignition pull". The ECU exposes Feedback
// Knock Correction — its own per-cell timing-pull values driven by the
// knock sensors. When the mean of those values for a given (RPM, load)
// cell stays below `-trigger_degrees` over enough samples, that cell
// is genuinely seeing knock and the calibration's commanded timing in
// that cell is too high. The algorithm subtracts `pull_step_degrees`
// from the cell's current timing.
//
// Monotonic-subtract: the algorithm only ever LOWERS a cell's timing.
// There is no add-back path here. The docs describe an opt-in
// follow-up that adds timing back in cells where Feedback Knock has
// remained at 0 for the entire log; that's a separable slice with its
// own gate semantics and is deliberately not in this implementation.
//
// 2D table layout: the timing table is indexed (load, RPM) — load on
// the outer axis (rows), RPM on the inner axis (cols). Flat row-major:
//   timing[row * rpm_axis.size() + col]
// where row = nearest_cell(load, load_axis), col = nearest_cell(rpm,
// rpm_axis). The result reports cells in the same flat order.

struct KnockSample {
    double rpm{0.0};
    double load{0.0};
    // Signed degrees. Negative = ECU is pulling timing. The trigger
    // looks for mean < -opts.trigger_degrees, so a sustained -1.6
    // average with trigger_degrees=1.5 fires a pull.
    double feedback_knock{0.0};
    double coolant_c{0.0};
    double iat_c{0.0};
    bool   limp_mode{false};
};

struct KnockPullOptions {
    double      trigger_degrees      = 1.5;   // mean below -trigger fires
    double      pull_step_degrees    = 0.75;  // subtracted from cell on fire
    std::size_t min_samples_per_cell = 30;

    // Data-quality gates.
    double      min_coolant_c       = 80.0;
    double      min_iat_c           = -20.0;
    double      max_iat_c           = 80.0;
    bool        reject_limp_mode    = true;
};

struct KnockCellProposal {
    std::size_t cell_index{0};     // flat row-major index
    double      current_value{0.0};
    double      proposed_value{0.0};
    double      mean_feedback_knock{0.0};
    std::size_t samples_used{0};
    bool        pulled{false};
};

struct KnockPullResult {
    std::vector<KnockCellProposal> cells;
    std::size_t                    rows{0};   // = load_axis.size()
    std::size_t                    cols{0};   // = rpm_axis.size()
    std::size_t                    total_samples{0};
    std::size_t                    samples_after_gates{0};
};

[[nodiscard]] bool knock_sample_passes_gates(
    KnockSample const     &s,
    KnockPullOptions const &opts) noexcept;

// Locate (row, col) for a given (rpm, load) pair. Returns flat index
// row * rpm_axis.size() + col. Either axis empty returns 0.
[[nodiscard]] std::size_t nearest_cell_2d(
    double                  rpm,
    double                  load,
    std::span<double const> rpm_axis,
    std::span<double const> load_axis) noexcept;

// The canonical knock-pull entry point. `current_timing` is a flat
// row-major vector of size `rpm_axis.size() * load_axis.size()`.
// Cells with fewer than `opts.min_samples_per_cell` retained samples,
// or whose mean feedback knock isn't worse than `-trigger_degrees`,
// stay at their current_value with `pulled=false`. Triggered cells get
// `current_value - pull_step_degrees`.
[[nodiscard]] Result<KnockPullResult> tune_knock_pull(
    std::span<double const>      rpm_axis,
    std::span<double const>      load_axis,
    std::span<double const>      current_timing,
    std::span<KnockSample const> samples,
    KnockPullOptions const      &opts = {});

// =====================================================================
// CSV → MafSample reader
// =====================================================================
//
// Parses a column-headered CSV log into MafSample records. The header
// row identifies columns by name (case-insensitive); columns may
// appear in any order; unknown columns are ignored.
//
// Required columns:
//   maf_voltage, actual_afr, commanded_afr, rpm, throttle_pct,
//   coolant_c, iat_c
//
// Optional columns:
//   time_ms       — milliseconds since the log start. When missing,
//                   rows are assumed at 50 ms apart (20 Hz).
//   closed_loop   — boolean (0/1, true/false, yes/no, on/off). An empty
//                   cell parses as false; explicit non-boolean tokens
//                   error with row + column flagged.
//   knock         — boolean knock-event flag for this sample. Used to
//                   derive `knock_in_window`, which is set true on any
//                   sample that has another sample with knock=1 within
//                   the prior 250 ms (per docs/12 §"Data-quality gates").
//                   Empty cells parse as false; same error semantics as
//                   `closed_loop` for non-boolean text.
//   limp_mode     — boolean. Empty-cell and error semantics match
//                   `closed_loop`.
//
// Derived per-sample fields:
//   rpm_rate, throttle_rate — forward differences over the time delta
//     between successive rows; the first row's rates are 0.
//   knock_in_window — see above.
//
// Lines beginning with '#' and blank lines are skipped. CRLF tolerated.
// Empty input returns an empty vector (not an error). Missing required
// column → InvalidArgument. Non-numeric value in a numeric column or
// non-boolean value in a boolean column → InvalidArgument with the
// row/column flagged.
[[nodiscard]] Result<std::vector<MafSample>> read_maf_samples_csv(
    std::string_view text);

// =====================================================================
// CSV → KnockSample reader
// =====================================================================
//
// Mirror of `read_maf_samples_csv` for the knock-pull algorithm.
//
// Required columns:
//   rpm, load, feedback_knock, coolant_c, iat_c
//
// Optional columns:
//   limp_mode — boolean (0/1, true/false, yes/no, on/off). Empty cells
//               parse as false; explicit non-boolean tokens error.
//
// `feedback_knock` is signed degrees — negative means the ECU is
// pulling timing — and the algorithm's trigger checks for mean <
// `-opts.trigger_degrees`. Unlike the MAF reader, no time-derived
// fields are computed (the pull is steady-state per cell per
// docs/12, not rate-of-change driven); a `time_ms` column, if
// present, is silently ignored. Blank lines and '#' comments
// tolerated.
[[nodiscard]] Result<std::vector<KnockSample>> read_knock_samples_csv(
    std::string_view text);

} // namespace st::autotune

#endif // ST_AUTOTUNE_HPP
