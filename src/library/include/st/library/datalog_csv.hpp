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
#include <string>
#include <string_view>
#include <vector>

namespace st::library::datalog_csv {

// Split a single CSV line on commas. Trims trailing CR for CRLF
// endings; does not unquote (the AP datalog format never quotes).
[[nodiscard]] std::vector<std::string_view>
split_line(std::string_view line);

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

// Full parsed datalog. data is column-major (data[col][row]). headers.size()
// always equals data.size(). row_count is the row count of the first
// column — every column is padded to the same length by parse_csv.
struct ParsedDatalog {
    std::vector<std::string> headers;
    std::vector<std::vector<float>> data;
    std::vector<ChannelStats> stats;
    std::size_t row_count{0};
};

// Parse a whole CSV text blob. Empty input returns an empty
// ParsedDatalog (no headers, no data). Robust to CRLF endings + a
// trailing newline. Computes stats inline so callers don't need to
// re-walk the data.
[[nodiscard]] ParsedDatalog parse(std::string_view text);

} // namespace st::library::datalog_csv

#endif // ST_LIBRARY_DATALOG_CSV_HPP
