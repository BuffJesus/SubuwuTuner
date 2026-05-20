// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/log/adaptive_history.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

using namespace st::log::adaptive;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<double> flatten(std::initializer_list<std::initializer_list<double>> rows) {
    std::vector<double> out;
    for (auto const &row : rows) {
        for (double v : row)
            out.push_back(v);
    }
    return out;
}

ColumnMapping basic_mapping() {
    ColumnMapping m{};
    // ts, ltft, dam, idleadapt
    m.timestamp_idx = 0;
    m.signal_idx[0] = 1; // Ltft
    m.signal_idx[1] = 2; // Dam
    m.signal_idx[2] = 3; // IdleAdapt
    return m;
}

BucketConfig daily_unix_cfg() {
    BucketConfig c{};
    c.bucket_seconds = 86400.0;
    c.timestamp_unit = TimestampUnit::UnixSeconds;
    c.min_samples_per_bucket = 0;
    return c;
}

} // namespace

TEST_CASE("snapshot_from_samples returns empty on empty input", "[adaptive][snapshot]") {
    auto const s = snapshot_from_samples({}, 4, basic_mapping(), daily_unix_cfg());
    REQUIRE(s.total_samples == 0);
    REQUIRE(s.series[0].has_data == false);
    REQUIRE(s.series[0].points.empty());
}

TEST_CASE("snapshot_from_samples buckets by day at default 86400 s",
          "[adaptive][snapshot][bucket]") {
    // 3 samples in day-0 + 2 samples in day-1 (day boundary at 86400).
    auto const samples = flatten({
        // ts(s),  ltft, dam, iac
        {0.0, 1.0, 1.0, 0.0},
        {3600.0, 3.0, 1.0, 0.0},
        {86399.0, 5.0, 1.0, 0.0},
        {86400.0, 9.0, 1.0, 0.0},
        {172700.0, 11.0, 1.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 4, basic_mapping(), daily_unix_cfg());
    auto const &ltft = s.series[0];
    REQUIRE(ltft.points.size() == 2);
    REQUIRE(ltft.points[0].count == 3);
    REQUIRE(ltft.points[1].count == 2);
    REQUIRE_THAT(ltft.points[0].mean, WithinAbs(3.0, 1e-9));  // (1+3+5)/3
    REQUIRE_THAT(ltft.points[1].mean, WithinAbs(10.0, 1e-9)); // (9+11)/2
}

TEST_CASE("snapshot_from_samples computes drift_per_second slope", "[adaptive][snapshot][drift]") {
    // LTFT growing linearly with time. Two daily buckets at midpoints
    // 43200 and 129600. Means 0 and 10. Slope = 10 / 86400.
    auto const samples = flatten({
        {0.0, 0.0, 1.0, 0.0},
        {86400.0, 10.0, 1.0, 0.0},
    });
    BucketConfig cfg = daily_unix_cfg();
    auto const s = snapshot_from_samples(samples, 4, basic_mapping(), cfg);
    REQUIRE(s.series[0].points.size() == 2);
    REQUIRE_THAT(s.series[0].drift_per_second, WithinAbs(10.0 / 86400.0, 1e-9));
}

TEST_CASE("snapshot_from_samples respects min_samples_per_bucket", "[adaptive][snapshot][filter]") {
    auto const samples = flatten({
        // Day 0: 3 samples — passes filter (count == 3)
        {0.0, 1.0, 1.0, 0.0},
        {3600.0, 3.0, 1.0, 0.0},
        {7200.0, 5.0, 1.0, 0.0},
        // Day 1: 1 sample — fails filter
        {86400.0, 9.0, 1.0, 0.0},
    });
    BucketConfig cfg = daily_unix_cfg();
    cfg.min_samples_per_bucket = 2;
    auto const s = snapshot_from_samples(samples, 4, basic_mapping(), cfg);
    REQUIRE(s.series[0].points.size() == 1);
    REQUIRE(s.series[0].points[0].count == 3);
}

TEST_CASE("snapshot_from_samples normalizes UnixMillis timestamps", "[adaptive][snapshot][units]") {
    // Same data as the daily-bucket test but timestamps in ms.
    auto const samples = flatten({
        {0.0, 1.0, 1.0, 0.0},
        {3600000.0, 3.0, 1.0, 0.0},
        {86400000.0, 9.0, 1.0, 0.0},
    });
    BucketConfig cfg = daily_unix_cfg();
    cfg.timestamp_unit = TimestampUnit::UnixMillis;
    auto const s = snapshot_from_samples(samples, 4, basic_mapping(), cfg);
    REQUIRE(s.series[0].points.size() == 2);
}

TEST_CASE("snapshot_from_samples emits one point per row when bucket_seconds=0",
          "[adaptive][snapshot][unbucketed]") {
    auto const samples = flatten({
        {1.0, 1.5, 1.0, 0.0},
        {2.0, 2.5, 1.0, 0.0},
        {3.0, 3.5, 1.0, 0.0},
    });
    BucketConfig cfg = daily_unix_cfg();
    cfg.bucket_seconds = 0.0;
    auto const s = snapshot_from_samples(samples, 4, basic_mapping(), cfg);
    REQUIRE(s.series[0].points.size() == 3);
    REQUIRE_THAT(s.series[0].points[0].mean, WithinAbs(1.5, 1e-9));
    REQUIRE_THAT(s.series[0].points[2].mean, WithinAbs(3.5, 1e-9));
}

TEST_CASE("snapshot_from_samples skips signals not in mapping", "[adaptive][snapshot][missing]") {
    ColumnMapping m = basic_mapping();
    m.signal_idx[1] = kNoColumn; // No Dam in this log
    m.signal_idx[2] = kNoColumn; // No IdleAdapt
    auto const samples = flatten({
        {0.0, 2.0, 1.0, 0.0},
        {86400.0, 4.0, 1.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 4, m, daily_unix_cfg());
    REQUIRE(s.series[0].has_data == true);
    REQUIRE(s.series[1].has_data == false);
    REQUIRE(s.series[2].has_data == false);
}

TEST_CASE("snapshot_from_samples records time-span fields", "[adaptive][snapshot][span]") {
    auto const samples = flatten({
        {100.0, 1.0, 1.0, 0.0},
        {1100.0, 2.0, 1.0, 0.0},
        {5000.0, 3.0, 1.0, 0.0},
    });
    auto const s = snapshot_from_samples(samples, 4, basic_mapping(), daily_unix_cfg());
    REQUIRE_THAT(s.earliest_timestamp, WithinAbs(100.0, 1e-9));
    REQUIRE_THAT(s.latest_timestamp, WithinAbs(5000.0, 1e-9));
    REQUIRE_THAT(s.time_span_seconds, WithinAbs(4900.0, 1e-9));
}

TEST_CASE("snapshot_from_csv reports FileNotFound on missing path", "[adaptive][csv]") {
    auto const r = snapshot_from_csv("/definitely/missing.csv", basic_mapping(), daily_unix_cfg());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("snapshot_from_csv rejects mapping with no timestamp column",
          "[adaptive][csv][validate]") {
    ColumnMapping m = basic_mapping();
    m.timestamp_idx = kNoColumn;
    auto const r = snapshot_from_csv("/tmp/whatever.csv", m, daily_unix_cfg());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("snapshot_from_csv rejects mapping with no signal columns", "[adaptive][csv][validate]") {
    ColumnMapping m = basic_mapping();
    for (std::size_t i = 0; i < kSignalCount; ++i)
        m.signal_idx[i] = kNoColumn;
    auto const r = snapshot_from_csv("/tmp/whatever.csv", m, daily_unix_cfg());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("snapshot_from_csv reads a small log", "[adaptive][csv][roundtrip]") {
    auto const path = std::filesystem::temp_directory_path() / "subuwutuner_test_adaptive.csv";
    {
        std::ofstream of{path};
        of << "ts,ltft,dam,iac\n"
              "0,1.0,1.0,0.0\n"
              "3600,3.0,1.0,0.0\n"
              "86400,9.0,0.95,0.5\n";
    }
    auto const r = snapshot_from_csv(path.string(), basic_mapping(), daily_unix_cfg());
    REQUIRE(r.has_value());
    REQUIRE(r->total_samples == 9u); // 3 rows x 3 signals
    REQUIRE(r->series[0].points.size() == 2);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
