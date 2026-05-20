// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/coldstart.hpp"

#include "st/core/csv.hpp"
#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace st::log::coldstart {

namespace {

inline bool has_col(std::size_t idx) noexcept {
    return idx != kNoColumn;
}

inline double to_seconds(double raw, TimestampUnit unit) noexcept {
    switch (unit) {
        case TimestampUnit::UnixSeconds: return raw;
        case TimestampUnit::UnixMillis:  return raw / 1'000.0;
        case TimestampUnit::UnixMicros:  return raw / 1'000'000.0;
        case TimestampUnit::RowIndex:    return raw;
    }
    return raw;
}

// Max sample-to-sample delta we'll attribute to a single phase. Larger
// gaps almost always mean the log paused / reconnected / skipped
// frames; counting that delta as "engine spent N s in this phase"
// would lie. 5 s is generous for any normal Subaru logger cadence.
inline constexpr double kMaxPhaseDeltaSeconds = 5.0;

TestPhase classify_phase(double rpm, double ect_c,
                         WindowConfig const &cfg) noexcept {
    if (ect_c >= cfg.cold_threshold_c) {
        return TestPhase::ClosedLoop;
    }
    if (rpm <= 0.0) {
        return TestPhase::PreCrank;
    }
    if (rpm < cfg.cranking_max_rpm) {
        return TestPhase::Cranking;
    }
    if (rpm < cfg.initial_firing_max_rpm) {
        return TestPhase::InitialFiring;
    }
    if (rpm < cfg.high_idle_max_rpm) {
        return TestPhase::HighIdle;
    }
    return TestPhase::Warmup;
}

struct EctAcc {
    double         sum_obs{0.0};
    double         min_obs{ std::numeric_limits<double>::infinity()};
    double         max_obs{-std::numeric_limits<double>::infinity()};
    double         sum_cmd{0.0};
    std::uint32_t  count{0};
    std::uint32_t  count_with_cmd{0};
};

std::size_t max_referenced_column(PidMapping const &m) noexcept {
    std::size_t lo = 0;
    auto const  consider = [&](std::size_t v) {
        if (v != kNoColumn && v + 1 > lo) lo = v + 1;
    };
    consider(m.timestamp_idx);
    consider(m.ect_idx);
    consider(m.iat_idx);
    consider(m.rpm_idx);
    consider(m.observed_lambda_idx);
    consider(m.commanded_lambda_idx);
    consider(m.timing_deg_idx);
    consider(m.closed_loop_idx);
    return lo;
}

}  // namespace

double TargetLambdaCurve::sample(double ect_c) const noexcept {
    if (points.empty()) {
        return 1.0;  // stoichiometric fallback
    }
    // Clamp at endpoints; assume points are sorted ascending by ect_c.
    if (ect_c <= points.front().first)  return points.front().second;
    if (ect_c >= points.back().first)   return points.back().second;
    for (std::size_t i = 1; i < points.size(); ++i) {
        auto const &p0 = points[i - 1];
        auto const &p1 = points[i];
        if (ect_c <= p1.first) {
            double const span = p1.first - p0.first;
            if (span <= 0.0) return p0.second;
            double const t = (ect_c - p0.first) / span;
            return p0.second + t * (p1.second - p0.second);
        }
    }
    return points.back().second;
}

ColdStartSnapshot snapshot_from_samples(std::span<double const>   samples_row_major,
                                       std::size_t               pid_count,
                                       PidMapping const         &mapping,
                                       WindowConfig const       &cfg,
                                       Methodology const        &methodology) {
    ColdStartSnapshot snap{};

    if (pid_count == 0
        || samples_row_major.empty()
        || !has_col(mapping.timestamp_idx)
        || !has_col(mapping.ect_idx)
        || !has_col(mapping.rpm_idx)
        || cfg.ect_bin_width_c <= 0.0) {
        return snap;
    }

    std::size_t const n_rows = samples_row_major.size() / pid_count;
    if (n_rows == 0) {
        return snap;
    }

    snap.samples.reserve(n_rows);

    std::map<std::int64_t, EctAcc> ect_buckets;

    double    prev_ts_s   = 0.0;
    TestPhase prev_phase  = TestPhase::PreCrank;
    bool      has_prev    = false;

    double coldest = std::numeric_limits<double>::infinity();
    double warmest = -std::numeric_limits<double>::infinity();

    for (std::size_t row = 0; row < n_rows; ++row) {
        std::size_t const off = row * pid_count;

        double const ts_raw = samples_row_major[off + mapping.timestamp_idx];
        double const ect_c  = samples_row_major[off + mapping.ect_idx];
        double const rpm    = samples_row_major[off + mapping.rpm_idx];

        if (!std::isfinite(ts_raw)
            || !std::isfinite(ect_c)
            || !std::isfinite(rpm)) {
            snap.samples_gated_out++;
            continue;
        }

        double const ts_s = to_seconds(ts_raw, cfg.timestamp_unit);
        TestPhase const phase = classify_phase(rpm, ect_c, cfg);

        snap.phase_counts[static_cast<std::size_t>(phase)]++;

        // Phase duration accounting via timestamp deltas. The delta gets
        // attributed to the *previous* sample's phase (the time spent
        // sitting in that phase before transitioning to the current
        // sample). Bounded to drop log-pause / disconnect artifacts.
        if (has_prev) {
            double const dt = ts_s - prev_ts_s;
            if (dt > 0.0 && dt < kMaxPhaseDeltaSeconds) {
                snap.phase_seconds[static_cast<std::size_t>(prev_phase)] += dt;
            }
        }
        prev_ts_s  = ts_s;
        prev_phase = phase;
        has_prev   = true;

        snap.samples_considered++;

        PhaseSample ps{};
        ps.phase     = phase;
        ps.timestamp = ts_s;
        ps.ect_c     = ect_c;
        ps.rpm       = rpm;
        if (has_col(mapping.iat_idx)) {
            ps.iat_c = samples_row_major[off + mapping.iat_idx];
        }
        if (has_col(mapping.observed_lambda_idx)) {
            ps.observed_lambda =
                samples_row_major[off + mapping.observed_lambda_idx];
        }
        if (has_col(mapping.commanded_lambda_idx)) {
            ps.commanded_lambda =
                samples_row_major[off + mapping.commanded_lambda_idx];
        }
        if (has_col(mapping.timing_deg_idx)) {
            ps.timing_deg = samples_row_major[off + mapping.timing_deg_idx];
        }
        if (has_col(mapping.closed_loop_idx)) {
            double const cl =
                samples_row_major[off + mapping.closed_loop_idx];
            ps.closed_loop = (cl > 0.5);
        }
        snap.samples.push_back(ps);

        if (ect_c < coldest) coldest = ect_c;
        if (ect_c > warmest) warmest = ect_c;

        // Per-ECT-bin lambda accumulator. Only buckets samples that are
        // in cold regime AND have observed-lambda data.
        if (ect_c < cfg.cold_threshold_c
            && has_col(mapping.observed_lambda_idx)
            && std::isfinite(ps.observed_lambda)) {
            std::int64_t const bidx = static_cast<std::int64_t>(
                std::floor(ect_c / cfg.ect_bin_width_c));
            auto &acc = ect_buckets[bidx];
            acc.sum_obs += ps.observed_lambda;
            if (ps.observed_lambda < acc.min_obs) acc.min_obs = ps.observed_lambda;
            if (ps.observed_lambda > acc.max_obs) acc.max_obs = ps.observed_lambda;
            if (has_col(mapping.commanded_lambda_idx)
                && std::isfinite(ps.commanded_lambda)) {
                acc.sum_cmd += ps.commanded_lambda;
                acc.count_with_cmd++;
            }
            acc.count++;
        }
    }

    // Reduce ECT bins → EctBin records + compute mean_lambda_deviation.
    double         total_dev       = 0.0;
    std::uint32_t  total_dev_count = 0;

    snap.ect_bins.reserve(ect_buckets.size());
    for (auto const &[bidx, acc] : ect_buckets) {
        if (acc.count < cfg.min_samples_per_bin) continue;
        EctBin bin{};
        bin.ect_center_c = (static_cast<double>(bidx) + 0.5)
                            * cfg.ect_bin_width_c;
        bin.observed_lambda_mean = acc.sum_obs
                                    / static_cast<double>(acc.count);
        bin.observed_lambda_min  = acc.min_obs;
        bin.observed_lambda_max  = acc.max_obs;
        if (acc.count_with_cmd > 0) {
            bin.commanded_lambda_mean = acc.sum_cmd
                / static_cast<double>(acc.count_with_cmd);
        }
        bin.count       = acc.count;
        bin.has_lambda  = true;
        double const target = methodology.target_lambda_vs_ect.sample(
            bin.ect_center_c);
        bin.deviation_from_target = std::abs(
            bin.observed_lambda_mean - target);
        total_dev       += bin.deviation_from_target;
        total_dev_count += 1;
        snap.ect_bins.push_back(bin);
    }
    if (total_dev_count > 0) {
        snap.mean_lambda_deviation = total_dev
            / static_cast<double>(total_dev_count);
    }

    if (coldest != std::numeric_limits<double>::infinity()) {
        snap.coldest_ect_c = coldest;
        snap.warmest_ect_c = warmest;
    }

    return snap;
}

st::Result<ColdStartSnapshot> snapshot_from_csv(std::string_view          csv_path,
                                                PidMapping const         &mapping,
                                                WindowConfig const       &cfg,
                                                Methodology const        &methodology) {
    if (!has_col(mapping.timestamp_idx)
        || !has_col(mapping.ect_idx)
        || !has_col(mapping.rpm_idx)) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"coldstart: mapping requires "
                                       "timestamp, ECT, and RPM columns"});
    }

    std::ifstream f{std::string{csv_path}};
    if (!f) {
        return st::failure(st::ErrorCode::FileNotFound,
                           std::string{"coldstart: cannot open '"}
                               + std::string{csv_path} + "'");
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string const text = std::move(buf).str();

    auto const lines = st::core::csv::split_lines(text);
    if (lines.size() < 2) {
        return st::failure(st::ErrorCode::ParseError,
                           std::string{"coldstart: CSV needs a header row "
                                       "and at least one data row"});
    }

    auto const  header_fields = st::core::csv::split_fields(lines[0]);
    std::size_t const pid_count = header_fields.size();
    if (pid_count == 0) {
        return st::failure(st::ErrorCode::ParseError,
                           std::string{"coldstart: CSV header is empty"});
    }
    std::size_t const needed = max_referenced_column(mapping);
    if (needed > pid_count) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"coldstart: mapping references column "
                                       "index "}
                               + std::to_string(needed - 1)
                               + " but CSV has only "
                               + std::to_string(pid_count) + " columns");
    }

    std::vector<double> samples;
    samples.reserve((lines.size() - 1) * pid_count);
    for (std::size_t row = 1; row < lines.size(); ++row) {
        auto const fields = st::core::csv::split_fields(lines[row]);
        if (fields.size() < pid_count) {
            return st::failure(st::ErrorCode::ParseError,
                               std::string{"coldstart: CSV row "}
                                   + std::to_string(row + 1) + " has "
                                   + std::to_string(fields.size())
                                   + " fields, expected "
                                   + std::to_string(pid_count));
        }
        for (std::size_t col = 0; col < pid_count; ++col) {
            double v = 0.0;
            if (!st::core::csv::parse_double(fields[col], v)) {
                return st::failure(st::ErrorCode::ParseError,
                                   std::string{"coldstart: CSV row "}
                                       + std::to_string(row + 1) + " col "
                                       + std::to_string(col + 1)
                                       + ": not a number");
            }
            samples.push_back(v);
        }
    }

    return snapshot_from_samples(samples, pid_count, mapping, cfg, methodology);
}

}  // namespace st::log::coldstart
