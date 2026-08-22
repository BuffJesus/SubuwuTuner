// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/datalog_csv.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
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
    while (!cell.empty() && (cell.front() == ' ' || cell.front() == '\t')) {
        cell.remove_prefix(1);
    }
    while (!cell.empty() && (cell.back() == ' ' || cell.back() == '\t' || cell.back() == '\r')) {
        cell.remove_suffix(1);
    }
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
    if (end == buf || *end != '\0') {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return static_cast<float>(v);
}

namespace {

ChannelMetadata parse_metadata(std::string const &header) {
    ChannelMetadata metadata{header, {}};
    if (header.size() < 4) {
        return metadata;
    }
    char const closing = header.back();
    char const opening = closing == ']' ? '[' : (closing == ')' ? '(' : '\0');
    if (opening == '\0') {
        return metadata;
    }
    auto const start = header.find_last_of(opening);
    if (start == std::string::npos || start == 0 || start + 2 >= header.size()) {
        return metadata;
    }
    std::size_t name_end = start;
    while (name_end > 0 && header[name_end - 1] == ' ') {
        --name_end;
    }
    metadata.display_name = header.substr(0, name_end);
    metadata.unit = header.substr(start + 1, header.size() - start - 2);
    return metadata;
}

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

std::vector<ChannelStats> range_stats(ParsedDatalog const &datalog, std::size_t first_row,
                                      std::size_t last_row) {
    first_row = std::min(first_row, datalog.row_count);
    last_row = std::min(last_row, datalog.row_count);
    if (last_row < first_row) {
        std::swap(first_row, last_row);
    }
    std::vector<ChannelStats> result(datalog.data.size());
    for (std::size_t column = 0; column < datalog.data.size(); ++column) {
        double min_value = std::numeric_limits<double>::infinity();
        double max_value = -std::numeric_limits<double>::infinity();
        double sum = 0.0;
        std::size_t count = 0;
        auto const stop = std::min(last_row, datalog.data[column].size());
        for (std::size_t row = std::min(first_row, stop); row < stop; ++row) {
            float const value = datalog.data[column][row];
            if (std::isnan(value)) {
                continue;
            }
            min_value = std::min(min_value, static_cast<double>(value));
            max_value = std::max(max_value, static_cast<double>(value));
            sum += static_cast<double>(value);
            ++count;
        }
        if (count != 0) {
            result[column] = {min_value, max_value, sum / static_cast<double>(count), count};
        }
    }
    return result;
}

std::string export_csv(ParsedDatalog const &datalog, std::vector<std::size_t> const &columns,
                       std::size_t first_row, std::size_t last_row) {
    std::vector<std::size_t> valid_columns;
    for (auto const column : columns) {
        if (column < datalog.data.size()) {
            valid_columns.push_back(column);
        }
    }
    auto write_cell = [](std::ostringstream &out, std::string_view value) {
        bool const quote = value.find_first_of(",\"\r\n") != std::string_view::npos;
        if (!quote) {
            out << value;
            return;
        }
        out << '"';
        for (char ch : value) {
            out << ch;
            if (ch == '"') {
                out << '"';
            }
        }
        out << '"';
    };
    std::ostringstream out;
    for (std::size_t i = 0; i < valid_columns.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        write_cell(out, datalog.headers[valid_columns[i]]);
    }
    out << '\n';
    first_row = std::min(first_row, datalog.row_count);
    last_row = std::min(last_row, datalog.row_count);
    if (last_row < first_row) {
        std::swap(first_row, last_row);
    }
    for (std::size_t row = first_row; row < last_row; ++row) {
        for (std::size_t i = 0; i < valid_columns.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            auto const &values = datalog.data[valid_columns[i]];
            if (row < values.size() && !std::isnan(values[row])) {
                out << values[row];
            }
        }
        out << '\n';
    }
    return out.str();
}

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
    auto is_comment = [](std::string_view line) {
        while (!line.empty() &&
               (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
            line.remove_prefix(1);
        }
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.remove_prefix(3);
        }
        return !line.empty() && line.front() == '#';
    };
    std::size_t header_line = 0;
    while (header_line < lines.size() && is_comment(lines[header_line])) {
        ++header_line;
    }
    if (header_line == lines.size()) {
        return dl;
    }
    auto const header_cells = split_line(lines[header_line]);
    dl.headers.reserve(header_cells.size());
    for (auto const &cell : header_cells) {
        dl.headers.emplace_back(cell);
    }
    if (!dl.headers.empty() && dl.headers[0].size() >= 3 &&
        static_cast<unsigned char>(dl.headers[0][0]) == 0xEF &&
        static_cast<unsigned char>(dl.headers[0][1]) == 0xBB &&
        static_cast<unsigned char>(dl.headers[0][2]) == 0xBF) {
        dl.headers[0].erase(0, 3);
    }
    dl.metadata.reserve(dl.headers.size());
    for (auto const &header : dl.headers) {
        dl.metadata.push_back(parse_metadata(header));
    }
    auto normalized_header = [](std::string value) {
        std::string out;
        out.reserve(value.size());
        for (char ch : value) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (ch != ' ' && ch != '_' && ch != '-') {
                out.push_back(ch);
            }
        }
        return out;
    };
    for (std::size_t i = 0; i < dl.headers.size(); ++i) {
        auto const name = normalized_header(dl.headers[i]);
        if (name == "time" || name.starts_with("time(") || name.starts_with("time[") ||
            name == "timestamp" || name == "times" || name == "timestampms" ||
            name == "elapsedtime") {
            dl.time_column = i;
            break;
        }
    }
    dl.data.assign(dl.headers.size(), {});
    for (auto &col : dl.data) {
        col.reserve(lines.size() - header_line - 1);
    }
    for (std::size_t li = header_line + 1; li < lines.size(); ++li) {
        if (is_comment(lines[li])) {
            continue;
        }
        auto const cells = split_line(lines[li]);
        if (cells.size() != dl.data.size()) {
            ++dl.malformed_row_count;
        }
        for (std::size_t col = 0; col < dl.data.size(); ++col) {
            float const v = col < cells.size() ? parse_cell(cells[col])
                                               : std::numeric_limits<float>::quiet_NaN();
            if (std::isnan(v) && col < cells.size() && !cells[col].empty()) {
                ++dl.invalid_cell_count;
            }
            dl.data[col].push_back(v);
        }
    }
    if (!dl.data.empty()) {
        dl.row_count = dl.data[0].size();
    }
    compute_stats(dl);
    return dl;
}

std::optional<std::size_t> append_derived_channel(ParsedDatalog &datalog, std::string name,
                                                  std::string unit, std::size_t lhs_column,
                                                  std::size_t rhs_column,
                                                  DerivedOperation operation) {
    if (lhs_column >= datalog.data.size() || rhs_column >= datalog.data.size()) {
        return std::nullopt;
    }
    auto const &lhs = datalog.data[lhs_column];
    auto const &rhs = datalog.data[rhs_column];
    std::size_t const count = std::min(lhs.size(), rhs.size());
    std::vector<float> values;
    values.reserve(datalog.row_count);
    for (std::size_t i = 0; i < count; ++i) {
        float const a = lhs[i];
        float const b = rhs[i];
        if (std::isnan(a) || std::isnan(b) ||
            ((operation == DerivedOperation::Ratio ||
              operation == DerivedOperation::PercentError) &&
             b == 0.0f)) {
            values.push_back(std::numeric_limits<float>::quiet_NaN());
            continue;
        }
        switch (operation) {
        case DerivedOperation::Subtract:
            values.push_back(a - b);
            break;
        case DerivedOperation::Ratio:
            values.push_back(a / b);
            break;
        case DerivedOperation::PercentError:
            values.push_back((a - b) / b * 100.0f);
            break;
        }
    }
    values.resize(datalog.row_count, std::numeric_limits<float>::quiet_NaN());
    datalog.headers.push_back(name + (unit.empty() ? "" : " [" + unit + "]"));
    datalog.metadata.push_back({std::move(name), std::move(unit)});
    datalog.data.push_back(std::move(values));
    compute_stats(datalog);
    return datalog.data.size() - 1;
}

} // namespace st::library::datalog_csv
