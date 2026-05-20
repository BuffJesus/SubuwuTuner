// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/ebcs.hpp"

#include "st/core/csv.hpp"
#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace st::log::ebcs {

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

inline double row_val(std::span<double const> samples,
                      std::size_t              pid_count,
                      std::size_t              row,
                      std::size_t              col) noexcept {
    return samples[row * pid_count + col];
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    auto const mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    double const m = v[mid];
    if (v.size() % 2 == 1) return m;
    // For even-sized vectors, average the two central values.
    auto const lower_max = *std::max_element(
        v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return (m + lower_max) * 0.5;
}

// Build per-row scratch with derived signals so the rest of the
// detector reads simple arrays.
struct Row {
    double  t{0.0};
    double  target{0.0};
    double  actual{0.0};
    double  throttle{0.0};
    double  rpm{0.0};
};

std::vector<Row> materialize(std::span<double const> samples,
                             std::size_t              pid_count,
                             PidMapping const        &m,
                             TimestampUnit            unit) {
    std::size_t const n = samples.size() / pid_count;
    std::vector<Row> out;
    out.reserve(n);
    for (std::size_t r = 0; r < n; ++r) {
        Row x{};
        double const ts_raw = row_val(samples, pid_count, r, m.timestamp_idx);
        x.t        = to_seconds(ts_raw, unit);
        x.target   = row_val(samples, pid_count, r, m.target_boost_idx);
        x.actual   = row_val(samples, pid_count, r, m.actual_boost_idx);
        x.throttle = row_val(samples, pid_count, r, m.throttle_idx);
        if (has_col(m.rpm_idx)) {
            x.rpm = row_val(samples, pid_count, r, m.rpm_idx);
        }
        // Skip rows with non-finite values; the detector treats them as gaps.
        if (!std::isfinite(x.t) || !std::isfinite(x.target)
            || !std::isfinite(x.actual) || !std::isfinite(x.throttle)) {
            continue;
        }
        out.push_back(x);
    }
    return out;
}

// Characterize a single tip-in window. Caller has identified the
// step-start row and the end row (either when target+5% is re-crossed
// downward, or max_event_duration_s elapses).
TipInEvent characterize(std::span<Row const> window, DetectorConfig const &cfg) {
    TipInEvent ev{};
    if (window.empty()) {
        ev.quality = StepQuality::Aborted;
        return ev;
    }
    ev.start_timestamp = window.front().t;
    ev.end_timestamp   = window.back().t;
    ev.initial_boost   = window.front().actual;
    ev.target_boost    = window.front().target;
    ev.initial_rpm     = window.front().rpm;

    // Take target as the median of the last 25% of the window — handles
    // the case where target itself drifts during the event.
    {
        std::vector<double> tgt_tail;
        std::size_t const tail_start = window.size() * 3 / 4;
        for (std::size_t i = tail_start; i < window.size(); ++i) {
            tgt_tail.push_back(window[i].target);
        }
        if (!tgt_tail.empty()) {
            ev.target_boost = median_of(std::move(tgt_tail));
        }
    }

    // Peak and steady-state actual boost.
    double peak = window.front().actual;
    for (auto const &r : window) {
        if (r.actual > peak) peak = r.actual;
    }
    ev.peak_boost = peak;

    {
        std::vector<double> tail;
        std::size_t const tail_start = window.size() * 3 / 4;
        for (std::size_t i = tail_start; i < window.size(); ++i) {
            tail.push_back(window[i].actual);
        }
        if (!tail.empty()) {
            ev.steady_state_boost = median_of(std::move(tail));
        }
    }
    ev.steady_state_error = ev.steady_state_boost - ev.target_boost;

    double const step_size =
        std::max(ev.target_boost - ev.initial_boost, 1e-6);

    // Rise time: time from when actual crosses 10% to 90% of the step.
    double const ten_pct    = ev.initial_boost + 0.10 * step_size;
    double const ninety_pct = ev.initial_boost + 0.90 * step_size;
    double t_10 = -1.0, t_90 = -1.0;
    for (auto const &r : window) {
        if (t_10 < 0.0 && r.actual >= ten_pct)    t_10 = r.t;
        if (t_10 >= 0.0 && r.actual >= ninety_pct) {
            t_90 = r.t;
            break;
        }
    }
    if (t_10 >= 0.0 && t_90 >= 0.0) {
        ev.rise_time_s = t_90 - t_10;
    }

    // Overshoot pct.
    if (peak > ev.target_boost) {
        ev.overshoot_pct = 100.0 * (peak - ev.target_boost) / step_size;
    }

    // Settling time: first time after peak when |actual - target| stays
    // within ±5% of step_size for 0.5 s.
    double const settle_band = 0.05 * step_size;
    double settling_start = -1.0;
    for (std::size_t i = 0; i < window.size(); ++i) {
        bool const within = std::abs(window[i].actual - ev.target_boost)
                            <= settle_band;
        if (within && settling_start < 0.0) {
            settling_start = window[i].t;
        } else if (!within) {
            settling_start = -1.0;
        } else if (within && window[i].t - settling_start >= 0.5) {
            ev.settling_time_s = settling_start - ev.start_timestamp;
            break;
        }
    }

    // ---- Quality classification ----
    // Choppy: high coefficient-of-variation across the steady-state tail
    // only (the transient itself is supposed to swing wildly — that's the
    // step). 25% of window matches the steady-state extraction above.
    std::size_t const tail_start = window.size() * 3 / 4;
    std::size_t const tail_count = window.size() - tail_start;
    double sum = 0.0;
    for (std::size_t i = tail_start; i < window.size(); ++i) {
        sum += window[i].actual;
    }
    double const mean = (tail_count > 0)
        ? sum / static_cast<double>(tail_count) : 0.0;
    double var = 0.0;
    for (std::size_t i = tail_start; i < window.size(); ++i) {
        double const d = window[i].actual - mean;
        var += d * d;
    }
    double const stddev = (tail_count > 0)
        ? std::sqrt(var / static_cast<double>(tail_count)) : 0.0;
    double const cv = (std::abs(mean) > 1e-6) ? (stddev / std::abs(mean)) : 0.0;

    if (window.front().throttle - window.back().throttle > 30.0) {
        // User let off mid-event.
        ev.quality = StepQuality::Aborted;
    } else if (ev.overshoot_pct > cfg.spike_overshoot_threshold_pct) {
        ev.quality = StepQuality::Spiked;
    } else if (cv > cfg.noise_cv_threshold) {
        ev.quality = StepQuality::Choppy;
    } else if (t_90 < 0.0
               || (ev.end_timestamp - ev.start_timestamp)
                  >= cfg.max_event_duration_s - 1e-3) {
        ev.quality = StepQuality::Slow;
    } else {
        ev.quality = StepQuality::Good;
    }

    return ev;
}

void make_suggestions(BoostSnapshot &snap, DetectorConfig const &cfg) {
    if (snap.good_event_count == 0) {
        snap.gain_suggestions.emplace_back(
            "No good-quality tip-in events detected — collect more "
            "clean WOT logs before evaluating PID gains.");
        return;
    }
    if (snap.median_overshoot_pct > cfg.overshoot_warn_pct) {
        snap.gain_suggestions.emplace_back(
            "Overshoot is high (median " +
            std::to_string(snap.median_overshoot_pct).substr(0, 5) +
            " %). Consider reducing proportional gain (Kp) by 10-15%, "
            "or increasing derivative gain (Kd) if the response is "
            "already fast enough.");
    }
    if (snap.median_rise_time_s > cfg.slow_rise_threshold_s) {
        snap.gain_suggestions.emplace_back(
            "Rise time is slow (median " +
            std::to_string(snap.median_rise_time_s).substr(0, 5) +
            " s). Consider increasing proportional gain (Kp); watch "
            "for added overshoot on the next pass.");
    }
    if (std::abs(snap.mean_steady_state_error)
        > cfg.steady_state_error_threshold) {
        if (snap.mean_steady_state_error < 0.0) {
            snap.gain_suggestions.emplace_back(
                "Steady-state undershoot (mean " +
                std::to_string(snap.mean_steady_state_error).substr(0, 6) +
                "). Consider increasing integral gain (Ki), or check "
                "the wastegate-duty base map for a low baseline.");
        } else {
            snap.gain_suggestions.emplace_back(
                "Steady-state overshoot (mean +" +
                std::to_string(snap.mean_steady_state_error).substr(0, 5) +
                "). Consider reducing integral gain (Ki), or check "
                "the wastegate-duty base map for a high baseline.");
        }
    }
    if (snap.gain_suggestions.empty()) {
        snap.gain_suggestions.emplace_back(
            "Boost response is within configured thresholds. No "
            "immediate gain-table adjustments suggested.");
    }
    snap.gain_suggestions.emplace_back(
        "These are starting points — verify on a dyno or controlled "
        "road test before driving aggressively.");
}

std::size_t max_referenced_column(PidMapping const &m) noexcept {
    std::size_t lo = 0;
    auto const  consider = [&](std::size_t v) {
        if (v != kNoColumn && v + 1 > lo) lo = v + 1;
    };
    consider(m.timestamp_idx);
    consider(m.target_boost_idx);
    consider(m.actual_boost_idx);
    consider(m.wgdc_idx);
    consider(m.throttle_idx);
    consider(m.rpm_idx);
    return lo;
}

}  // namespace

BoostSnapshot snapshot_from_samples(std::span<double const>  samples_row_major,
                                    std::size_t              pid_count,
                                    PidMapping const        &mapping,
                                    DetectorConfig const    &cfg) {
    BoostSnapshot snap{};
    if (pid_count == 0
        || samples_row_major.empty()
        || !has_col(mapping.timestamp_idx)
        || !has_col(mapping.target_boost_idx)
        || !has_col(mapping.actual_boost_idx)
        || !has_col(mapping.throttle_idx)) {
        return snap;
    }
    auto const rows = materialize(samples_row_major, pid_count, mapping,
                                   cfg.timestamp_unit);
    snap.samples_considered = rows.size();
    if (rows.size() < 4) {
        return snap;
    }

    // Detect tip-in events. A tip-in is identified by a backward-looking
    // window of ~0.5 s in which BOTH throttle rose by
    // throttle_step_threshold_pct AND target boost rose by
    // target_boost_step_threshold. The event itself starts at the
    // lookback row (the pre-step moment) and runs forward until
    // max_event_duration_s elapses or the next tip-in begins.
    for (std::size_t i = 1; i < rows.size(); ++i) {
        std::size_t lookback = i;
        while (lookback > 0
               && rows[i].t - rows[lookback].t < 0.5) {
            --lookback;
        }
        if (lookback == i) continue;  // sample cadence too coarse here

        double const throttle_rise = rows[i].throttle - rows[lookback].throttle;
        if (throttle_rise < cfg.throttle_step_threshold_pct) continue;

        double const target_rise = rows[i].target - rows[lookback].target;
        if (target_rise < cfg.target_boost_step_threshold) continue;

        // Walk forward until max_event_duration_s elapses OR the user
        // lifts off (throttle drops 30+ pts below the post-step level).
        // The lift-detection only applies AFTER we're past the step
        // start row `i` — the lookback rows are pre-step and naturally
        // have low throttle.
        double const peak_throttle = rows[i].throttle;
        std::size_t end = lookback;
        while (end < rows.size()
               && rows[end].t - rows[lookback].t < cfg.max_event_duration_s) {
            if (end > i && rows[end].throttle < peak_throttle - 30.0) break;
            ++end;
        }
        if (end <= lookback + 2) {
            ++i;
            continue;
        }
        if (rows[end - 1].t - rows[lookback].t < cfg.min_event_duration_s) {
            i = end;
            continue;
        }

        TipInEvent const ev = characterize(
            std::span<Row const>{rows.data() + lookback, rows.data() + end},
            cfg);
        snap.events.push_back(ev);
        snap.total_event_count++;
        if (ev.quality == StepQuality::Good) {
            snap.good_event_count++;
        }
        i = end;
    }

    // Aggregate over Good events.
    std::vector<double> rise_times, overshoots, settling_times;
    double sse_sum = 0.0;
    std::uint32_t sse_count = 0;
    for (auto const &ev : snap.events) {
        if (ev.quality != StepQuality::Good) continue;
        if (ev.rise_time_s > 0.0)       rise_times.push_back(ev.rise_time_s);
        if (ev.overshoot_pct > 0.0)     overshoots.push_back(ev.overshoot_pct);
        if (ev.settling_time_s > 0.0)   settling_times.push_back(ev.settling_time_s);
        sse_sum += ev.steady_state_error;
        sse_count++;
    }
    snap.median_rise_time_s     = median_of(rise_times);
    snap.median_overshoot_pct   = median_of(overshoots);
    snap.median_settling_time_s = median_of(settling_times);
    snap.mean_steady_state_error =
        (sse_count > 0) ? sse_sum / static_cast<double>(sse_count) : 0.0;

    make_suggestions(snap, cfg);
    return snap;
}

st::Result<BoostSnapshot> snapshot_from_csv(std::string_view        csv_path,
                                            PidMapping const       &mapping,
                                            DetectorConfig const   &cfg) {
    if (!has_col(mapping.timestamp_idx)
        || !has_col(mapping.target_boost_idx)
        || !has_col(mapping.actual_boost_idx)
        || !has_col(mapping.throttle_idx)) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"ebcs: mapping requires timestamp, "
                                       "target_boost, actual_boost, and "
                                       "throttle columns"});
    }

    std::ifstream f{std::string{csv_path}};
    if (!f) {
        return st::failure(st::ErrorCode::FileNotFound,
                           std::string{"ebcs: cannot open '"}
                               + std::string{csv_path} + "'");
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string const text = std::move(buf).str();

    auto const lines = st::core::csv::split_lines(text);
    if (lines.size() < 2) {
        return st::failure(st::ErrorCode::ParseError,
                           std::string{"ebcs: CSV needs a header row and at "
                                       "least one data row"});
    }
    auto const  header_fields = st::core::csv::split_fields(lines[0]);
    std::size_t const pid_count = header_fields.size();
    if (pid_count == 0) {
        return st::failure(st::ErrorCode::ParseError,
                           std::string{"ebcs: CSV header is empty"});
    }
    std::size_t const needed = max_referenced_column(mapping);
    if (needed > pid_count) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           std::string{"ebcs: mapping references column "
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
                               std::string{"ebcs: CSV row "}
                                   + std::to_string(row + 1) + " has "
                                   + std::to_string(fields.size())
                                   + " fields, expected "
                                   + std::to_string(pid_count));
        }
        for (std::size_t col = 0; col < pid_count; ++col) {
            double v = 0.0;
            if (!st::core::csv::parse_double(fields[col], v)) {
                return st::failure(st::ErrorCode::ParseError,
                                   std::string{"ebcs: CSV row "}
                                       + std::to_string(row + 1) + " col "
                                       + std::to_string(col + 1)
                                       + ": not a number");
            }
            samples.push_back(v);
        }
    }

    return snapshot_from_samples(samples, pid_count, mapping, cfg);
}

}  // namespace st::log::ebcs
