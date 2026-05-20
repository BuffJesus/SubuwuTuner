// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/adaptive_history.hpp"

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

namespace st::log::adaptive {

namespace {

inline bool has_col(std::size_t idx) noexcept {
    return idx != kNoColumn;
}

// Normalize input timestamps to seconds based on the configured unit.
inline double to_seconds(double raw, TimestampUnit unit) noexcept {
    switch (unit) {
        case TimestampUnit::UnixSeconds: return raw;
        case TimestampUnit::UnixMillis:  return raw / 1'000.0;
        case TimestampUnit::UnixMicros:  return raw / 1'000'000.0;
        case TimestampUnit::RowIndex:    return raw;  // synthetic 1-Hz clock
    }
    return raw;
}

struct BucketAcc {
    double         sum{0.0};
    double         min{ std::numeric_limits<double>::infinity()};
    double         max{-std::numeric_limits<double>::infinity()};
    std::uint32_t  count{0};
};

inline std::int64_t bucket_index(double timestamp_s, double bucket_s) noexcept {
    // bucket_s > 0 guaranteed by caller; floor toward minus-infinity so
    // negative timestamps still bucket correctly.
    return static_cast<std::int64_t>(std::floor(timestamp_s / bucket_s));
}

std::size_t max_referenced_column(ColumnMapping const &m) noexcept {
    std::size_t lo = 0;
    auto const  consider = [&](std::size_t v) {
        if (v != kNoColumn && v + 1 > lo) {
            lo = v + 1;
        }
    };
    consider(m.timestamp_idx);
    for (auto v : m.signal_idx) consider(v);
    return lo;
}

}  // namespace

HistorySnapshot snapshot_from_samples(std::span<double const>   samples_row_major,
                                     std::size_t               pid_count,
                                     ColumnMapping const      &mapping,
                                     BucketConfig const       &cfg) {
    HistorySnapshot snap{};
    for (std::size_t k = 0; k < kSignalCount; ++k) {
        snap.series[k].kind = static_cast<SignalKind>(k);
    }

    if (pid_count == 0
        || !has_col(mapping.timestamp_idx)
        || samples_row_major.empty()) {
        return snap;
    }
    std::size_t const n_rows = samples_row_major.size() / pid_count;
    if (n_rows == 0) {
        return snap;
    }

    double const bucket_s = (cfg.bucket_seconds > 0.0)
        ? cfg.bucket_seconds
        : 0.0;  // 0 = one point per sample

    // Per-signal: bucket_index -> accumulator. Ordered map keeps buckets
    // sorted ascending, which is what the output expects.
    std::array<std::map<std::int64_t, BucketAcc>, kSignalCount> buckets{};

    double earliest = std::numeric_limits<double>::infinity();
    double latest   = -std::numeric_limits<double>::infinity();

    for (std::size_t row = 0; row < n_rows; ++row) {
        double const ts_raw = samples_row_major[row * pid_count
                                                 + mapping.timestamp_idx];
        if (!std::isfinite(ts_raw)) continue;
        double const ts_s = to_seconds(ts_raw, cfg.timestamp_unit);
        if (!std::isfinite(ts_s)) continue;

        if (ts_s < earliest) earliest = ts_s;
        if (ts_s > latest)   latest   = ts_s;

        // For "no bucketing" use a synthetic per-row bucket index so each
        // row becomes its own point.
        std::int64_t const bidx = (bucket_s > 0.0)
            ? bucket_index(ts_s, bucket_s)
            : static_cast<std::int64_t>(row);

        for (std::size_t k = 0; k < kSignalCount; ++k) {
            std::size_t const col = mapping.signal_idx[k];
            if (!has_col(col)) continue;
            double const v = samples_row_major[row * pid_count + col];
            if (!std::isfinite(v)) continue;

            auto      &acc = buckets[k][bidx];
            acc.sum   += v;
            if (v < acc.min) acc.min = v;
            if (v > acc.max) acc.max = v;
            acc.count++;
            snap.total_samples++;
        }
    }

    if (earliest != std::numeric_limits<double>::infinity()) {
        snap.earliest_timestamp = earliest;
        snap.latest_timestamp   = latest;
        snap.time_span_seconds  = latest - earliest;
    }

    // Reduce per-signal buckets to TimeSeriesPoints + overall stats +
    // drift slope. Linear least-squares over (bucket_center_s, mean).
    for (std::size_t k = 0; k < kSignalCount; ++k) {
        SignalSeries &s = snap.series[k];
        s.kind = static_cast<SignalKind>(k);
        auto   const &per_bidx = buckets[k];
        if (per_bidx.empty()) {
            continue;
        }
        s.has_data    = true;
        s.overall_min = std::numeric_limits<double>::infinity();
        s.overall_max = -std::numeric_limits<double>::infinity();

        double sum_for_overall   = 0.0;
        std::uint64_t count_overall = 0;

        s.points.reserve(per_bidx.size());
        for (auto const &[bidx, acc] : per_bidx) {
            if (acc.count < cfg.min_samples_per_bucket) continue;
            double const center = (bucket_s > 0.0)
                ? (static_cast<double>(bidx) * bucket_s
                   + bucket_s * 0.5)
                : static_cast<double>(bidx);
            double const mean = acc.sum / static_cast<double>(acc.count);
            s.points.push_back({center, mean, acc.min, acc.max, acc.count});
            sum_for_overall += acc.sum;
            count_overall   += acc.count;
            if (acc.min < s.overall_min) s.overall_min = acc.min;
            if (acc.max > s.overall_max) s.overall_max = acc.max;
        }
        if (count_overall > 0) {
            s.overall_mean = sum_for_overall
                             / static_cast<double>(count_overall);
        } else {
            // All buckets failed the min_samples_per_bucket filter; clear
            // has_data so the UI shows a "no data" placeholder.
            s.has_data    = false;
            s.overall_min = 0.0;
            s.overall_max = 0.0;
        }

        // Linear least-squares slope (per-second) over points' (center, mean).
        if (s.points.size() >= 2) {
            double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
            for (auto const &p : s.points) {
                sum_x  += p.bucket_center;
                sum_y  += p.mean;
                sum_xy += p.bucket_center * p.mean;
                sum_xx += p.bucket_center * p.bucket_center;
            }
            double const n_pts = static_cast<double>(s.points.size());
            double const denom = n_pts * sum_xx - sum_x * sum_x;
            if (std::abs(denom) > 1e-12) {
                s.drift_per_second = (n_pts * sum_xy - sum_x * sum_y) / denom;
            }
        }
    }

    return snap;
}

st::Result<HistorySnapshot> snapshot_from_csv(std::string_view          csv_path,
                                              ColumnMapping const      &mapping,
                                              BucketConfig const       &cfg) {
    if (!has_col(mapping.timestamp_idx)) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"adaptive-history: mapping is missing "
                                       "timestamp column"});
    }
    bool any_signal = false;
    for (auto v : mapping.signal_idx) {
        if (has_col(v)) { any_signal = true; break; }
    }
    if (!any_signal) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"adaptive-history: mapping has no "
                                       "signal columns set"});
    }

    std::ifstream f{std::string{csv_path}};
    if (!f) {
        return st::failure(st::ErrorCode::FileNotFound,
                           std::string{"adaptive-history: cannot open '"}
                               + std::string{csv_path} + "'");
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string const text = std::move(buf).str();

    auto const lines = st::core::csv::split_lines(text);
    if (lines.size() < 2) {
        return st::failure(st::ErrorCode::ParseError,
                           std::string{"adaptive-history: CSV needs a header "
                                       "row and at least one data row"});
    }

    auto const  header_fields = st::core::csv::split_fields(lines[0]);
    std::size_t const pid_count = header_fields.size();
    if (pid_count == 0) {
        return st::failure(st::ErrorCode::ParseError,
                           std::string{"adaptive-history: CSV header is empty"});
    }
    std::size_t const needed = max_referenced_column(mapping);
    if (needed > pid_count) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"adaptive-history: mapping references "
                                       "column index "}
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
                               std::string{"adaptive-history: CSV row "}
                                   + std::to_string(row + 1) + " has "
                                   + std::to_string(fields.size())
                                   + " fields, expected "
                                   + std::to_string(pid_count));
        }
        for (std::size_t col = 0; col < pid_count; ++col) {
            double v = 0.0;
            if (!st::core::csv::parse_double(fields[col], v)) {
                return st::failure(st::ErrorCode::ParseError,
                                   std::string{"adaptive-history: CSV row "}
                                       + std::to_string(row + 1) + " col "
                                       + std::to_string(col + 1)
                                       + ": not a number");
            }
            samples.push_back(v);
        }
    }

    return snapshot_from_samples(samples, pid_count, mapping, cfg);
}

}  // namespace st::log::adaptive
