// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/devices/ap3/datalogger_cfg.hpp"

#include "st/core/error.hpp"

#include <string>
#include <string_view>

namespace st::devices::ap3 {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

Result<DataloggerPreset> decode_datalogger_cfg(std::string_view text) {
    DataloggerPreset out;
    bool saw_name = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i != text.size() && text[i] != '\n') {
            continue;
        }
        std::string_view line = text.substr(start, i - start);
        start = i + 1;
        // Strip a trailing '\r' (CRLF) before any other processing.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        auto const eq = line.find('=');
        if (eq == std::string_view::npos) {
            // Tolerate stray non-key lines (likely user-edited cruft);
            // refusing here would break round-trip on real AP files
            // that have a vestigial header line or blank record.
            continue;
        }
        auto const key = trim(line.substr(0, eq));
        auto const value = trim(line.substr(eq + 1));
        if (key == "preset_name") {
            out.name = std::string{value};
            saw_name = true;
        } else if (key == "preset_description") {
            out.description = std::string{value};
        } else if (key == "log_mon_list") {
            out.log_mon_list.clear();
            std::size_t lstart = 0;
            for (std::size_t j = 0; j <= value.size(); ++j) {
                if (j == value.size() || value[j] == ',') {
                    auto const item = trim(value.substr(lstart, j - lstart));
                    if (!item.empty()) {
                        out.log_mon_list.emplace_back(item);
                    }
                    lstart = j + 1;
                }
            }
        }
        // Unknown keys silently ignored — forward-compat with any
        // future fields the AP firmware adds.
    }
    if (!saw_name) {
        return st::failure(st::ErrorCode::ParseError,
                           "ap3::datalogger_cfg: missing required `preset_name` key");
    }
    return out;
}

std::string encode_datalogger_cfg(DataloggerPreset const &p) {
    std::string out;
    out.reserve(64 + p.description.size() + p.log_mon_list.size() * 16);
    out += "preset_name=";
    out += p.name;
    out += "\r\n";
    out += "preset_description=";
    out += p.description;
    out += "\r\n";
    out += "log_mon_list=";
    for (std::size_t i = 0; i < p.log_mon_list.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += p.log_mon_list[i];
    }
    out += "\r\n";
    return out;
}

} // namespace st::devices::ap3
