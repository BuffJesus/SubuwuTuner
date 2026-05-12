// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_EDIT_HPP
#define ST_EDIT_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"

#include <cstddef>

namespace st::edit {

// Inclusive rectangular selection over a TableData's values grid.
// For a 1D table (one row), set r_start = r_end = 0.
struct Rect {
    std::size_t r_start{0};
    std::size_t r_end{0};
    std::size_t c_start{0};
    std::size_t c_end{0};

    [[nodiscard]] constexpr std::size_t rows() const noexcept {
        return r_end >= r_start ? r_end - r_start + 1 : 0;
    }

    [[nodiscard]] constexpr std::size_t cols() const noexcept {
        return c_end >= c_start ? c_end - c_start + 1 : 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return rows() == 0 || cols() == 0; }
};

// Build a Rect covering every cell in the table. Convenient for "edit the
// whole table" operations.
[[nodiscard]] Rect whole_table(Definition::TableData const &td) noexcept;

// ---- Cell operations ----------------------------------------------------
//
// Every operation:
//   * mutates `td.values` in place
//   * returns OutOfRange if the rect would step past the grid
//   * returns InvalidArgument if the rect is empty (start > end)
//
// Operations work in *scaled engineering units*. Converting back to raw bytes
// for ROM writeback is the responsibility of a future projection layer.

[[nodiscard]] Status set_cells(Definition::TableData &td, Rect r, double value);
[[nodiscard]] Status add_cells(Definition::TableData &td, Rect r, double delta);
[[nodiscard]] Status multiply_cells(Definition::TableData &td, Rect r, double factor);

// Multiply every cell in the rect by (1 + pct/100). pct=10 means "+10%".
[[nodiscard]] Status percent_scale_cells(Definition::TableData &td, Rect r, double pct);

// Box-blur the selection: each cell becomes the average of its and its
// neighbors' values, clamped to the selection's edges. Repeat `iterations`
// times. Pure smoothing — does not change cells outside the rect.
[[nodiscard]] Status smooth_cells(Definition::TableData &td, Rect r, int iterations = 1);

// Bilinear interpolation across the rect using the rect's corners as anchors.
// For a 1D selection (rows() == 1), this is plain linear interpolation
// between the two endpoint columns; same when cols() == 1. For a single cell
// (rows() == cols() == 1), this is a no-op.
[[nodiscard]] Status interpolate_cells(Definition::TableData &td, Rect r);

} // namespace st::edit

#endif // ST_EDIT_HPP
