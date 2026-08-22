// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::datalog_csv — minimal CSV parser tailored to the
// already-decompressed COBB AP datalog format (first row is column
// names, subsequent rows are numeric floats, comma-separated, no
// quoted fields). Lives in st::library so the GUI's F6 datalog
// viewer can call it AND it stays unit-testable independently of
// the ImGui modal.

#ifndef ST_LIBRARY_DATALOG_CSV_HPP
#define ST_LIBRARY_DATALOG_CSV_HPP

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::library::datalog_csv {

// Split a single CSV line on commas. Trims trailing CR for CRLF
// endings; does not unquote (the AP datalog format never quotes).
[[nodiscard]] std::vector<std::string_view> split_line(std::string_view line);

// Parse a single CSV cell as a float. Empty cells, non-numeric
// content, and parse failures all return NaN — the caller filters
// NaN before computing stats.
[[nodiscard]] float parse_cell(std::string_view cell);

// One channel's stats. NaN samples are excluded; sample_count is
// the post-filter row count.
struct ChannelStats {
    double min{std::numeric_limits<double>::quiet_NaN()};
    double max{std::numeric_limits<double>::quiet_NaN()};
    double mean{std::numeric_limits<double>::quiet_NaN()};
    std::size_t sample_count{0};
};

struct ChannelMetadata {
    std::string display_name;
    std::string unit;
};

enum class DerivedOperation {
    Subtract,
    Ratio,
    PercentError,
};

// Full parsed datalog. data is column-major (data[col][row]). headers.size()
// always equals data.size(). row_count is the row count of the first
// column — every column is padded to the same length by parse_csv.
struct ParsedDatalog {
    std::vector<std::string> headers;
    std::vector<ChannelMetadata> metadata;
    std::vector<std::vector<float>> data;
    std::vector<ChannelStats> stats;
    std::size_t row_count{0};
    // Import diagnostics are deliberately non-fatal: real-world logger
    // exports commonly contain a short final row or an occasional extra
    // column. The explorer can surface that loss of fidelity without making
    // the rest of the session unusable.
    std::size_t malformed_row_count{0};
    std::size_t invalid_cell_count{0};
    std::optional<std::size_t> time_column;
};

// Parse a whole CSV text blob. Empty input returns an empty
// ParsedDatalog (no headers, no data). Robust to CRLF endings + a
// trailing newline. Computes stats inline so callers don't need to
// re-walk the data.
[[nodiscard]] ParsedDatalog parse(std::string_view text);

// Append a calculated channel while preserving the column-major shape and
// statistics invariants. Returns the new column index, or nullopt when either
// source index is invalid. Division by zero and missing source samples become
// NaN gaps.
[[nodiscard]] std::optional<std::size_t>
append_derived_channel(ParsedDatalog &datalog, std::string name, std::string unit,
                       std::size_t lhs_column, std::size_t rhs_column, DerivedOperation operation);

// Compute statistics for the half-open row range [first_row, last_row). Invalid
// bounds are clamped to the available rows; an empty range returns empty stats.
[[nodiscard]] std::vector<ChannelStats> range_stats(ParsedDatalog const &datalog,
                                                    std::size_t first_row,
                                                    std::size_t last_row);

// Export a half-open row range and selected columns as standards-compliant CSV.
// Unknown column indexes are ignored. NaN samples are emitted as empty cells.
[[nodiscard]] std::string export_csv(ParsedDatalog const &datalog,
                                     std::vector<std::size_t> const &columns,
                                     std::size_t first_row, std::size_t last_row);

} // namespace st::library::datalog_csv

#endif // ST_LIBRARY_DATALOG_CSV_HPP
