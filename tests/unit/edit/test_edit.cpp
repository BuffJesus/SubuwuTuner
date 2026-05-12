// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

st::Definition::TableData make_grid(std::vector<std::vector<double>> values) {
    st::Definition::TableData td;
    td.values = std::move(values);
    return td;
}

constexpr double kEps = 1e-9;

bool eq(double a, double b) {
    return (a > b ? a - b : b - a) < kEps;
}

} // namespace

TEST_CASE("whole_table covers every cell", "[edit][rect]") {
    auto const     td   = make_grid({{1, 2, 3}, {4, 5, 6}});
    auto const     rect = st::edit::whole_table(td);
    REQUIRE(rect.r_start == 0);
    REQUIRE(rect.r_end == 1);
    REQUIRE(rect.c_start == 0);
    REQUIRE(rect.c_end == 2);
    REQUIRE(rect.rows() == 2);
    REQUIRE(rect.cols() == 3);
}

TEST_CASE("whole_table on an empty TableData returns an empty rect",
          "[edit][rect]") {
    st::Definition::TableData empty;
    auto const                rect = st::edit::whole_table(empty);
    REQUIRE(rect.empty());
}

TEST_CASE("set_cells overwrites only the selection", "[edit][set]") {
    auto       td = make_grid({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    auto const s  = st::edit::set_cells(td, {1, 2, 0, 1}, 0.0);
    REQUIRE(s.has_value());

    REQUIRE(td.values[0] == std::vector<double>{1, 2, 3}); // row 0 untouched
    REQUIRE(td.values[1] == std::vector<double>{0, 0, 6});
    REQUIRE(td.values[2] == std::vector<double>{0, 0, 9});
}

TEST_CASE("add_cells adds delta in selection only", "[edit][add]") {
    auto       td = make_grid({{1, 2, 3}, {4, 5, 6}});
    auto const s  = st::edit::add_cells(td, {0, 1, 1, 2}, 10.0);
    REQUIRE(s.has_value());

    REQUIRE(td.values[0] == std::vector<double>{1, 12, 13});
    REQUIRE(td.values[1] == std::vector<double>{4, 15, 16});
}

TEST_CASE("multiply_cells and percent_scale_cells", "[edit][multiply]") {
    auto       td = make_grid({{2, 4}, {6, 8}});
    auto const s  = st::edit::multiply_cells(td, {0, 1, 0, 1}, 0.5);
    REQUIRE(s.has_value());
    REQUIRE(td.values[0] == std::vector<double>{1, 2});
    REQUIRE(td.values[1] == std::vector<double>{3, 4});

    auto td2     = make_grid({{100, 200}});
    auto const t = st::edit::percent_scale_cells(td2, {0, 0, 0, 1}, 10.0);
    REQUIRE(t.has_value());
    REQUIRE(eq(td2.values[0][0], 110.0));
    REQUIRE(eq(td2.values[0][1], 220.0));
}

TEST_CASE("set_cells rejects an empty or inverted rect",
          "[edit][validation]") {
    auto td = make_grid({{1, 2}, {3, 4}});
    // r_start > r_end
    auto s = st::edit::set_cells(td, {2, 1, 0, 0}, 99.0);
    REQUIRE_FALSE(s.has_value());
    REQUIRE(s.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("set_cells rejects an out-of-bounds rect", "[edit][validation]") {
    auto td = make_grid({{1, 2}, {3, 4}});
    auto s  = st::edit::set_cells(td, {0, 5, 0, 0}, 99.0);
    REQUIRE_FALSE(s.has_value());
    REQUIRE(s.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("smooth_cells applies a single-pass box blur clamped to selection",
          "[edit][smooth]") {
    // 3x3 grid; box-blur the middle 3x3 (== whole grid) with one iteration.
    // Each cell becomes the average of its 3x3 neighborhood (with smaller
    // neighborhoods at edges). For a 3x3 grid:
    //   center [1][1] sees all 9 cells -> mean = (1+2+3+4+5+6+7+8+9)/9 = 5
    //   corner [0][0] sees its 2x2 quadrant -> (1+2+4+5)/4 = 3
    //   edge   [0][1] sees its 2x3 top half -> (1+2+3+4+5+6)/6 = 3.5
    auto       td = make_grid({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    auto const s  = st::edit::smooth_cells(td, st::edit::whole_table(td), 1);
    REQUIRE(s.has_value());

    REQUIRE(eq(td.values[0][0], 3.0));
    REQUIRE(eq(td.values[0][1], 3.5));
    REQUIRE(eq(td.values[1][1], 5.0));
    REQUIRE(eq(td.values[2][2], 7.0));
}

TEST_CASE("smooth_cells leaves cells outside the selection alone",
          "[edit][smooth]") {
    auto       td = make_grid({{1, 1, 1}, {1, 100, 1}, {1, 1, 1}});
    auto const s  = st::edit::smooth_cells(td, {1, 1, 1, 1}, 1);
    REQUIRE(s.has_value());

    // Single-cell selection: neighborhood is the cell itself, so unchanged.
    REQUIRE(eq(td.values[1][1], 100.0));

    // All other cells untouched.
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            if (i == 1 && j == 1) continue;
            REQUIRE(eq(td.values[i][j], 1.0));
        }
    }
}

TEST_CASE("interpolate_cells fills 1D selection linearly", "[edit][interpolate]") {
    auto       td = make_grid({{0.0, 99.0, 99.0, 99.0, 100.0}});
    auto const s  = st::edit::interpolate_cells(td, {0, 0, 0, 4});
    REQUIRE(s.has_value());

    REQUIRE(eq(td.values[0][0], 0.0));
    REQUIRE(eq(td.values[0][1], 25.0));
    REQUIRE(eq(td.values[0][2], 50.0));
    REQUIRE(eq(td.values[0][3], 75.0));
    REQUIRE(eq(td.values[0][4], 100.0));
}

TEST_CASE("interpolate_cells does bilinear from the 4 corners",
          "[edit][interpolate]") {
    // Corners: (0,0)=0, (0,2)=20, (2,0)=10, (2,2)=30. Interior should be
    // bilinearly interpolated.
    auto td = make_grid({
        {0,   0,   20},
        {0,   0,   0},
        {10,  0,   30},
    });
    auto const s = st::edit::interpolate_cells(td, st::edit::whole_table(td));
    REQUIRE(s.has_value());

    // Center: avg of all corners = (0+20+10+30)/4 = 15
    REQUIRE(eq(td.values[1][1], 15.0));
    // Mid-top edge: (0+20)/2 = 10
    REQUIRE(eq(td.values[0][1], 10.0));
    // Mid-left edge: (0+10)/2 = 5
    REQUIRE(eq(td.values[1][0], 5.0));
    // Mid-right: (20+30)/2 = 25
    REQUIRE(eq(td.values[1][2], 25.0));
    // Mid-bottom: (10+30)/2 = 20
    REQUIRE(eq(td.values[2][1], 20.0));
    // Corners must remain unchanged.
    REQUIRE(eq(td.values[0][0], 0.0));
    REQUIRE(eq(td.values[0][2], 20.0));
    REQUIRE(eq(td.values[2][0], 10.0));
    REQUIRE(eq(td.values[2][2], 30.0));
}

TEST_CASE("interpolate_cells is a no-op on a single-cell rect",
          "[edit][interpolate]") {
    auto       td   = make_grid({{1, 2, 3}, {4, 5, 6}});
    auto const orig = td.values;
    auto const s    = st::edit::interpolate_cells(td, {0, 0, 0, 0});
    REQUIRE(s.has_value());
    REQUIRE(td.values == orig);
}
