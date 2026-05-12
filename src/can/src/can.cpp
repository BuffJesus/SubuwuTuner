// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/can.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace st::can {

bool frames_equal(Frame const &a, Frame const &b) noexcept {
    if (a.timestamp_ns != b.timestamp_ns) return false;
    if (a.bus != b.bus)                   return false;
    if (a.id != b.id)                     return false;
    if (a.extended != b.extended)         return false;
    if (a.remote != b.remote)             return false;
    if (a.dlc != b.dlc)                   return false;
    for (std::size_t i = 0; i < a.dlc; ++i) {
        if (a.data[i] != b.data[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------

namespace {

// Try to parse one whitespace-delimited token from the input cursor.
// On success advances `pos` past the token and returns the token's
// string_view (referencing the original buffer). On EOL returns an
// empty optional. Whitespace handling is permissive — runs of spaces
// and tabs are treated identically.
std::string_view next_token(std::string_view line, std::size_t &pos) {
    while (pos < line.size()
           && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    auto const start = pos;
    while (pos < line.size()
           && line[pos] != ' ' && line[pos] != '\t') {
        ++pos;
    }
    return line.substr(start, pos - start);
}

// Parse an unsigned integer in hex; returns false on garbage.
bool parse_hex_u32(std::string_view tok, std::uint32_t &out) {
    if (tok.empty()) return false;
    std::uint32_t v = 0;
    auto const    r = std::from_chars(tok.data(), tok.data() + tok.size(),
                                      v, 16);
    if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size()) {
        return false;
    }
    out = v;
    return true;
}

bool parse_hex_u8(std::string_view tok, std::uint8_t &out) {
    std::uint32_t v = 0;
    if (!parse_hex_u32(tok, v)) return false;
    if (v > 0xFFU) return false;
    out = static_cast<std::uint8_t>(v);
    return true;
}

bool parse_decimal_u32(std::string_view tok, std::uint32_t &out) {
    if (tok.empty()) return false;
    std::uint32_t v = 0;
    auto const    r = std::from_chars(tok.data(), tok.data() + tok.size(),
                                      v, 10);
    if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size()) {
        return false;
    }
    out = v;
    return true;
}

// Parse `<seconds>.<fraction>` into int64 nanoseconds. The .asc format
// uses 6-decimal-place timestamps (microsecond resolution) but some
// emitters write more; we accept any decimal precision and round to ns.
bool parse_seconds_to_ns(std::string_view tok, std::int64_t &out) {
    if (tok.empty()) return false;
    auto const dot = tok.find('.');
    std::int64_t   secs   = 0;
    std::int64_t   frac_ns = 0;
    bool           negative = !tok.empty() && tok.front() == '-';
    std::size_t    int_begin = negative ? 1U : 0U;

    auto const int_part = tok.substr(int_begin,
                                      dot == std::string_view::npos
                                          ? std::string_view::npos
                                          : dot - int_begin);
    if (int_part.empty()) {
        secs = 0;  // leading "." like ".001" — valid; integer part is 0
    } else {
        auto const r = std::from_chars(int_part.data(),
                                       int_part.data() + int_part.size(),
                                       secs, 10);
        if (r.ec != std::errc{} || r.ptr != int_part.data() + int_part.size()) {
            return false;
        }
    }

    if (dot != std::string_view::npos) {
        auto const frac = tok.substr(dot + 1);
        // Convert up to 9 digits of fraction; pad/truncate to exactly 9.
        char padded[10] = {'0','0','0','0','0','0','0','0','0','\0'};
        auto const take = std::min<std::size_t>(frac.size(), 9);
        for (std::size_t i = 0; i < take; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(frac[i]))) {
                return false;
            }
            padded[i] = frac[i];
        }
        auto const r = std::from_chars(padded, padded + 9, frac_ns, 10);
        if (r.ec != std::errc{}) return false;
    }

    constexpr std::int64_t kNsPerSec = 1'000'000'000;
    std::int64_t value = secs * kNsPerSec + frac_ns;
    if (negative) value = -value;
    out = value;
    return true;
}

// Try to parse a single frame from `line`. Returns true on success and
// fills `out`. Returns false (without setting `err`) when the line is
// not a frame line (header, comment, blank); sets `err` and returns
// false when the line *looks* like a frame line but is malformed.
bool try_parse_frame_line(std::string_view line, Frame &out,
                          std::string &err) {
    std::size_t pos = 0;
    auto const  ts_tok = next_token(line, pos);
    if (ts_tok.empty()) return false;
    // First token must be a number (seconds) — otherwise it's a header
    // line ("date ...", "base hex ...", "Begin Triggerblock ...").
    if (!(ts_tok.front() == '-' || std::isdigit(static_cast<unsigned char>(ts_tok.front()))
          || ts_tok.front() == '.')) {
        return false;
    }
    std::int64_t ts_ns = 0;
    if (!parse_seconds_to_ns(ts_tok, ts_ns)) {
        return false;  // unknown header pattern starting with a digit
    }

    auto const chan_tok = next_token(line, pos);
    std::uint32_t chan = 0;
    if (!parse_decimal_u32(chan_tok, chan) || chan == 0) {
        err = "missing or non-numeric channel";
        return false;
    }

    auto id_tok = next_token(line, pos);
    if (id_tok.empty()) {
        err = "missing CAN id";
        return false;
    }
    bool extended = false;
    if (id_tok.back() == 'x' || id_tok.back() == 'X') {
        extended = true;
        id_tok.remove_suffix(1);
    }
    std::uint32_t can_id = 0;
    if (!parse_hex_u32(id_tok, can_id)) {
        err = "bad CAN id '" + std::string{id_tok} + "'";
        return false;
    }

    auto const dir_tok = next_token(line, pos);
    if (dir_tok != "Rx" && dir_tok != "Tx") {
        // Some .asc emitters embed extra fields before Rx/Tx (e.g.,
        // length type codes). For now this means "not a frame line we
        // recognize" rather than a hard error.
        return false;
    }

    auto       kind_tok = next_token(line, pos);
    bool       remote   = false;
    if (kind_tok == "r") {
        remote = true;
    } else if (kind_tok != "d") {
        // Unknown kind — treat as not-a-frame (be forgiving).
        return false;
    }

    auto const dlc_tok = next_token(line, pos);
    std::uint32_t dlc_u = 0;
    if (!parse_decimal_u32(dlc_tok, dlc_u) || dlc_u > 8) {
        err = "bad DLC '" + std::string{dlc_tok} + "'";
        return false;
    }

    Frame f;
    f.timestamp_ns = ts_ns;
    f.bus          = static_cast<BusId>(chan - 1);
    f.id           = can_id;
    f.extended     = extended;
    f.remote       = remote;
    f.dlc          = static_cast<std::uint8_t>(dlc_u);

    if (!remote) {
        for (std::uint8_t i = 0; i < f.dlc; ++i) {
            auto const tok = next_token(line, pos);
            std::uint8_t  byte = 0;
            if (!parse_hex_u8(tok, byte)) {
                err = "bad data byte '" + std::string{tok} + "'";
                return false;
            }
            f.data[i] = byte;
        }
    }

    out = f;
    return true;
}

} // namespace

Result<std::vector<Frame>> parse_asc(std::string_view text) {
    std::vector<Frame> out;
    std::size_t        line_no = 0;
    std::size_t        cursor  = 0;

    while (cursor <= text.size()) {
        auto const nl = text.find('\n', cursor);
        auto const end =
            nl == std::string_view::npos ? text.size() : nl;
        auto line = text.substr(cursor, end - cursor);
        // Strip optional trailing '\r' for CRLF-encoded files.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++line_no;

        Frame       f;
        std::string err;
        if (try_parse_frame_line(line, f, err)) {
            out.push_back(f);
        } else if (!err.empty()) {
            return failure(ErrorCode::ParseError,
                           "asc: line " + std::to_string(line_no)
                               + ": " + err);
        }

        if (nl == std::string_view::npos) break;
        cursor = nl + 1;
    }
    return out;
}

Result<std::vector<Frame>> read_asc(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound,
                       "asc: cannot open: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto const text = std::move(ss).str();
    return parse_asc(text);
}

// ---------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------

namespace {

// Format `timestamp_ns` as a 6-decimal-place seconds value, the
// standard .asc precision. Negative values are accepted but unusual.
void append_seconds_6dp(std::string &out, std::int64_t timestamp_ns) {
    bool const negative = timestamp_ns < 0;
    if (negative) timestamp_ns = -timestamp_ns;
    auto const sec  = timestamp_ns / 1'000'000'000;
    auto const frac = (timestamp_ns % 1'000'000'000) / 1'000;  // µs
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s%lld.%06lld",
                  negative ? "-" : "",
                  static_cast<long long>(sec),
                  static_cast<long long>(frac));
    out += buf;
}

void append_id(std::string &out, std::uint32_t id, bool extended) {
    char buf[24];
    if (extended) {
        std::snprintf(buf, sizeof(buf), "%Xx", id);
    } else {
        std::snprintf(buf, sizeof(buf), "%X", id);
    }
    out += buf;
}

} // namespace

std::string format_asc(std::span<Frame const> frames) {
    std::string out;
    out.reserve(frames.size() * 64 + 128);

    // Minimal header — cantools, SavvyCAN, and CANalyzer all accept this
    // shape. The "Begin Triggerblock" line is required by some parsers
    // (cantools rejects without it) so include it.
    out += "date Wed Jan 01 00:00:00 1970\n";
    out += "base hex  timestamps absolute\n";
    out += "internal events logged\n";
    out += "// version 8.0.0\n";
    out += "Begin Triggerblock\n";

    char dlc_buf[8];
    for (auto const &f : frames) {
        out += "   ";
        append_seconds_6dp(out, f.timestamp_ns);
        out += ' ';
        out += std::to_string(static_cast<unsigned>(f.bus) + 1U);
        out += "  ";
        append_id(out, f.id, f.extended);
        out += "             Rx   ";
        if (f.remote) {
            out += "r ";
        } else {
            out += "d ";
        }
        std::snprintf(dlc_buf, sizeof(dlc_buf), "%u",
                      static_cast<unsigned>(f.dlc));
        out += dlc_buf;
        if (!f.remote) {
            char byte_buf[4];
            for (std::uint8_t i = 0; i < f.dlc; ++i) {
                std::snprintf(byte_buf, sizeof(byte_buf), " %02X",
                              static_cast<unsigned>(f.data[i]));
                out += byte_buf;
            }
        }
        out += '\n';
    }

    out += "End TriggerBlock\n";
    return out;
}

Status write_asc(std::filesystem::path const &path,
                  std::span<Frame const>       frames) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "asc: cannot open for write: " + path.string());
    }
    auto const text = format_asc(frames);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "asc: write failed: " + path.string());
    }
    return ok();
}

} // namespace st::can
