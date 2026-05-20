// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LOG_KNOCK_DASHBOARD_HPP
#define ST_LOG_KNOCK_DASHBOARD_HPP

// Per-cylinder knock dashboard — see docs/05-improvements.md §11.
// Domain types only; no ImGui or UI dependency.
//
// Cylinder-count-aware: supports H4 (FA / EJ) and H6 (EZ30 / EZ36) by
// taking the count as a runtime parameter, not a compile-time template.
// Slots beyond `cylinder_count` are zero-initialized and ignored.
//
// Engine support tiers:
//   - H4 FA-DIT / EJ20G / EJ25 / EJ255 / EJ257 — full per-cyl FLKC and
//     FBKC available via SSM extended PIDs. Best-case experience.
//   - H6 EZ30 / EZ36 (Outback 3.0R/3.6R, Tribeca, Legacy 3.6) — per-cyl
//     FBKC exposed; FLKC granularity varies by firmware. 3×2 grid.
//   - H6 EG33 (SVX, 1991-96) — ancient SSM, single global knock count.
//     Dashboard runs in degraded mode: cylinder_count=6 but most
//     `kNoPid` slots; UI shows a "no per-cylinder signal" placeholder.
//     Documented as supported with limitations, not first-class.

#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace st::log::knock {

inline constexpr std::uint8_t kMaxCylinders = 6;
inline constexpr std::size_t  kNoPid        = std::numeric_limits<std::size_t>::max();

// Maps logical cylinders to PID indices in a LogStream sample row.
// A `kNoPid` slot means "this signal is not in the current log; metric
// computed from it stays at its default."
struct PidMapping {
    // Fine knock learn (FLKC) per cyl — long-term per-cell knock retard.
    std::array<std::size_t, kMaxCylinders> fine_knock_learn{
        kNoPid, kNoPid, kNoPid, kNoPid, kNoPid, kNoPid};

    // Feedback knock correction (FBKC) per cyl — short-term knock retard.
    std::array<std::size_t, kMaxCylinders> feedback_knock{
        kNoPid, kNoPid, kNoPid, kNoPid, kNoPid, kNoPid};

    // Raw knock-sensor noise per cyl (if exposed by the logger; rare).
    std::array<std::size_t, kMaxCylinders> knock_sensor_noise{
        kNoPid, kNoPid, kNoPid, kNoPid, kNoPid, kNoPid};

    // RPM and Load PIDs — used to gate the metrics (only count knock
    // when the engine is actually loaded above an idle threshold).
    std::size_t rpm_idx{kNoPid};
    std::size_t load_idx{kNoPid};

    // 4 for H4 (FA20DIT / EJ20-25), 6 for H6 (EZ30 / EZ36).
    std::uint8_t cylinder_count{4};
};

// Per-cylinder rolling-window snapshot. The UI renders one of these
// per cylinder in the dashboard grid (2×2 for H4, 3×2 for H6).
struct CylinderSnapshot {
    double        current_flkc{0.0};       // latest fine knock learn   [°]
    double        current_fbkc{0.0};       // latest feedback knock     [°]
    double        mean_flkc_window{0.0};   // window mean of FLKC       [°]
    double        min_flkc_window{0.0};    // window worst (most negative)
    std::uint32_t event_count_window{0};   // negative-FBKC samples in window
    double        delta_from_cyl_mean{0.0};// (this_cyl_mean - all_cyl_mean)

    // Recent samples for the strip chart, oldest-first.
    // Length is at most `KnockSnapshot::strip_capacity`.
    std::vector<double> strip_flkc;
    std::vector<double> strip_fbkc;
};

struct KnockSnapshot {
    std::uint8_t                                cylinder_count{4};
    std::array<CylinderSnapshot, kMaxCylinders> per_cyl{};
    double                                      window_seconds{10.0};
    std::size_t                                 strip_capacity{600};
    std::uint64_t                               samples_considered{0};
    std::uint64_t                               samples_gated_out{0};
};

struct WindowConfig {
    double  window_seconds{10.0};
    double  sample_rate_hz{20.0};
    // Gate thresholds — only count a sample toward knock metrics when
    // the engine is actually loaded. Default values are conservative;
    // tune per platform when wiring up.
    double  min_rpm{1500.0};
    double  min_load{1.5};        // g/rev or whatever the load axis is
    bool    require_load_gate{true};
};

// Compute a snapshot from a row-major (samples × pids) flat buffer.
// `pid_count` is the row stride. The caller has already chosen the
// sample window — this function does the per-cylinder aggregation.
[[nodiscard]] KnockSnapshot snapshot_from_samples(
    std::span<double const>  samples_row_major,
    std::size_t              pid_count,
    PidMapping const        &mapping,
    WindowConfig const      &cfg);

// CSV replay entry point — reads a SubuwuTuner CSV log (per `st::log::CsvSink`
// format) and returns a snapshot. Returns Err on malformed CSV or missing
// header columns referenced by `mapping`.
[[nodiscard]] st::core::Result<KnockSnapshot> snapshot_from_csv(
    std::string_view         csv_path,
    PidMapping const        &mapping,
    WindowConfig const      &cfg);

}  // namespace st::log::knock

#endif  // ST_LOG_KNOCK_DASHBOARD_HPP
