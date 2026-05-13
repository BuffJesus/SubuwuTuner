// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

// ---------------------------------------------------------------------
// Knock-based ignition pull
// ---------------------------------------------------------------------

bool knock_sample_passes_gates(KnockSample const     &s,
                                KnockPullOptions const &opts) noexcept {
    if (!std::isfinite(s.feedback_knock) || !std::isfinite(s.rpm)
        || !std::isfinite(s.load)) {
        return false;
    }
    if (s.rpm < 0.0 || s.load < 0.0) return false;
    if (s.coolant_c < opts.min_coolant_c)                   return false;
    if (s.iat_c < opts.min_iat_c || s.iat_c > opts.max_iat_c) return false;
    if (opts.reject_limp_mode && s.limp_mode)               return false;
    return true;
}

std::size_t nearest_cell_2d(double                  rpm,
                             double                  load,
                             std::span<double const> rpm_axis,
                             std::span<double const> load_axis) noexcept {
    if (rpm_axis.empty() || load_axis.empty()) return 0;
    auto const col = nearest_cell(rpm, rpm_axis);
    auto const row = nearest_cell(load, load_axis);
    return row * rpm_axis.size() + col;
}

Result<KnockPullResult> tune_knock_pull(
    std::span<double const>      rpm_axis,
    std::span<double const>      load_axis,
    std::span<double const>      current_timing,
    std::span<KnockSample const> samples,
    KnockPullOptions const      &opts) {
    if (rpm_axis.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: RPM axis must be non-empty");
    }
    if (load_axis.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: load axis must be non-empty");
    }
    std::size_t const expected = rpm_axis.size() * load_axis.size();
    if (current_timing.size() != expected) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: current_timing length ("
                       + std::to_string(current_timing.size())
                       + ") must equal rpm_axis.size() * load_axis.size() ("
                       + std::to_string(expected) + ")");
    }
    if (opts.pull_step_degrees < 0.0
        || !std::isfinite(opts.pull_step_degrees)) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: pull_step_degrees must be non-negative");
    }
    if (opts.trigger_degrees < 0.0
        || !std::isfinite(opts.trigger_degrees)) {
        return failure(ErrorCode::InvalidArgument,
                       "autotune: trigger_degrees must be non-negative");
    }
    for (std::size_t i = 1; i < rpm_axis.size(); ++i) {
        if (!(rpm_axis[i - 1] <= rpm_axis[i])) {
            return failure(ErrorCode::InvalidArgument,
                           "autotune: RPM axis must be sorted ascending");
        }
    }
    for (std::size_t i = 1; i < load_axis.size(); ++i) {
        if (!(load_axis[i - 1] <= load_axis[i])) {
            return failure(ErrorCode::InvalidArgument,
                           "autotune: load axis must be sorted ascending");
        }
    }

    std::vector<std::vector<double>> per_cell(expected);
    std::size_t                      kept = 0;
    for (auto const &s : samples) {
        if (!knock_sample_passes_gates(s, opts)) continue;
        auto const idx = nearest_cell_2d(s.rpm, s.load, rpm_axis, load_axis);
        per_cell[idx].push_back(s.feedback_knock);
        ++kept;
    }

    KnockPullResult result;
    result.rows                = load_axis.size();
    result.cols                = rpm_axis.size();
    result.total_samples       = samples.size();
    result.samples_after_gates = kept;
    result.cells.reserve(expected);

    for (std::size_t i = 0; i < expected; ++i) {
        KnockCellProposal kp;
        kp.cell_index    = i;
        kp.current_value = current_timing[i];
        kp.samples_used  = per_cell[i].size();

        if (per_cell[i].size() < opts.min_samples_per_cell) {
            kp.proposed_value      = current_timing[i];
            kp.mean_feedback_knock = 0.0;
            kp.pulled              = false;
            result.cells.push_back(kp);
            continue;
        }

        double sum = 0.0;
        for (auto v : per_cell[i]) sum += v;
        double const mean =
            sum / static_cast<double>(per_cell[i].size());
        kp.mean_feedback_knock = mean;

        if (mean < -opts.trigger_degrees) {
            kp.proposed_value = current_timing[i] - opts.pull_step_degrees;
            kp.pulled         = true;
        } else {
            kp.proposed_value = current_timing[i];
            kp.pulled         = false;
        }
        result.cells.push_back(kp);
    }

    return result;
}

// ---------------------------------------------------------------------
// CSV → MafSample reader
// ---------------------------------------------------------------------

namespace {

std::string_view trim_view(std::string_view s) noexcept {
    while (!s.empty()
           && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty()
           && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string to_lower_str(std::string_view s) {
    std::string out(s);
    for (auto &c : out) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::vector<std::string_view> split_csv_fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t                   start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::optional<double> parse_double_field(std::string_view s) {
    auto const t = trim_view(s);
    if (t.empty()) {
        return std::nullopt;
    }
    std::string copy(t);
    char       *end = nullptr;
    double const v  = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    if (!std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

std::optional<bool> parse_bool_field(std::string_view s) {
    auto const lower = to_lower_str(trim_view(s));
    if (lower.empty()) {
        return false;
    }
    if (lower == "1" || lower == "true" || lower == "yes"
        || lower == "on") {
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no"
        || lower == "off") {
        return false;
    }
    return std::nullopt;
}

// Slice text into trimmed non-blank, non-comment lines. Shared by
// every CSV reader in this file.
std::vector<std::string_view> non_blank_lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t                   i = 0;
    while (i < text.size()) {
        std::size_t e = i;
        while (e < text.size() && text[e] != '\n' && text[e] != '\r') {
            ++e;
        }
        auto const line = trim_view(text.substr(i, e - i));
        if (!line.empty() && line.front() != '#') {
            out.push_back(line);
        }
        i = e;
        while (i < text.size() && (text[i] == '\n' || text[i] == '\r')) {
            ++i;
        }
    }
    return out;
}

} // namespace

Result<std::vector<MafSample>> read_maf_samples_csv(std::string_view text) {
    auto const lines = non_blank_lines(text);
    if (lines.empty()) {
        return std::vector<MafSample>{};
    }

    // Build column-name → index map from the header row.
    auto const                         header = split_csv_fields(lines[0]);
    std::map<std::string, std::size_t> col_idx;
    for (std::size_t i = 0; i < header.size(); ++i) {
        col_idx.emplace(to_lower_str(trim_view(header[i])), i);
    }

    auto require_col = [&](char const *name) -> Result<std::size_t> {
        auto it = col_idx.find(name);
        if (it == col_idx.end()) {
            return failure(ErrorCode::InvalidArgument,
                           std::string{"autotune: CSV missing required column '"}
                               + name + "'");
        }
        return it->second;
    };
    auto find_opt_col = [&](char const *name) -> std::optional<std::size_t> {
        auto it = col_idx.find(name);
        return (it == col_idx.end())
                   ? std::optional<std::size_t>{}
                   : std::optional<std::size_t>{it->second};
    };

    auto const idx_maf = require_col("maf_voltage");
    if (!idx_maf.has_value()) {
        return failure(std::move(idx_maf).error());
    }
    auto const idx_act = require_col("actual_afr");
    if (!idx_act.has_value()) {
        return failure(std::move(idx_act).error());
    }
    auto const idx_cmd = require_col("commanded_afr");
    if (!idx_cmd.has_value()) {
        return failure(std::move(idx_cmd).error());
    }
    auto const idx_rpm = require_col("rpm");
    if (!idx_rpm.has_value()) {
        return failure(std::move(idx_rpm).error());
    }
    auto const idx_tps = require_col("throttle_pct");
    if (!idx_tps.has_value()) {
        return failure(std::move(idx_tps).error());
    }
    auto const idx_clt = require_col("coolant_c");
    if (!idx_clt.has_value()) {
        return failure(std::move(idx_clt).error());
    }
    auto const idx_iat = require_col("iat_c");
    if (!idx_iat.has_value()) {
        return failure(std::move(idx_iat).error());
    }

    auto const idx_time = find_opt_col("time_ms");
    auto const idx_cl   = find_opt_col("closed_loop");
    auto const idx_kn   = find_opt_col("knock");
    auto const idx_lm   = find_opt_col("limp_mode");

    constexpr double kDefaultPeriodMs  = 50.0;
    constexpr double kKnockWindowMs    = 250.0;

    std::vector<MafSample> samples;
    std::vector<double>    time_ms;
    std::vector<bool>      knock_flag;
    if (lines.size() > 1) {
        samples.reserve(lines.size() - 1);
        time_ms.reserve(lines.size() - 1);
        knock_flag.reserve(lines.size() - 1);
    }

    auto row_error = [](std::size_t row, char const *col,
                         char const *what, std::string_view raw) {
        return failure(
            ErrorCode::InvalidArgument,
            std::string{"autotune: CSV row "} + std::to_string(row + 1)
                + " column '" + col + "' has " + what + " value '"
                + std::string{trim_view(raw)} + "'");
    };

    for (std::size_t row = 1; row < lines.size(); ++row) {
        auto const fields = split_csv_fields(lines[row]);

        auto get_double = [&](std::size_t i,
                              char const *col) -> Result<double> {
            if (i >= fields.size()) {
                return failure(
                    ErrorCode::InvalidArgument,
                    std::string{"autotune: CSV row "} + std::to_string(row + 1)
                        + " is missing field for column '" + col + "'");
            }
            auto v = parse_double_field(fields[i]);
            if (!v.has_value()) {
                return row_error(row, col, "non-numeric", fields[i]);
            }
            return *v;
        };

        MafSample s;
        if (auto r = get_double(*idx_maf, "maf_voltage"); r.has_value()) {
            s.maf_voltage = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_act, "actual_afr"); r.has_value()) {
            s.actual_afr = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_cmd, "commanded_afr"); r.has_value()) {
            s.commanded_afr = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_rpm, "rpm"); r.has_value()) {
            s.rpm = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_tps, "throttle_pct"); r.has_value()) {
            s.throttle_pct = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_clt, "coolant_c"); r.has_value()) {
            s.coolant_c = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_iat, "iat_c"); r.has_value()) {
            s.iat_c = *r;
        } else {
            return failure(std::move(r).error());
        }

        double t_ms = static_cast<double>(samples.size()) * kDefaultPeriodMs;
        if (idx_time.has_value()) {
            if (auto r = get_double(*idx_time, "time_ms"); r.has_value()) {
                t_ms = *r;
            } else {
                return failure(std::move(r).error());
            }
        }
        time_ms.push_back(t_ms);

        bool kf = false;
        if (idx_kn.has_value() && *idx_kn < fields.size()) {
            auto b = parse_bool_field(fields[*idx_kn]);
            if (!b.has_value()) {
                return row_error(row, "knock", "non-boolean",
                                  fields[*idx_kn]);
            }
            kf = *b;
        }
        knock_flag.push_back(kf);

        if (idx_cl.has_value() && *idx_cl < fields.size()) {
            auto b = parse_bool_field(fields[*idx_cl]);
            if (!b.has_value()) {
                return row_error(row, "closed_loop", "non-boolean",
                                  fields[*idx_cl]);
            }
            s.closed_loop = *b;
        }
        if (idx_lm.has_value() && *idx_lm < fields.size()) {
            auto b = parse_bool_field(fields[*idx_lm]);
            if (!b.has_value()) {
                return row_error(row, "limp_mode", "non-boolean",
                                  fields[*idx_lm]);
            }
            s.limp_mode = *b;
        }

        samples.push_back(s);
    }

    // Derive rpm_rate / throttle_rate using forward differences over
    // the row-to-row time delta. Negative or zero dt (out-of-order or
    // duplicate timestamps) collapses to zero rate — better than
    // propagating a meaningless spike to the gate.
    for (std::size_t i = 1; i < samples.size(); ++i) {
        double const dt_s = (time_ms[i] - time_ms[i - 1]) / 1000.0;
        if (dt_s <= 0.0 || !std::isfinite(dt_s)) {
            samples[i].rpm_rate      = 0.0;
            samples[i].throttle_rate = 0.0;
            continue;
        }
        samples[i].rpm_rate =
            (samples[i].rpm - samples[i - 1].rpm) / dt_s;
        samples[i].throttle_rate =
            (samples[i].throttle_pct - samples[i - 1].throttle_pct) / dt_s;
    }

    // Derive knock_in_window from the row-level `knock` flag using a
    // backward 250 ms window inclusive of the current sample. Maintains
    // a running count of knock-flagged samples inside the window — the
    // outer iterator advances right, the inner `lo` advances right when
    // entries fall off the back, so each sample is touched twice and
    // the whole pass is genuinely O(n) regardless of window depth.
    std::size_t lo               = 0;
    std::size_t knocks_in_window = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (knock_flag[i]) {
            ++knocks_in_window;
        }
        while (lo < i && time_ms[i] - time_ms[lo] > kKnockWindowMs) {
            if (knock_flag[lo]) {
                --knocks_in_window;
            }
            ++lo;
        }
        samples[i].knock_in_window = (knocks_in_window > 0);
    }

    return samples;
}

// ---------------------------------------------------------------------
// CSV → KnockSample reader
// ---------------------------------------------------------------------

Result<std::vector<KnockSample>> read_knock_samples_csv(
    std::string_view text) {
    auto const lines = non_blank_lines(text);
    if (lines.empty()) {
        return std::vector<KnockSample>{};
    }

    auto const                         header = split_csv_fields(lines[0]);
    std::map<std::string, std::size_t> col_idx;
    for (std::size_t i = 0; i < header.size(); ++i) {
        col_idx.emplace(to_lower_str(trim_view(header[i])), i);
    }

    auto require_col = [&](char const *name) -> Result<std::size_t> {
        auto it = col_idx.find(name);
        if (it == col_idx.end()) {
            return failure(ErrorCode::InvalidArgument,
                           std::string{"autotune: CSV missing required column '"}
                               + name + "'");
        }
        return it->second;
    };
    auto find_opt_col = [&](char const *name) -> std::optional<std::size_t> {
        auto it = col_idx.find(name);
        return (it == col_idx.end())
                   ? std::optional<std::size_t>{}
                   : std::optional<std::size_t>{it->second};
    };

    auto const idx_rpm = require_col("rpm");
    if (!idx_rpm.has_value()) {
        return failure(std::move(idx_rpm).error());
    }
    auto const idx_load = require_col("load");
    if (!idx_load.has_value()) {
        return failure(std::move(idx_load).error());
    }
    auto const idx_fk = require_col("feedback_knock");
    if (!idx_fk.has_value()) {
        return failure(std::move(idx_fk).error());
    }
    auto const idx_clt = require_col("coolant_c");
    if (!idx_clt.has_value()) {
        return failure(std::move(idx_clt).error());
    }
    auto const idx_iat = require_col("iat_c");
    if (!idx_iat.has_value()) {
        return failure(std::move(idx_iat).error());
    }
    auto const idx_lm = find_opt_col("limp_mode");

    std::vector<KnockSample> samples;
    if (lines.size() > 1) {
        samples.reserve(lines.size() - 1);
    }

    auto row_error = [](std::size_t row, char const *col,
                         char const *what, std::string_view raw) {
        return failure(
            ErrorCode::InvalidArgument,
            std::string{"autotune: CSV row "} + std::to_string(row + 1)
                + " column '" + col + "' has " + what + " value '"
                + std::string{trim_view(raw)} + "'");
    };

    for (std::size_t row = 1; row < lines.size(); ++row) {
        auto const fields = split_csv_fields(lines[row]);

        auto get_double = [&](std::size_t i,
                              char const *col) -> Result<double> {
            if (i >= fields.size()) {
                return failure(
                    ErrorCode::InvalidArgument,
                    std::string{"autotune: CSV row "} + std::to_string(row + 1)
                        + " is missing field for column '" + col + "'");
            }
            auto v = parse_double_field(fields[i]);
            if (!v.has_value()) {
                return row_error(row, col, "non-numeric", fields[i]);
            }
            return *v;
        };

        KnockSample s;
        if (auto r = get_double(*idx_rpm, "rpm"); r.has_value()) {
            s.rpm = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_load, "load"); r.has_value()) {
            s.load = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_fk, "feedback_knock"); r.has_value()) {
            s.feedback_knock = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_clt, "coolant_c"); r.has_value()) {
            s.coolant_c = *r;
        } else {
            return failure(std::move(r).error());
        }
        if (auto r = get_double(*idx_iat, "iat_c"); r.has_value()) {
            s.iat_c = *r;
        } else {
            return failure(std::move(r).error());
        }

        if (idx_lm.has_value() && *idx_lm < fields.size()) {
            auto b = parse_bool_field(fields[*idx_lm]);
            if (!b.has_value()) {
                return row_error(row, "limp_mode", "non-boolean",
                                  fields[*idx_lm]);
            }
            s.limp_mode = *b;
        }

        samples.push_back(s);
    }

    return samples;
}

} // namespace st::autotune
