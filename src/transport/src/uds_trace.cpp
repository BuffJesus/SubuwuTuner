// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/uds_trace.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace st::transport {

bool parse_uds_trace(std::filesystem::path const &path,
                     std::vector<UdsTracePair> &out_pairs,
                     std::string &err) {
    std::ifstream in{path};
    if (!in) {
        err = "uds_trace: cannot open trace file: " + path.string();
        return false;
    }
    auto const parse_hex_line = [&](std::string_view body,
                                    std::vector<std::uint8_t> &dst,
                                    int line_no) -> bool {
        std::istringstream iss{std::string{body}};
        std::string tok;
        while (iss >> tok) {
            std::string_view sv{tok};
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
            }
            if (sv.size() != 2) {
                err = "uds_trace: bad hex byte '" + tok + "' on line " +
                      std::to_string(line_no);
                return false;
            }
            unsigned value = 0;
            auto const res = std::from_chars(sv.data(), sv.data() + sv.size(),
                                             value, 16);
            if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size() ||
                value > 0xFFU) {
                err = "uds_trace: bad hex byte '" + tok + "' on line " +
                      std::to_string(line_no);
                return false;
            }
            dst.push_back(static_cast<std::uint8_t>(value));
        }
        return true;
    };

    std::string line;
    int line_no = 0;
    bool expect_request = true;
    UdsTracePair pending;
    while (std::getline(in, line)) {
        ++line_no;
        if (auto const hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size())
            continue;
        char const dir = line[i];
        if (dir != '>' && dir != '<') {
            err = "uds_trace: line " + std::to_string(line_no) +
                  " must start with '>' or '<' after any whitespace";
            return false;
        }
        std::string_view const body{line.data() + i + 1, line.size() - i - 1};
        if (dir == '>') {
            if (!expect_request) {
                err = "uds_trace: line " + std::to_string(line_no) +
                      ": two requests in a row (expected '<')";
                return false;
            }
            pending = UdsTracePair{};
            if (!parse_hex_line(body, pending.request, line_no))
                return false;
            if (pending.request.empty()) {
                err = "uds_trace: line " + std::to_string(line_no) +
                      ": request must have at least one byte";
                return false;
            }
            expect_request = false;
        } else {
            if (expect_request) {
                err = "uds_trace: line " + std::to_string(line_no) +
                      ": response with no preceding request";
                return false;
            }
            if (!parse_hex_line(body, pending.response, line_no))
                return false;
            if (pending.response.empty()) {
                err = "uds_trace: line " + std::to_string(line_no) +
                      ": response must have at least one byte";
                return false;
            }
            out_pairs.push_back(std::move(pending));
            expect_request = true;
        }
    }
    if (!expect_request) {
        err = "uds_trace: trace ends with an unmatched request "
              "(missing '<' response)";
        return false;
    }
    if (out_pairs.empty()) {
        err = "uds_trace: trace file contains no exchanges: " + path.string();
        return false;
    }
    return true;
}

} // namespace st::transport
