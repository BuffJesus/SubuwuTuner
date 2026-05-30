// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/edit.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/defs.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace st::edit {

namespace {

Status validate_rect(Definition::TableData const &td, Rect r) noexcept {
    if (td.values.empty() || td.values[0].empty()) {
        return failure(ErrorCode::OutOfRange, "edit: table has no cells");
    }
    if (r.r_start > r.r_end || r.c_start > r.c_end) {
        return failure(ErrorCode::InvalidArgument,
                       "edit: rect start exceeds end (rows " +
                           std::to_string(r.r_start) + ".." + std::to_string(r.r_end) +
                           ", cols " + std::to_string(r.c_start) + ".." +
                           std::to_string(r.c_end) + ")");
    }
    if (r.r_end >= td.values.size() || r.c_end >= td.values[0].size()) {
        return failure(ErrorCode::OutOfRange,
                       "edit: rect extends past table (rect rows " +
                           std::to_string(r.r_start) + ".." + std::to_string(r.r_end) +
                           ", cols " + std::to_string(r.c_start) + ".." +
                           std::to_string(r.c_end) + "; table is " +
                           std::to_string(td.values.size()) + "x" +
                           std::to_string(td.values[0].size()) + ")");
    }
    return ok();
}

} // namespace

Rect whole_table(Definition::TableData const &td) noexcept {
    if (td.values.empty() || td.values[0].empty()) {
        // Sentinel: r_start > r_end and c_start > c_end signals "no cells".
        return Rect{1, 0, 1, 0};
    }
    return Rect{0, td.values.size() - 1, 0, td.values[0].size() - 1};
}

Status set_cells(Definition::TableData &td, Rect r, double value) {
    if (auto vr = validate_rect(td, r); !vr.has_value())
        return vr;
    for (std::size_t row = r.r_start; row <= r.r_end; ++row) {
        for (std::size_t col = r.c_start; col <= r.c_end; ++col) {
            td.values[row][col] = value;
        }
    }
    return ok();
}

Status add_cells(Definition::TableData &td, Rect r, double delta) {
    if (auto vr = validate_rect(td, r); !vr.has_value())
        return vr;
    for (std::size_t row = r.r_start; row <= r.r_end; ++row) {
        for (std::size_t col = r.c_start; col <= r.c_end; ++col) {
            td.values[row][col] += delta;
        }
    }
    return ok();
}

Status multiply_cells(Definition::TableData &td, Rect r, double factor) {
    if (auto vr = validate_rect(td, r); !vr.has_value())
        return vr;
    for (std::size_t row = r.r_start; row <= r.r_end; ++row) {
        for (std::size_t col = r.c_start; col <= r.c_end; ++col) {
            td.values[row][col] *= factor;
        }
    }
    return ok();
}

Status percent_scale_cells(Definition::TableData &td, Rect r, double pct) {
    return multiply_cells(td, r, 1.0 + pct / 100.0);
}

Status smooth_cells(Definition::TableData &td, Rect r, int iterations) {
    if (auto vr = validate_rect(td, r); !vr.has_value())
        return vr;
    if (iterations <= 0)
        return ok();

    auto const rows = r.rows();
    auto const cols = r.cols();

    // Work on a local copy so neighbour reads aren't contaminated by writes
    // within the same iteration. The copy holds only the selection.
    std::vector<std::vector<double>> work(rows, std::vector<double>(cols, 0.0));
    auto const snapshot = [&]() {
        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t j = 0; j < cols; ++j) {
                work[i][j] = td.values[r.r_start + i][r.c_start + j];
            }
        }
    };

    for (int iter = 0; iter < iterations; ++iter) {
        snapshot();
        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t j = 0; j < cols; ++j) {
                double sum = 0.0;
                std::size_t n = 0;
                std::size_t const i_lo = i == 0 ? 0 : i - 1;
                std::size_t const i_hi = i + 1 < rows ? i + 1 : i;
                std::size_t const j_lo = j == 0 ? 0 : j - 1;
                std::size_t const j_hi = j + 1 < cols ? j + 1 : j;
                for (std::size_t ii = i_lo; ii <= i_hi; ++ii) {
                    for (std::size_t jj = j_lo; jj <= j_hi; ++jj) {
                        sum += work[ii][jj];
                        ++n;
                    }
                }
                td.values[r.r_start + i][r.c_start + j] = sum / static_cast<double>(n);
            }
        }
    }
    return ok();
}

Status interpolate_cells(Definition::TableData &td, Rect r) {
    if (auto vr = validate_rect(td, r); !vr.has_value())
        return vr;

    auto const rows = r.rows();
    auto const cols = r.cols();
    if (rows == 1 && cols == 1)
        return ok();

    auto const &grid = td.values;
    double const v00 = grid[r.r_start][r.c_start];
    double const v01 = grid[r.r_start][r.c_end];
    double const v10 = grid[r.r_end][r.c_start];
    double const v11 = grid[r.r_end][r.c_end];

    auto const row_denom = rows > 1 ? static_cast<double>(rows - 1) : 1.0;
    auto const col_denom = cols > 1 ? static_cast<double>(cols - 1) : 1.0;

    for (std::size_t i = 0; i < rows; ++i) {
        double const ty = rows > 1 ? static_cast<double>(i) / row_denom : 0.0;
        for (std::size_t j = 0; j < cols; ++j) {
            double const tx = cols > 1 ? static_cast<double>(j) / col_denom : 0.0;

            double const top = v00 + tx * (v01 - v00);
            double const bottom = v10 + tx * (v11 - v10);
            double const out = top + ty * (bottom - top);

            td.values[r.r_start + i][r.c_start + j] = out;
        }
    }
    return ok();
}

// ---- Snapshots & undo/redo ----------------------------------------------

Result<Snapshot> snapshot(Definition::TableData const &td, Rect r) {
    if (auto vr = validate_rect(td, r); !vr.has_value())
        return failure(vr.error());

    Snapshot s;
    s.rect = r;
    s.values.assign(r.rows(), std::vector<double>(r.cols(), 0.0));
    for (std::size_t i = 0; i < r.rows(); ++i) {
        for (std::size_t j = 0; j < r.cols(); ++j) {
            s.values[i][j] = td.values[r.r_start + i][r.c_start + j];
        }
    }
    return s;
}

Status restore(Definition::TableData &td, Snapshot const &s) {
    if (auto vr = validate_rect(td, s.rect); !vr.has_value())
        return vr;
    if (s.values.size() != s.rect.rows()) {
        return failure(ErrorCode::InvalidArgument,
                       "edit: snapshot row count " + std::to_string(s.values.size()) +
                           " does not match its rect (" + std::to_string(s.rect.rows()) +
                           " rows expected)");
    }
    for (std::size_t i = 0; i < s.rect.rows(); ++i) {
        if (s.values[i].size() != s.rect.cols()) {
            return failure(ErrorCode::InvalidArgument,
                           "edit: snapshot row " + std::to_string(i) + " column count " +
                               std::to_string(s.values[i].size()) +
                               " does not match its rect (" +
                               std::to_string(s.rect.cols()) + " cols expected)");
        }
        for (std::size_t j = 0; j < s.rect.cols(); ++j) {
            td.values[s.rect.r_start + i][s.rect.c_start + j] = s.values[i][j];
        }
    }
    return ok();
}

void History::record(Edit e) {
    // Branch: drop everything past the cursor before appending.
    if (cursor_ < edits_.size()) {
        edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(cursor_), edits_.end());
    }
    edits_.push_back(std::move(e));
    cursor_ = edits_.size();
}

Edit const *History::undo() noexcept {
    if (!can_undo())
        return nullptr;
    --cursor_;
    return &edits_[cursor_];
}

Edit const *History::redo() noexcept {
    if (!can_redo())
        return nullptr;
    auto const *e = &edits_[cursor_];
    ++cursor_;
    return e;
}

void History::clear() noexcept {
    edits_.clear();
    cursor_ = 0;
}

void History::load(std::vector<Edit> edits, std::size_t cursor) noexcept {
    edits_ = std::move(edits);
    cursor_ = cursor > edits_.size() ? edits_.size() : cursor;
}

} // namespace st::edit
