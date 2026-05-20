// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LOG_EBCS_HPP
#define ST_LOG_EBCS_HPP

// EBCS PID assistant — see docs/05-improvements.md §11.
// Domain types only; no ImGui or UI dependency.
//
// What it does: take a CSV log containing a sequence of tip-in events
// (throttle steps from low to high, target boost rises sharply), find
// the step responses, and characterize each one — rise time, overshoot,
// settling time, steady-state error. From those characterizations
// produce heuristic gain-adjustment suggestions ("consider reducing
// proportional gain"; "consider increasing derivative gain"). The
// actual gain *values* are in the user's ROM via the existing edit
// surface; this module just surfaces what direction to nudge them.
//
// Why this matters: every modern Subaru ECU has an Electronic Boost
// Control System (EBCS) with documented Kp/Ki/Kd tables. Tuners almost
// universally adjust the target-boost map and the wastegate-duty map
// but ignore the actual controller gains — leaving boost overshoot /
// undershoot / oscillation as a long-tail complaint that has no
// community workflow to fix. This module is the fix.
//
// Output is advisory. Bad PID gains can cause boost overshoot that
// damages the engine. Suggestions ship as "starting points for dyno
// verification" — never as auto-applied edits.

#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::log::ebcs {

inline constexpr std::size_t kNoColumn =
    std::numeric_limits<std::size_t>::max();

// Maps the input row's flat layout to the signals this module needs.
// timestamp + target_boost + actual_boost + throttle are mandatory;
// the rest are optional (used only for tagging and quality gating).
struct PidMapping {
    std::size_t  timestamp_idx{kNoColumn};
    std::size_t  target_boost_idx{kNoColumn};
    std::size_t  actual_boost_idx{kNoColumn};
    std::size_t  wgdc_idx{kNoColumn};       // wastegate duty cycle (%)
    std::size_t  throttle_idx{kNoColumn};   // throttle position (%)
    std::size_t  rpm_idx{kNoColumn};
};

// Quality verdict per tip-in. Aggregates only count `Good` events.
enum class StepQuality : std::uint8_t {
    Good        = 0,   // clean step response, characterization is trustworthy
    Choppy      = 1,   // actual_boost too noisy for stable characterization
    Slow        = 2,   // didn't reach target before sample window closed
    Spiked      = 3,   // overshoot > spike_threshold; likely a knock event
    Aborted     = 4,   // user lifted throttle before the response settled
};

// Per-event characterization. One PhaseSample-equivalent for tip-ins.
struct TipInEvent {
    double         start_timestamp{0.0};
    double         end_timestamp{0.0};
    double         initial_boost{0.0};
    double         target_boost{0.0};
    double         peak_boost{0.0};
    double         steady_state_boost{0.0};
    // Step characterization (only meaningful for Good events).
    double         rise_time_s{0.0};         // 10% → 90% of step
    double         overshoot_pct{0.0};       // (peak - target) / step_size
    double         settling_time_s{0.0};     // time to stay within ±5% of target
    double         steady_state_error{0.0};  // signed: actual - target
    double         initial_rpm{0.0};
    StepQuality    quality{StepQuality::Good};
};

struct BoostSnapshot {
    std::vector<TipInEvent>      events;          // all detected, any quality
    // Aggregates over Good-quality events only.
    double                       median_rise_time_s{0.0};
    double                       median_overshoot_pct{0.0};
    double                       median_settling_time_s{0.0};
    double                       mean_steady_state_error{0.0};
    std::uint32_t                good_event_count{0};
    std::uint32_t                total_event_count{0};
    std::uint64_t                samples_considered{0};
    // Heuristic suggestions for the user. Each entry is a single
    // human-readable line; the UI renders them as a bulleted list.
    // Empty when there's nothing actionable (or no Good events).
    std::vector<std::string>     gain_suggestions;
};

enum class TimestampUnit : std::uint8_t {
    UnixSeconds  = 0,
    UnixMillis   = 1,
    UnixMicros   = 2,
    RowIndex     = 3,
};

struct DetectorConfig {
    // ---- Tip-in event detection ----
    // Throttle position must step up by at least this many percentage
    // points (over up to 0.5 s) for a candidate event to register.
    double         throttle_step_threshold_pct{30.0};
    // Target boost must rise by at least this much (in whatever unit
    // the log uses — psi or kPa) to confirm the candidate is a real
    // boost step rather than a clutched coast.
    double         target_boost_step_threshold{2.0};
    // Skip events shorter than this — likely glitches, not real responses.
    double         min_event_duration_s{0.5};
    // Stop analyzing an event after this much elapsed time. Events that
    // haven't settled by here are marked `Slow`.
    double         max_event_duration_s{6.0};

    // ---- Quality gating ----
    // Above this overshoot percentage, an event is marked `Spiked`.
    double         spike_overshoot_threshold_pct{50.0};
    // Coefficient-of-variation in actual_boost above which we mark
    // `Choppy` — too noisy for the rise-time / settling-time math.
    double         noise_cv_threshold{0.30};

    // ---- Suggestion heuristics ----
    // Above this median overshoot, the system suggests reducing Kp
    // and/or increasing Kd.
    double         overshoot_warn_pct{15.0};
    // Above this median rise time, the system suggests increasing Kp.
    double         slow_rise_threshold_s{1.5};
    // Above this absolute steady-state error, the system suggests
    // adjusting Ki.
    double         steady_state_error_threshold{1.0};

    TimestampUnit  timestamp_unit{TimestampUnit::UnixSeconds};
};

// Detect tip-in events and aggregate.
[[nodiscard]] BoostSnapshot snapshot_from_samples(
    std::span<double const>   samples_row_major,
    std::size_t               pid_count,
    PidMapping const         &mapping,
    DetectorConfig const     &cfg);

// CSV replay entry. Same CSV conventions as the sibling modules.
//
// Returns:
//   FileNotFound on missing path
//   ParseError on empty / malformed CSV or non-numeric field
//   InvalidArgument when mandatory mapping columns (timestamp_idx,
//     target_boost_idx, actual_boost_idx, throttle_idx) are missing,
//     or when the mapping references a column past the end of the CSV
[[nodiscard]] st::Result<BoostSnapshot> snapshot_from_csv(
    std::string_view          csv_path,
    PidMapping const         &mapping,
    DetectorConfig const     &cfg);

}  // namespace st::log::ebcs

#endif  // ST_LOG_EBCS_HPP
