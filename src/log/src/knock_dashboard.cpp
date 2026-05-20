// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/knock_dashboard.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace st::log::knock {

namespace {

inline bool has_pid(std::size_t idx) noexcept {
    return idx != kNoPid;
}

inline double row_value(std::span<double const> samples_row_major,
                        std::size_t              pid_count,
                        std::size_t              row,
                        std::size_t              pid_idx) noexcept {
    return samples_row_major[row * pid_count + pid_idx];
}

inline bool sample_passes_gate(std::span<double const> samples_row_major,
                               std::size_t              pid_count,
                               std::size_t              row,
                               PidMapping const        &mapping,
                               WindowConfig const      &cfg) noexcept {
    if (!cfg.require_load_gate) {
        return true;
    }
    if (has_pid(mapping.rpm_idx)
        && row_value(samples_row_major, pid_count, row, mapping.rpm_idx)
               < cfg.min_rpm) {
        return false;
    }
    if (has_pid(mapping.load_idx)
        && row_value(samples_row_major, pid_count, row, mapping.load_idx)
               < cfg.min_load) {
        return false;
    }
    return true;
}

}  // namespace

KnockSnapshot snapshot_from_samples(std::span<double const>  samples_row_major,
                                    std::size_t              pid_count,
                                    PidMapping const        &mapping,
                                    WindowConfig const      &cfg) {
    KnockSnapshot snap{};
    snap.cylinder_count = std::min(mapping.cylinder_count, kMaxCylinders);
    snap.window_seconds = cfg.window_seconds;

    if (pid_count == 0 || samples_row_major.empty()) {
        return snap;
    }

    std::size_t const n_rows = samples_row_major.size() / pid_count;
    if (n_rows == 0) {
        return snap;
    }

    // Window length in samples — keep at most window_seconds * rate samples
    // from the tail of the buffer for aggregation.
    std::size_t const window_rows = (cfg.sample_rate_hz > 0.0)
        ? static_cast<std::size_t>(cfg.sample_rate_hz * cfg.window_seconds)
        : n_rows;
    std::size_t const start_row = (n_rows > window_rows)
        ? (n_rows - window_rows)
        : 0;

    // Per-cyl accumulators
    struct Acc {
        double        sum_flkc{0.0};
        double        min_flkc{0.0};
        double        last_flkc{0.0};
        double        last_fbkc{0.0};
        std::uint32_t count{0};
        std::uint32_t event_count{0};
        bool          has_min{false};
    };
    std::array<Acc, kMaxCylinders> acc{};

    for (std::size_t row = start_row; row < n_rows; ++row) {
        bool const passed = sample_passes_gate(
            samples_row_major, pid_count, row, mapping, cfg);
        if (!passed) {
            snap.samples_gated_out++;
            continue;
        }
        snap.samples_considered++;

        for (std::uint8_t c = 0; c < snap.cylinder_count; ++c) {
            auto const flk_idx  = mapping.fine_knock_learn[c];
            auto const fbkc_idx = mapping.feedback_knock[c];

            if (has_pid(flk_idx)) {
                double const v = row_value(samples_row_major, pid_count, row,
                                           flk_idx);
                acc[c].sum_flkc  += v;
                acc[c].last_flkc  = v;
                if (!acc[c].has_min || v < acc[c].min_flkc) {
                    acc[c].min_flkc = v;
                    acc[c].has_min  = true;
                }
                snap.per_cyl[c].strip_flkc.push_back(v);
                if (snap.per_cyl[c].strip_flkc.size() > snap.strip_capacity) {
                    snap.per_cyl[c].strip_flkc.erase(
                        snap.per_cyl[c].strip_flkc.begin());
                }
                acc[c].count++;
            }
            if (has_pid(fbkc_idx)) {
                double const v = row_value(samples_row_major, pid_count, row,
                                           fbkc_idx);
                acc[c].last_fbkc = v;
                if (v < 0.0) {
                    acc[c].event_count++;
                }
                snap.per_cyl[c].strip_fbkc.push_back(v);
                if (snap.per_cyl[c].strip_fbkc.size() > snap.strip_capacity) {
                    snap.per_cyl[c].strip_fbkc.erase(
                        snap.per_cyl[c].strip_fbkc.begin());
                }
            }
        }
    }

    // Per-cylinder rollups
    double all_cyl_mean_sum    = 0.0;
    std::uint8_t cyls_with_data = 0;
    for (std::uint8_t c = 0; c < snap.cylinder_count; ++c) {
        auto &out = snap.per_cyl[c];
        out.current_flkc = acc[c].last_flkc;
        out.current_fbkc = acc[c].last_fbkc;
        if (acc[c].count > 0) {
            out.mean_flkc_window = acc[c].sum_flkc
                                   / static_cast<double>(acc[c].count);
            out.min_flkc_window  = acc[c].has_min ? acc[c].min_flkc : 0.0;
            all_cyl_mean_sum    += out.mean_flkc_window;
            cyls_with_data++;
        }
        out.event_count_window = acc[c].event_count;
    }
    if (cyls_with_data > 0) {
        double const all_mean = all_cyl_mean_sum
                                / static_cast<double>(cyls_with_data);
        for (std::uint8_t c = 0; c < snap.cylinder_count; ++c) {
            if (acc[c].count > 0) {
                snap.per_cyl[c].delta_from_cyl_mean =
                    snap.per_cyl[c].mean_flkc_window - all_mean;
            }
        }
    }

    return snap;
}

st::Result<KnockSnapshot> snapshot_from_csv(std::string_view /*csv_path*/,
                                            PidMapping const & /*mapping*/,
                                            WindowConfig const & /*cfg*/) {
    // CSV path lands when a shared CSV reader is factored out from
    // st::autotune. Tracked under docs/05 §11; until then the in-memory
    // snapshot_from_samples() is the supported entry point.
    return st::failure(st::ErrorCode::NotImplemented,
                       std::string{"st::log::knock::snapshot_from_csv: "
                                   "CSV reader not yet implemented"});
}

}  // namespace st::log::knock
