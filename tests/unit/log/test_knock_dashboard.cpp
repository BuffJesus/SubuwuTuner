// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/knock_dashboard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

using namespace st::log::knock;
using Catch::Matchers::WithinAbs;

namespace {

// Build a row-major (n_rows × pid_count) buffer from a list of rows.
std::vector<double> flatten(std::initializer_list<std::initializer_list<double>> rows) {
    std::vector<double> out;
    for (auto const &row : rows) {
        for (double v : row) {
            out.push_back(v);
        }
    }
    return out;
}

// Simple H4 mapping: 6 PIDs — rpm, load, flkc1..4
PidMapping h4_mapping() {
    PidMapping m{};
    m.cylinder_count = 4;
    m.rpm_idx = 0;
    m.load_idx = 1;
    m.fine_knock_learn = {2, 3, 4, 5, kNoPid, kNoPid};
    m.feedback_knock = {kNoPid, kNoPid, kNoPid, kNoPid, kNoPid, kNoPid};
    return m;
}

WindowConfig default_cfg() {
    WindowConfig c{};
    c.window_seconds = 10.0;
    c.sample_rate_hz = 20.0;
    c.min_rpm = 1500.0;
    c.min_load = 1.5;
    c.require_load_gate = true;
    return c;
}

} // namespace

TEST_CASE("snapshot_from_samples returns empty on empty input", "[knock][snapshot]") {
    PidMapping const m = h4_mapping();
    WindowConfig const c = default_cfg();
    auto const s = snapshot_from_samples({}, 6, m, c);
    REQUIRE(s.cylinder_count == 4);
    REQUIRE(s.samples_considered == 0);
    REQUIRE(s.samples_gated_out == 0);
    REQUIRE(s.per_cyl[0].strip_flkc.empty());
}

TEST_CASE("snapshot_from_samples gates out samples below min_rpm", "[knock][snapshot][gate]") {
    PidMapping const m = h4_mapping();
    WindowConfig const c = default_cfg();
    // 4 rows; the first two are below min_rpm (1500) and gated out.
    auto const samples = flatten({
        // rpm,  load,  flkc1, flkc2, flkc3, flkc4
        {800.0, 2.0, -1.0, -1.0, -1.0, -1.0},
        {1400.0, 2.0, -1.0, -1.0, -1.0, -1.0},
        {2000.0, 2.0, -2.0, -3.0, -1.0, 0.0},
        {3000.0, 2.0, -4.0, -3.0, -2.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 6, m, c);
    REQUIRE(s.samples_gated_out == 2);
    REQUIRE(s.samples_considered == 2);
    // cyl 0: window samples -2, -4 → mean -3, min -4
    REQUIRE_THAT(s.per_cyl[0].mean_flkc_window, WithinAbs(-3.0, 1e-9));
    REQUIRE_THAT(s.per_cyl[0].min_flkc_window, WithinAbs(-4.0, 1e-9));
    // current = last sample
    REQUIRE_THAT(s.per_cyl[0].current_flkc, WithinAbs(-4.0, 1e-9));
    // cyl 3: window samples 0, 0 → mean 0
    REQUIRE_THAT(s.per_cyl[3].mean_flkc_window, WithinAbs(0.0, 1e-9));
}

TEST_CASE("snapshot_from_samples computes delta from all-cyl mean", "[knock][snapshot][delta]") {
    PidMapping const m = h4_mapping();
    WindowConfig const c = default_cfg();
    auto const samples = flatten({
        // rpm,  load,  flkc1, flkc2, flkc3, flkc4
        {2500.0, 2.0, -4.0, -2.0, 0.0, 0.0},
        {2500.0, 2.0, -4.0, -2.0, 0.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 6, m, c);
    // means: -4, -2, 0, 0 → grand mean = -1.5
    // deltas: -2.5, -0.5, +1.5, +1.5
    REQUIRE_THAT(s.per_cyl[0].delta_from_cyl_mean, WithinAbs(-2.5, 1e-9));
    REQUIRE_THAT(s.per_cyl[1].delta_from_cyl_mean, WithinAbs(-0.5, 1e-9));
    REQUIRE_THAT(s.per_cyl[2].delta_from_cyl_mean, WithinAbs(1.5, 1e-9));
    REQUIRE_THAT(s.per_cyl[3].delta_from_cyl_mean, WithinAbs(1.5, 1e-9));
}

TEST_CASE("snapshot_from_samples counts negative FBKC as knock events",
          "[knock][snapshot][events]") {
    PidMapping m = h4_mapping();
    m.feedback_knock = {2, 3, 4, 5, kNoPid, kNoPid};
    m.fine_knock_learn = {kNoPid, kNoPid, kNoPid, kNoPid, kNoPid, kNoPid};
    WindowConfig const c = default_cfg();
    // 5 rows above gate, fbkc cycles -2.5, 0, -1, 0, -3 on cyl 0.
    auto const samples = flatten({
        {2500.0, 2.0, -2.5, 0.0, 0.0, 0.0},
        {2500.0, 2.0, 0.0, 0.0, 0.0, 0.0},
        {2500.0, 2.0, -1.0, 0.0, 0.0, 0.0},
        {2500.0, 2.0, 0.0, 0.0, 0.0, 0.0},
        {2500.0, 2.0, -3.0, 0.0, 0.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 6, m, c);
    REQUIRE(s.per_cyl[0].event_count_window == 3u);
    REQUIRE(s.per_cyl[1].event_count_window == 0u);
}

TEST_CASE("snapshot_from_samples supports H6 cylinder count", "[knock][snapshot][h6]") {
    PidMapping m{};
    m.cylinder_count = 6;
    m.rpm_idx = 0;
    m.load_idx = 1;
    // 8 PIDs total: rpm, load, flkc1..6
    m.fine_knock_learn = {2, 3, 4, 5, 6, 7};
    WindowConfig const c = default_cfg();
    auto const samples = flatten({
        {2500.0, 2.0, -1.0, -2.0, -3.0, -4.0, -5.0, -6.0},
        {2500.0, 2.0, -1.0, -2.0, -3.0, -4.0, -5.0, -6.0},
    });
    auto const s = snapshot_from_samples(samples, 8, m, c);
    REQUIRE(s.cylinder_count == 6);
    REQUIRE_THAT(s.per_cyl[0].mean_flkc_window, WithinAbs(-1.0, 1e-9));
    REQUIRE_THAT(s.per_cyl[5].mean_flkc_window, WithinAbs(-6.0, 1e-9));
}

TEST_CASE("snapshot_from_samples disables gate when require_load_gate is false",
          "[knock][snapshot][nogate]") {
    PidMapping m = h4_mapping();
    WindowConfig c = default_cfg();
    c.require_load_gate = false;
    // All rows below min_rpm but should still be counted.
    auto const samples = flatten({
        {800.0, 0.0, -1.0, -1.0, -1.0, -1.0},
        {1000.0, 0.0, -2.0, -2.0, -2.0, -2.0},
    });
    auto const s = snapshot_from_samples(samples, 6, m, c);
    REQUIRE(s.samples_gated_out == 0);
    REQUIRE(s.samples_considered == 2);
}

TEST_CASE("snapshot_from_samples respects kNoPid for missing signals",
          "[knock][snapshot][missing]") {
    PidMapping m{};
    m.cylinder_count = 4;
    m.rpm_idx = 0;
    m.load_idx = 1;
    // Only cyl 0 and 2 have flkc; 1 and 3 are missing.
    m.fine_knock_learn = {2, kNoPid, 3, kNoPid, kNoPid, kNoPid};
    WindowConfig const c = default_cfg();
    auto const samples = flatten({
        {2500.0, 2.0, -1.5, -2.5},
        {2500.0, 2.0, -1.5, -2.5},
    });
    auto const s = snapshot_from_samples(samples, 4, m, c);
    REQUIRE_THAT(s.per_cyl[0].mean_flkc_window, WithinAbs(-1.5, 1e-9));
    REQUIRE_THAT(s.per_cyl[2].mean_flkc_window, WithinAbs(-2.5, 1e-9));
    REQUIRE(s.per_cyl[1].strip_flkc.empty());
    REQUIRE(s.per_cyl[3].strip_flkc.empty());
}

TEST_CASE("snapshot_from_csv reports FileNotFound on missing path", "[knock][snapshot][csv]") {
    PidMapping const m = h4_mapping();
    WindowConfig const c = default_cfg();
    auto const r = snapshot_from_csv("/definitely/does/not/exist.csv", m, c);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("snapshot_from_csv reads a small log", "[knock][snapshot][csv]") {
    // Write a tiny CSV to a temp path, then read it back.
    auto const path = std::filesystem::temp_directory_path() / "subuwutuner_test_knock.csv";
    {
        std::ofstream of{path};
        of << "rpm,load,flkc1,flkc2,flkc3,flkc4\n"
              "2500,2.5,-1.0,-2.0,0.0,0.0\n"
              "2500,2.5,-1.0,-2.0,0.0,0.0\n"
              "2500,2.5,-1.0,-2.0,0.0,0.0\n";
    }
    PidMapping const m = h4_mapping();
    WindowConfig const c = default_cfg();
    auto const r = snapshot_from_csv(path.string(), m, c);
    REQUIRE(r.has_value());
    REQUIRE(r->samples_considered == 3);
    REQUIRE_THAT(r->per_cyl[0].mean_flkc_window, WithinAbs(-1.0, 1e-9));
    REQUIRE_THAT(r->per_cyl[1].mean_flkc_window, WithinAbs(-2.0, 1e-9));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("snapshot_from_csv rejects a CSV with a column-count mismatch",
          "[knock][snapshot][csv]") {
    auto const path = std::filesystem::temp_directory_path() / "subuwutuner_test_knock_bad.csv";
    {
        std::ofstream of{path};
        // Header says 4 columns, but data row has 6 columns referenced
        // by h4_mapping (rpm=0, load=1, flkc1..4=2..5).
        of << "rpm,load,flkc1,flkc2\n"
              "2500,2.5,-1.0,-2.0\n";
    }
    PidMapping const m = h4_mapping();
    WindowConfig const c = default_cfg();
    auto const r = snapshot_from_csv(path.string(), m, c);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
