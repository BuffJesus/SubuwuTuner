// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_CSV_HPP
#define ST_CORE_CSV_HPP

// Minimal CSV-parsing helpers shared by the log-analyzer modules
// (st::log::knock, st::log::adaptive, st::log::coldstart). Pulled
// out after three callers grew identical copies inline.
//
// Scope is deliberately narrow:
//   - row format: comma-separated, no quoting, no escaping
//   - field trim is space/tab/CR only (NOT a full std::isspace —
//     keeps consistent behavior across translation units regardless
//     of locale)
//   - lines split on '\n'; blank lines and '#'-comment lines skipped
//   - numeric parsing accepts an empty field as 0.0 (so half-filled
//     log rows degrade gracefully); strict strtod otherwise
//
// st::autotune's CSV reader has different semantics (locale-sensitive
// isspace trim, isfinite check, optional bool fields) and is NOT
// migrated to these helpers — see src/autotune/src/autotune.cpp for
// the parallel set. Factor those in too if a fifth caller arrives.

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace st::core::csv {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty()
           && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty()
           && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] inline std::vector<std::string_view>
split_fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t                   start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.push_back(trim(line.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

// Parses an integer or floating-point decimal from `s`. Empty input
// returns 0.0 with `true` so half-filled CSV rows degrade gracefully.
// Returns `false` on a non-numeric input.
[[nodiscard]] inline bool parse_double(std::string_view s, double &out) noexcept {
    if (s.empty()) {
        out = 0.0;
        return true;
    }
    std::string copy(s);
    char       *end = nullptr;
    double const v  = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || end == nullptr || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

// Splits `text` into non-blank, non-'#'-comment lines. Lines retain
// no trailing CR (handled by trim()).
[[nodiscard]] inline std::vector<std::string_view>
split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t                   i = 0;
    while (i < text.size()) {
        std::size_t e = i;
        while (e < text.size() && text[e] != '\n') {
            ++e;
        }
        std::string_view const line = trim(text.substr(i, e - i));
        if (!line.empty() && line.front() != '#') {
            out.push_back(line);
        }
        i = e + 1;
    }
    return out;
}

}  // namespace st::core::csv

#endif  // ST_CORE_CSV_HPP
