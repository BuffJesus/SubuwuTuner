// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LOG_COLDSTART_HPP
#define ST_LOG_COLDSTART_HPP

// Cold-start tuning workflow — see docs/05-improvements.md §11.
// Domain types only; no ImGui or UI dependency.
//
// What the existing tools (Atlas et al.) expose: the cold-start
// tables — cranking enrichment, open-loop fueling enrichment vs ECT,
// idle target RPM vs ECT, spark advance compensation vs IAT, catalyst
// heating mode flags. What none of them ship: a *methodology* for
// tuning them. Most dyno tuners tune warm because dynos don't help
// with cold-start; the workflow gap is real.
//
// This module's contribution is the analysis side: take a cold-start
// log (engine soaked < 20 °C, key-on, drive to operating temp), bin
// the samples by ECT, classify which test phase each sample belongs
// to, and surface compliance against a user-defined target lambda
// curve. The UI on top sequences the user through "what to log next"
// and proposes table edits from the binned observations.

#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace st::log::coldstart {

inline constexpr std::size_t kNoColumn = std::numeric_limits<std::size_t>::max();

// Maps the input row's flat layout to the signals this module cares
// about. timestamp_idx + ect_idx + rpm_idx are mandatory; signals that
// aren't present in the log stay as `kNoColumn`.
struct PidMapping {
    std::size_t timestamp_idx{kNoColumn};
    std::size_t ect_idx{kNoColumn}; // engine coolant temp (°C)
    std::size_t iat_idx{kNoColumn}; // intake air temp (°C)
    std::size_t rpm_idx{kNoColumn};
    std::size_t observed_lambda_idx{kNoColumn};  // wideband λ
    std::size_t commanded_lambda_idx{kNoColumn}; // ECU target λ
    std::size_t timing_deg_idx{kNoColumn};       // ignition advance
    std::size_t closed_loop_idx{kNoColumn};      // 1 = closed loop, 0 = OL
};

// Discrete cold-start phases. Used to bucket samples by what the engine
// was actually doing, not just by ECT. A 600 RPM hot-restart sample at
// 25 °C looks different from a cranking-at-25 °C sample even though the
// ECT is the same.
enum class TestPhase : std::uint8_t {
    PreCrank = 0,      // RPM == 0 (engine off / pre-start)
    Cranking = 1,      // 0 < RPM < ~400
    InitialFiring = 2, // ~400 <= RPM < ~700, engine firing but not stable
    HighIdle = 3,      // 700 <= RPM < ~1500, ECT below cold_threshold
    Warmup = 4,        // engine warm enough to leave high-idle, ECT
                       //   still below operating-temp threshold
    ClosedLoop = 5,    // ECT above operating-temp threshold (reference
                       //   bucket — not part of cold-start proper)
};

inline constexpr std::size_t kPhaseCount = 6;

// Per-sample classification result. The caller's index into the input
// buffer is implicit (parallel to the input rows after gating).
struct PhaseSample {
    TestPhase phase{TestPhase::PreCrank};
    double timestamp{0.0};
    double ect_c{0.0};
    double iat_c{0.0};
    double rpm{0.0};
    double observed_lambda{0.0};
    double commanded_lambda{0.0};
    double timing_deg{0.0};
    bool closed_loop{false};
};

// ECT-binned aggregate. One entry per non-empty ECT bin. The lambda
// fields are populated only when the log carries an observed_lambda
// column; otherwise count > 0 with mean/min/max as defaults.
struct EctBin {
    double ect_center_c{0.0};
    double observed_lambda_mean{0.0};
    double observed_lambda_min{0.0};
    double observed_lambda_max{0.0};
    double commanded_lambda_mean{0.0};
    double deviation_from_target{0.0}; // |observed - target_curve(ect)|
    std::uint32_t count{0};
    bool has_lambda{false};
};

// User-defined target curve: pairs of (ect_c, target_lambda).
// Values between points are linearly interpolated; outside the range,
// the nearest endpoint is used.
struct TargetLambdaCurve {
    std::vector<std::pair<double, double>> points;

    // Sample the curve at `ect_c`. Returns 1.0 (stoichiometric default)
    // if `points` is empty.
    [[nodiscard]] double sample(double ect_c) const noexcept;
};

// Optional methodology bundle. Extend as additional target curves are
// defined (timing pull vs IAT, idle target RPM vs ECT, etc.). The
// snapshot uses target_lambda_vs_ect for `deviation_from_target`.
struct Methodology {
    TargetLambdaCurve target_lambda_vs_ect;
};

struct ColdStartSnapshot {
    std::vector<PhaseSample> samples; // gated rows only
    std::array<std::uint32_t, kPhaseCount> phase_counts{};
    std::array<double, kPhaseCount> phase_seconds{};
    std::vector<EctBin> ect_bins;
    double coldest_ect_c{0.0};
    double warmest_ect_c{0.0};
    double mean_lambda_deviation{0.0};
    std::uint64_t samples_considered{0};
    std::uint64_t samples_gated_out{0};
};

enum class TimestampUnit : std::uint8_t {
    UnixSeconds = 0,
    UnixMillis = 1,
    UnixMicros = 2,
    RowIndex = 3,
};

struct WindowConfig {
    // ECT (°C) below which a sample is considered "cold-start regime."
    // Samples above this go into the ClosedLoop bucket as reference but
    // do NOT contribute to ect_bins.
    double cold_threshold_c{55.0};

    // ECT bin width for `ect_bins`. Default 5 °C buckets cover the
    // typical -30 ... +55 cold-start range in ~17 bins.
    double ect_bin_width_c{5.0};

    // RPM thresholds for the phase classifier. Defaults match the
    // descriptive ranges in TestPhase comments.
    double cranking_max_rpm{400.0};
    double initial_firing_max_rpm{700.0};
    double high_idle_max_rpm{1500.0};

    // Minimum samples-per-bin to keep the bin in `ect_bins`. Buckets
    // below this are dropped (but their samples still count toward
    // samples_considered).
    std::uint32_t min_samples_per_bin{3};

    TimestampUnit timestamp_unit{TimestampUnit::UnixSeconds};
};

// Classify and aggregate. Returns an empty snapshot for empty input.
[[nodiscard]] ColdStartSnapshot snapshot_from_samples(std::span<double const> samples_row_major,
                                                      std::size_t pid_count,
                                                      PidMapping const &mapping,
                                                      WindowConfig const &cfg,
                                                      Methodology const &methodology);

// CSV replay entry point. Same CSV conventions as the sibling modules
// (knock_dashboard, adaptive_history): CR/LF tolerant, blank lines and
// `#` comments skipped, header drives column count.
//
// Returns:
//   FileNotFound on missing path
//   ParseError on empty / malformed CSV or non-numeric field
//   InvalidArgument when mandatory mapping columns (timestamp_idx,
//     ect_idx, rpm_idx) are missing, or when the mapping references a
//     column past the end of the CSV
[[nodiscard]] st::Result<ColdStartSnapshot> snapshot_from_csv(std::string_view csv_path,
                                                              PidMapping const &mapping,
                                                              WindowConfig const &cfg,
                                                              Methodology const &methodology);

} // namespace st::log::coldstart

#endif // ST_LOG_COLDSTART_HPP
