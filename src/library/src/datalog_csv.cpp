// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/datalog_csv.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace st::library::datalog_csv {

std::vector<std::string_view> split_line(std::string_view line) {
    std::vector<std::string_view> cells;
    cells.reserve(16);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            std::string_view const cell = line.substr(start, i - start);
            std::string_view trimmed = cell;
            while (!trimmed.empty() && trimmed.back() == '\r') {
                trimmed.remove_suffix(1);
            }
            cells.push_back(trimmed);
            start = i + 1;
        }
    }
    return cells;
}

float parse_cell(std::string_view cell) {
    if (cell.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    char stack_buf[64];
    char *buf = stack_buf;
    std::string heap_buf;
    if (cell.size() >= sizeof stack_buf) {
        heap_buf.assign(cell.begin(), cell.end());
        buf = heap_buf.data();
    } else {
        std::memcpy(stack_buf, cell.data(), cell.size());
        stack_buf[cell.size()] = '\0';
    }
    char *end = nullptr;
    double const v = std::strtod(buf, &end);
    if (end == buf) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return static_cast<float>(v);
}

namespace {

void compute_stats(ParsedDatalog &dl) {
    dl.stats.assign(dl.headers.size(), ChannelStats{});
    for (std::size_t col = 0; col < dl.data.size(); ++col) {
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        double sum = 0.0;
        std::size_t n = 0;
        for (float v : dl.data[col]) {
            if (std::isnan(v)) {
                continue;
            }
            double const d = static_cast<double>(v);
            if (d < min_v) {
                min_v = d;
            }
            if (d > max_v) {
                max_v = d;
            }
            sum += d;
            ++n;
        }
        if (n > 0) {
            dl.stats[col].min = min_v;
            dl.stats[col].max = max_v;
            dl.stats[col].mean = sum / static_cast<double>(n);
            dl.stats[col].sample_count = n;
        }
    }
}

} // namespace

ParsedDatalog parse(std::string_view text) {
    ParsedDatalog dl;
    if (text.empty()) {
        return dl;
    }
    std::vector<std::string_view> lines;
    lines.reserve(1024);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.empty()) {
        return dl;
    }
    auto const header_cells = split_line(lines[0]);
    dl.headers.reserve(header_cells.size());
    for (auto const &cell : header_cells) {
        dl.headers.emplace_back(cell);
    }
    dl.data.assign(dl.headers.size(), {});
    for (auto &col : dl.data) {
        col.reserve(lines.size() - 1);
    }
    for (std::size_t li = 1; li < lines.size(); ++li) {
        auto const cells = split_line(lines[li]);
        for (std::size_t col = 0; col < dl.data.size(); ++col) {
            float const v = col < cells.size()
                                  ? parse_cell(cells[col])
                                  : std::numeric_limits<float>::quiet_NaN();
            dl.data[col].push_back(v);
        }
    }
    if (!dl.data.empty()) {
        dl.row_count = dl.data[0].size();
    }
    compute_stats(dl);
    return dl;
}

} // namespace st::library::datalog_csv
