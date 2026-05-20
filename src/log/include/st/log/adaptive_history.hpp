// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LOG_ADAPTIVE_HISTORY_HPP
#define ST_LOG_ADAPTIVE_HISTORY_HPP

// Adaptive-learning history visualizer — see docs/05-improvements.md §11.
// Domain types only; no ImGui or UI dependency.
//
// Charts the long-cycle drift of ECU adaptive-learning state — LTFT,
// DAM, idle-adapt — across days and weeks. The state arrays are already
// in the def; what's missing in the existing tools is a UI that treats
// them as a *time series* rather than a point-in-time snapshot.
//
// Key contrast vs. the knock dashboard:
//   - Knock dashboard windows within a single log file (~10 s of samples).
//   - This module aggregates across LONG spans (a single log of an hour-
//     long drive, or — more usefully — many logs concatenated over weeks).
//
// What we're looking for: drift. A stable engine's LTFT/DAM should sit
// near zero / near 1.0. Persistent negative LTFT trending more negative
// over weeks = injectors getting dirty or MAF aging. DAM trending below
// 1.0 = knock-learning is acting on a degrading condition. Idle-adapt
// drifting away from zero = vacuum leak or IAC valve wear.

#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace st::log::adaptive {

// Signals the visualizer tracks. Extend cautiously — every entry costs
// a CSV column the user must provide.
enum class SignalKind : std::uint8_t {
    Ltft = 0,      // Long-term fuel trim (% correction, typically -25..+25)
    Dam = 1,       // Dynamic advance multiplier (0.0..1.0, scales ignition advance)
    IdleAdapt = 2, // Idle air control adaptation (% or steps; sign of vacuum leak)
};

inline constexpr std::size_t kSignalCount = 3;
inline constexpr std::size_t kNoColumn = std::numeric_limits<std::size_t>::max();

// Column-mapping from the input row's flat layout to the signals this
// module knows about. `timestamp_idx` is mandatory; signal indices are
// optional and `kNoColumn` for ones the log doesn't carry.
struct ColumnMapping {
    // Per-sample timestamp column. Units interpreted per
    // BucketConfig::timestamp_unit. kNoColumn is not valid here — every
    // row needs a time anchor.
    std::size_t timestamp_idx{kNoColumn};
    // Per-signal column index (kNoColumn = signal not present in log).
    std::array<std::size_t, kSignalCount> signal_idx{kNoColumn, kNoColumn, kNoColumn};
};

// Single time-bucket aggregate for one signal.
struct TimeSeriesPoint {
    double bucket_center; // seconds since epoch (or whatever epoch
                          // the input timestamps use)
    double mean;
    double min;
    double max;
    std::uint32_t count;
};

// Per-signal output: the bucketed series plus simple cross-bucket stats.
struct SignalSeries {
    SignalKind kind{SignalKind::Ltft};
    std::vector<TimeSeriesPoint> points; // bucket-ordered by time
    double overall_mean{0.0};
    double overall_min{0.0};
    double overall_max{0.0};
    // Linear least-squares slope (signal change per second). Multiply by
    // 86400 for "per day". Positive = signal trending up over time.
    double drift_per_second{0.0};
    bool has_data{false};
};

struct HistorySnapshot {
    std::array<SignalSeries, kSignalCount> series{};
    std::uint64_t total_samples{0};
    double earliest_timestamp{0.0};
    double latest_timestamp{0.0};
    double time_span_seconds{0.0};
};

// Timestamp unit hint. Decouples the file format from the math —
// the bucketer always works in seconds internally.
enum class TimestampUnit : std::uint8_t {
    UnixSeconds = 0, // unix epoch in seconds
    UnixMillis = 1,  // unix epoch in milliseconds
    UnixMicros = 2,  // unix epoch in microseconds
    RowIndex = 3,    // monotonic row number; bucket math treats it
                     // as a synthetic 1-second-tick clock
};

struct BucketConfig {
    // Width of each time bucket, in seconds. Default = 1 day. Use 0 to
    // disable bucketing and emit one TimeSeriesPoint per raw sample.
    double bucket_seconds{86400.0};
    TimestampUnit timestamp_unit{TimestampUnit::UnixSeconds};
    // Lower bound on samples-per-bucket — buckets that don't reach it are
    // dropped from `points` (still counted in `total_samples`). Helps
    // visually when a couple of stray data points would otherwise dominate
    // a bucket's mean. 0 = keep every non-empty bucket.
    std::uint32_t min_samples_per_bucket{0};
};

// Aggregate a flat row-major (samples × pid_count) buffer into a
// per-signal time series. Rows whose timestamp_idx column is non-finite
// or outside a sane range are silently skipped.
[[nodiscard]] HistorySnapshot snapshot_from_samples(std::span<double const> samples_row_major,
                                                    std::size_t pid_count,
                                                    ColumnMapping const &mapping,
                                                    BucketConfig const &cfg);

// CSV replay entry point. Same CSV conventions as
// `st::log::knock::snapshot_from_csv`: CR/LF tolerant, blank lines and
// `#` comments skipped, header drives column count, data rows must have
// the column count the header declares.
//
// Returns:
//   FileNotFound on missing path
//   ParseError on empty / malformed CSV or non-numeric field
//   InvalidArgument when the mapping references a column past the end,
//     when timestamp_idx is kNoColumn, or when no signal column is set
[[nodiscard]] st::Result<HistorySnapshot>
snapshot_from_csv(std::string_view csv_path, ColumnMapping const &mapping, BucketConfig const &cfg);

} // namespace st::log::adaptive

#endif // ST_LOG_ADAPTIVE_HISTORY_HPP
