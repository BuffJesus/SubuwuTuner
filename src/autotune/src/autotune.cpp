// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace st::autotune {

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

bool sample_passes_gates(MafSample const     &s,
                         MafTuneOptions const &opts) noexcept {
    // Reject anything that would make the ratio degenerate first —
    // those samples are useless to the algorithm regardless of the
    // engine-state gates below.
    if (!std::isfinite(s.actual_afr) || !std::isfinite(s.commanded_afr)) {
        return false;
    }
    if (s.actual_afr <= 0.0 || s.commanded_afr <= 0.0) {
        return false;
    }
    if (!std::isfinite(s.maf_voltage) || s.maf_voltage < 0.0) {
        return false;
    }

    // Engine-state gates.
    if (s.coolant_c < opts.min_coolant_c)                   return false;
    if (s.iat_c < opts.min_iat_c || s.iat_c > opts.max_iat_c) return false;
    if (std::fabs(s.rpm_rate) > opts.max_rpm_rate)          return false;
    if (std::fabs(s.throttle_rate) > opts.max_throttle_rate) return false;
    if (opts.reject_knock_window && s.knock_in_window)      return false;
    if (opts.reject_limp_mode && s.limp_mode)               return false;
    if (opts.require_open_loop && s.closed_loop)            return false;

    return true;
}

std::size_t nearest_cell(double                  voltage,
                         std::span<double const> axis) noexcept {
    if (axis.empty()) return 0;
    std::size_t best  = 0;
    double      best_d = std::fabs(axis[0] - voltage);
    for (std::size_t i = 1; i < axis.size(); ++i) {
        double const d = std::fabs(axis[i] - voltage);
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    return best;
}

double trimmed_mean(std::span<double const> values, double trim_fraction) {
    if (values.empty()) return 0.0;
    if (trim_fraction < 0.0) trim_fraction = 0.0;
    if (trim_fraction > 0.5) trim_fraction = 0.5;

    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());

    std::size_t const n    = sorted.size();
    std::size_t const drop = static_cast<std::size_t>(
        std::floor(static_cast<double>(n) * trim_fraction));
    // Always retain at least one value: with trim=0.10 on n=5 we'd
    // drop 0 from each side; with n=10 we'd drop 1 from each side.
    std::size_t const begin = drop;
    std::size_t const end   = n > drop ? n - drop : drop;
    if (end <= begin) {
        // Should not happen given the clamp above; defensive bail.
        double sum = 0.0;
        for (auto v : sorted) sum += v;
        return sum / static_cast<double>(n);
    }
    double      sum     = 0.0;
    std::size_t counted = 0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += sorted[i];
        ++counted;
    }
    return sum / static_cast<double>(counted);
}

namespace {

// Confidence ramps linearly from 0 at min_samples to 1.0 at
// 3 × min_samples, then stays at 1.0. Simple, monotonic, and easy to
// explain in CLI output ("47 samples, confidence 0.31").
double confidence_score(std::size_t samples,
                        std::size_t min_samples) noexcept {
    if (min_samples == 0) return 1.0;
    if (samples < min_samples) return 0.0;
    double const ratio = static_cast<double>(samples)
                         / static_cast<double>(3 * min_samples);
    return std::min(1.0, ratio);
}

} // namespace

// ---------------------------------------------------------------------
// tune_maf
// ---------------------------------------------------------------------

Result<MafTuneResult> tune_maf(std::span<double const>    axis,
                                std::span<double const>    current_scaling,
                                std::span<MafSample const> samples,
                                MafTuneOptions const      &opts) {
    if (axis.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: MAF axis must be non-empty");
    }
    if (axis.size() != current_scaling.size()) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: axis (" + std::to_string(axis.size())
                       + ") and current_scaling ("
                       + std::to_string(current_scaling.size())
                       + ") must have the same length");
    }
    if (opts.gain < 0.0 || !std::isfinite(opts.gain)) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: gain must be a non-negative finite number");
    }
    if (opts.max_delta_pct < 0.0 || !std::isfinite(opts.max_delta_pct)) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: max_delta_pct must be non-negative");
    }
    // Verify the axis is sorted ascending; the nearest-cell routine
    // does not require it, but downstream consumers (linter,
    // smoothing) assume axis order.
    for (std::size_t i = 1; i < axis.size(); ++i) {
        if (!(axis[i - 1] <= axis[i])) {
            return failure(ErrorCode::InvalidArgument,
                           "autotune: MAF axis must be sorted ascending; "
                           "index " + std::to_string(i)
                           + " breaks the order");
        }
    }

    // Bucket samples to nearest cell, recording the (actual/commanded)
    // error ratio. Each cell collects its samples in a vector for the
    // trimmed-mean pass.
    std::vector<std::vector<double>> per_cell(axis.size());
    std::size_t                      kept = 0;
    for (auto const &s : samples) {
        if (!sample_passes_gates(s, opts)) continue;
        auto const cell = nearest_cell(s.maf_voltage, axis);
        per_cell[cell].push_back(s.actual_afr / s.commanded_afr);
        ++kept;
    }

    MafTuneResult result;
    result.total_samples       = samples.size();
    result.samples_after_gates = kept;
    result.cells.reserve(axis.size());

    for (std::size_t i = 0; i < axis.size(); ++i) {
        CellProposal cp;
        cp.cell_index    = i;
        cp.current_value = current_scaling[i];
        cp.samples_used  = per_cell[i].size();

        if (per_cell[i].size() < opts.min_samples_per_cell) {
            cp.mean_error     = 1.0;
            cp.proposed_value = current_scaling[i];
            cp.confidence     = 0.0;
            result.cells.push_back(cp);
            continue;
        }

        double const mean_error =
            trimmed_mean(per_cell[i], opts.trim_fraction);
        double delta_pct = (mean_error - 1.0) * opts.gain;
        if (delta_pct > opts.max_delta_pct)  delta_pct = opts.max_delta_pct;
        if (delta_pct < -opts.max_delta_pct) delta_pct = -opts.max_delta_pct;

        cp.mean_error     = mean_error;
        cp.proposed_value = current_scaling[i] * (1.0 + delta_pct);
        cp.confidence     =
            confidence_score(per_cell[i].size(), opts.min_samples_per_cell);
        result.cells.push_back(cp);
    }

    return result;
}

// ---------------------------------------------------------------------
// smooth_proposals
// ---------------------------------------------------------------------

MafTuneResult smooth_proposals(MafTuneResult const &input,
                                double               max_delta_pct,
                                double               neighbor_weight) {
    MafTuneResult out;
    out.total_samples       = input.total_samples;
    out.samples_after_gates = input.samples_after_gates;
    out.cells               = input.cells;
    if (input.cells.size() <= 1) return out;
    if (max_delta_pct < 0.0)     max_delta_pct = 0.0;
    if (neighbor_weight < 0.0)   neighbor_weight = 0.0;

    auto const &in = input.cells;
    std::size_t const n = in.size();

    for (std::size_t i = 0; i < n; ++i) {
        double const self_w   = in[i].confidence;
        double       num      = self_w * in[i].proposed_value;
        double       den      = self_w;

        if (i > 0) {
            double const w = neighbor_weight * in[i - 1].confidence;
            num += w * in[i - 1].proposed_value;
            den += w;
        }
        if (i + 1 < n) {
            double const w = neighbor_weight * in[i + 1].confidence;
            num += w * in[i + 1].proposed_value;
            den += w;
        }

        double smoothed;
        if (den <= 0.0) {
            // No information here or in neighbors — leave unchanged.
            smoothed = in[i].proposed_value;
        } else {
            smoothed = num / den;
        }

        // Re-clamp to ±max_delta_pct of current_value so neighbor pull
        // can't break the per-pass safety bound the first pass enforced.
        double const cur     = in[i].current_value;
        double const lo      = cur * (1.0 - max_delta_pct);
        double const hi      = cur * (1.0 + max_delta_pct);
        if (smoothed < lo) smoothed = lo;
        if (smoothed > hi) smoothed = hi;

        out.cells[i].proposed_value = smoothed;
    }

    return out;
}

} // namespace st::autotune
