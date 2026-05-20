// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/dbc.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace st::dbc {

// ---------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------

Message const *Database::find_message(std::uint32_t id) const noexcept {
    for (auto const &m : messages) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

Signal const *Database::find_signal(std::uint32_t msg_id, std::string_view name) const noexcept {
    auto const *m = find_message(msg_id);
    if (m == nullptr)
        return nullptr;
    for (auto const &s : m->signals) {
        if (s.name == name)
            return &s;
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------

namespace {

constexpr std::uint32_t kExtendedFlag = 0x8000'0000U;

void trim(std::string_view &s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
}

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool parse_u32(std::string_view tok, std::uint32_t &out) {
    if (tok.empty())
        return false;
    std::uint32_t v = 0;
    auto const r = std::from_chars(tok.data(), tok.data() + tok.size(), v, 10);
    if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size()) {
        return false;
    }
    out = v;
    return true;
}

bool parse_size(std::string_view tok, std::size_t &out) {
    std::uint32_t v = 0;
    if (!parse_u32(tok, v))
        return false;
    out = v;
    return true;
}

bool parse_double(std::string_view tok, double &out) {
    if (tok.empty())
        return false;
    // GCC's libstdc++ supports floating-point std::from_chars since
    // 11; relying on it for portability. Falls back to std::strtod
    // semantics on the same conventions: decimal point, optional
    // exponent.
    char buf[64];
    auto const n = std::min(tok.size(), sizeof(buf) - 1);
    for (std::size_t i = 0; i < n; ++i)
        buf[i] = tok[i];
    buf[n] = '\0';
    char *end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf)
        return false;
    out = v;
    return true;
}

// Pull one whitespace-delimited token from `line`, advancing `pos`.
std::string_view next_token(std::string_view line, std::size_t &pos) {
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    auto const start = pos;
    while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    return line.substr(start, pos - start);
}

// Pull a quoted string ("..." with no escape handling beyond \" and \\),
// advancing `pos` past the closing quote. Returns false if no quoted
// string is found at the cursor (after whitespace).
bool next_quoted(std::string_view line, std::size_t &pos, std::string &out) {
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != '"')
        return false;
    ++pos;
    out.clear();
    while (pos < line.size()) {
        char const c = line[pos++];
        if (c == '"')
            return true;
        if (c == '\\' && pos < line.size()) {
            out.push_back(line[pos++]);
        } else {
            out.push_back(c);
        }
    }
    return false; // unterminated string
}

// Parse a BO_ line. Format:
//
//     BO_ <id> <name>: <length> <sender>
//
// where <id> is decimal; CAN-extended messages set the high bit of the
// 32-bit value (0x80000000).
bool parse_message_header(std::string_view line, Message &msg, std::string &err) {
    std::size_t pos = 0;
    auto const tag = next_token(line, pos);
    if (tag != "BO_")
        return false;

    auto const id_tok = next_token(line, pos);
    std::uint32_t raw_id = 0;
    if (!parse_u32(id_tok, raw_id)) {
        err = "BO_: bad id '" + std::string{id_tok} + "'";
        return false;
    }
    msg.extended = (raw_id & kExtendedFlag) != 0U;
    msg.id = raw_id & ~kExtendedFlag;

    auto name_tok = next_token(line, pos);
    if (name_tok.empty()) {
        err = "BO_: missing name";
        return false;
    }
    // The name token ends with ':' followed by length. The trailing
    // colon may be glued to the name (`Foo:`) or appear after a space
    // (`Foo :`). Handle both.
    std::string name_str;
    if (name_tok.back() == ':') {
        name_str.assign(name_tok.data(), name_tok.size() - 1);
    } else {
        name_str.assign(name_tok.data(), name_tok.size());
        auto const colon = next_token(line, pos);
        if (colon != ":") {
            err = "BO_: missing ':' after name";
            return false;
        }
    }
    msg.name = std::move(name_str);

    auto const len_tok = next_token(line, pos);
    if (!parse_size(len_tok, msg.length)) {
        err = "BO_: bad length '" + std::string{len_tok} + "'";
        return false;
    }

    auto const sender_tok = next_token(line, pos);
    msg.sender.assign(sender_tok.data(), sender_tok.size());
    return true;
}

// Parse one SG_ row. Format:
//
//     SG_ <name> : <start>|<len>@<order><sign> (<factor>,<offset>)
//          [<min>|<max>] "<unit>" <recv1>[,<recv2>...]
//
// Whitespace is forgiving; the syntactic markers `:`, `|`, `@`, `,`,
// `(`, `)`, `[`, `]` are not.
bool parse_signal_row(std::string_view line, Signal &sig, std::string &err) {
    std::size_t pos = 0;
    auto const tag = next_token(line, pos);
    if (tag != "SG_")
        return false;

    auto const name_tok = next_token(line, pos);
    if (name_tok.empty()) {
        err = "SG_: missing name";
        return false;
    }
    sig.name.assign(name_tok.data(), name_tok.size());

    auto const colon = next_token(line, pos);
    if (colon != ":") {
        err = "SG_: missing ':' after name";
        return false;
    }

    // <start>|<len>@<order><sign>
    auto const bits_tok = next_token(line, pos);
    {
        auto const pipe = bits_tok.find('|');
        auto const at = bits_tok.find('@');
        if (pipe == std::string_view::npos || at == std::string_view::npos || pipe >= at) {
            err = "SG_: bad bit-layout '" + std::string{bits_tok} + "'";
            return false;
        }
        auto const start_sv = bits_tok.substr(0, pipe);
        auto const len_sv = bits_tok.substr(pipe + 1, at - pipe - 1);
        if (!parse_size(start_sv, sig.start_bit) || !parse_size(len_sv, sig.length_bits)) {
            err = "SG_: bad start|length '" + std::string{bits_tok} + "'";
            return false;
        }
        if (at + 2 > bits_tok.size()) {
            err = "SG_: missing byte-order/sign after '@'";
            return false;
        }
        char const order_c = bits_tok[at + 1];
        char const sign_c = bits_tok[at + 2];
        if (order_c != '0' && order_c != '1') {
            err = "SG_: bad byte-order '" + std::string{order_c, order_c} + "'";
            return false;
        }
        sig.byte_order = (order_c == '1') ? ByteOrder::Intel : ByteOrder::Motorola;
        if (sign_c == '+') {
            sig.sign = SignKind::Unsigned;
        } else if (sign_c == '-') {
            sig.sign = SignKind::Signed;
        } else {
            err = "SG_: bad sign marker";
            return false;
        }
    }

    // (factor, offset)
    {
        auto const tup_tok = next_token(line, pos);
        if (tup_tok.size() < 5 || tup_tok.front() != '(' || tup_tok.back() != ')') {
            err = "SG_: bad (factor,offset) '" + std::string{tup_tok} + "'";
            return false;
        }
        auto const inner = tup_tok.substr(1, tup_tok.size() - 2);
        auto const comma = inner.find(',');
        if (comma == std::string_view::npos) {
            err = "SG_: missing ',' in (factor,offset)";
            return false;
        }
        if (!parse_double(inner.substr(0, comma), sig.factor) ||
            !parse_double(inner.substr(comma + 1), sig.offset)) {
            err = "SG_: non-numeric factor/offset";
            return false;
        }
    }

    // [min|max]
    {
        auto const rng_tok = next_token(line, pos);
        if (rng_tok.size() < 5 || rng_tok.front() != '[' || rng_tok.back() != ']') {
            err = "SG_: bad [min|max] '" + std::string{rng_tok} + "'";
            return false;
        }
        auto const inner = rng_tok.substr(1, rng_tok.size() - 2);
        auto const pipe = inner.find('|');
        if (pipe == std::string_view::npos) {
            err = "SG_: missing '|' in [min|max]";
            return false;
        }
        if (!parse_double(inner.substr(0, pipe), sig.min_value) ||
            !parse_double(inner.substr(pipe + 1), sig.max_value)) {
            err = "SG_: non-numeric min/max";
            return false;
        }
    }

    // "unit"
    if (!next_quoted(line, pos, sig.unit)) {
        err = "SG_: missing quoted unit";
        return false;
    }

    // Receivers — remainder of the line, comma-separated.
    sig.receivers.clear();
    while (pos < line.size()) {
        while (pos < line.size() &&
               (std::isspace(static_cast<unsigned char>(line[pos])) || line[pos] == ',')) {
            ++pos;
        }
        if (pos >= line.size())
            break;
        auto const start = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos])) &&
               line[pos] != ',') {
            ++pos;
        }
        sig.receivers.emplace_back(line.substr(start, pos - start));
    }
    return true;
}

} // namespace

Result<Database> parse_dbc(std::string_view text) {
    Database db;

    Message current;
    bool have_current = false;
    std::size_t cursor = 0;
    std::size_t line_no = 0;

    auto flush_current = [&] {
        if (have_current) {
            db.messages.push_back(std::move(current));
            current = Message{};
            have_current = false;
        }
    };

    while (cursor <= text.size()) {
        auto const nl = text.find('\n', cursor);
        auto const end = nl == std::string_view::npos ? text.size() : nl;
        auto line = text.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++line_no;

        auto trimmed = line;
        trim(trimmed);

        if (trimmed.empty()) {
            // Blank line ends the SG_ block under a BO_.
            flush_current();
        } else if (starts_with(trimmed, "VERSION")) {
            // VERSION "x.y.z"
            std::size_t pos = 0;
            (void)next_token(trimmed, pos);
            std::string v;
            if (next_quoted(trimmed, pos, v)) {
                db.version = std::move(v);
            }
        } else if (starts_with(trimmed, "BU_:")) {
            // BU_: <node> <node> ...
            std::size_t pos = 0;
            (void)next_token(trimmed, pos); // "BU_:"
            while (pos < trimmed.size()) {
                auto const tok = next_token(trimmed, pos);
                if (tok.empty())
                    break;
                db.nodes.emplace_back(tok);
            }
        } else if (starts_with(trimmed, "BO_ ")) {
            flush_current();
            Message m;
            std::string err;
            if (!parse_message_header(trimmed, m, err)) {
                return failure(ErrorCode::ParseError,
                               "dbc: line " + std::to_string(line_no) + ": " + err);
            }
            current = std::move(m);
            have_current = true;
        } else if (starts_with(trimmed, "SG_ ")) {
            if (!have_current) {
                return failure(ErrorCode::ParseError, "dbc: line " + std::to_string(line_no) +
                                                          ": SG_ outside of a BO_ block");
            }
            Signal s;
            std::string err;
            if (!parse_signal_row(trimmed, s, err)) {
                return failure(ErrorCode::ParseError,
                               "dbc: line " + std::to_string(line_no) + ": " + err);
            }
            current.signals.push_back(std::move(s));
        }
        // All other tags (NS_, BS_, CM_, BA_, VAL_, EV_, …) are
        // skipped silently — they don't affect message/signal layout.

        if (nl == std::string_view::npos)
            break;
        cursor = nl + 1;
    }
    flush_current();
    return db;
}

Result<Database> read_dbc(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound, "dbc: cannot open: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto const text = std::move(ss).str();
    return parse_dbc(text);
}

// ---------------------------------------------------------------------
// Emitting
// ---------------------------------------------------------------------

namespace {

// Print a double in DBC's canonical form: a fixed-point integer where
// the value is an exact int, %g otherwise. DBC parsers all accept both.
void append_dbc_double(std::string &out, double v) {
    char buf[64];
    if (v == static_cast<double>(static_cast<long long>(v)) && v >= -1e15 && v <= 1e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    } else {
        std::snprintf(buf, sizeof(buf), "%g", v);
    }
    out += buf;
}

void emit_signal(std::string &out, Signal const &s) {
    char num_buf[32];
    out += " SG_ ";
    out += s.name;
    out += " : ";

    std::snprintf(num_buf, sizeof(num_buf), "%zu|%zu@", s.start_bit, s.length_bits);
    out += num_buf;
    out += (s.byte_order == ByteOrder::Intel ? '1' : '0');
    out += (s.sign == SignKind::Signed ? '-' : '+');

    out += " (";
    append_dbc_double(out, s.factor);
    out += ',';
    append_dbc_double(out, s.offset);
    out += ") [";
    append_dbc_double(out, s.min_value);
    out += '|';
    append_dbc_double(out, s.max_value);
    out += "] \"";
    out += s.unit;
    out += "\" ";

    if (s.receivers.empty()) {
        out += "Vector__XXX";
    } else {
        for (std::size_t i = 0; i < s.receivers.size(); ++i) {
            if (i > 0)
                out += ',';
            out += s.receivers[i];
        }
    }
    out += '\n';
}

void emit_message(std::string &out, Message const &m) {
    char num_buf[32];
    auto const wire_id = m.id | (m.extended ? kExtendedFlag : 0U);
    std::snprintf(num_buf, sizeof(num_buf), "BO_ %u ", static_cast<unsigned>(wire_id));
    out += num_buf;
    out += m.name;
    out += ": ";
    std::snprintf(num_buf, sizeof(num_buf), "%zu ", m.length);
    out += num_buf;
    out += m.sender.empty() ? "Vector__XXX" : m.sender;
    out += '\n';
    for (auto const &s : m.signals) {
        emit_signal(out, s);
    }
    out += '\n';
}

} // namespace

std::string format_dbc(Database const &db) {
    std::string out;
    out.reserve(256 + db.messages.size() * 128);

    out += "VERSION \"";
    out += db.version;
    out += "\"\n\n";

    // The empty NS_ / BS_ sections are present in every canonical DBC.
    // Tools tolerate their absence but emit them by default — including
    // them keeps round-trip results visually identical to the kind of
    // files a user is likely to point at us.
    out += "NS_ :\n\n";
    out += "BS_:\n\n";

    out += "BU_:";
    for (auto const &n : db.nodes) {
        out += ' ';
        out += n;
    }
    out += "\n\n";

    for (auto const &m : db.messages) {
        emit_message(out, m);
    }
    return out;
}

Status write_dbc(std::filesystem::path const &path, Database const &db) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure, "dbc: cannot open for write: " + path.string());
    }
    auto const text = format_dbc(db);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "dbc: write failed: " + path.string());
    }
    return ok();
}

// ---------------------------------------------------------------------
// Signal decoding
// ---------------------------------------------------------------------
//
// Both byte orders share the same bit-numbering scheme: a 64-bit view of
// up to 8 data bytes where bit_index = (byte_index * 8) + bit_in_byte.
// They differ only in how successive bits of a multi-bit field are laid
// out across that view:
//   - Intel    walks bit indices upward from `start_bit`.
//   - Motorola walks downward within the current byte, then jumps to the
//     MSB of the next byte (= start + 8 + 7 - 0 = start + 15) and walks
//     down again. This is the Vector "sawtooth" pattern.

std::uint64_t extract_raw_bits(std::span<std::uint8_t const> data, Signal const &sig) noexcept {
    if (sig.length_bits == 0 || sig.length_bits > 64)
        return 0;
    // Compute every source-bit index ahead of time so we can bail if any
    // of them lies past `data.size()`.
    std::array<std::size_t, 64> indices{};
    if (sig.byte_order == ByteOrder::Intel) {
        for (std::size_t i = 0; i < sig.length_bits; ++i) {
            indices[i] = sig.start_bit + i;
        }
    } else {
        // Motorola: walk down within byte, wrap to MSB of next byte.
        std::size_t cursor = sig.start_bit;
        for (std::size_t i = 0; i < sig.length_bits; ++i) {
            indices[i] = cursor;
            std::size_t const bit_in_byte = cursor % 8;
            if (bit_in_byte == 0) {
                // Wrap to MSB of the next byte: byte_index + 1, bit 7.
                cursor = cursor + 8 + 7;
            } else {
                cursor -= 1;
            }
        }
    }
    // Bounds check against `data.size() * 8` once we know every index.
    // For Intel order, `indices[i]` is the source of output bit `i`
    // (LSB-first walk). For Motorola order, `indices[0]` is the MSB of
    // the field, so output bit (length-1-i) is the source.
    std::size_t const total_bits = data.size() * 8;
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < sig.length_bits; ++i) {
        std::size_t const bit_index = indices[i];
        if (bit_index >= total_bits)
            return 0;
        std::size_t const byte_index = bit_index / 8;
        std::size_t const bit_in_byte = bit_index % 8;
        std::uint64_t const bit_value = (data[byte_index] >> bit_in_byte) & 0x1U;
        std::size_t const out_bit =
            (sig.byte_order == ByteOrder::Intel) ? i : (sig.length_bits - 1 - i);
        raw |= bit_value << out_bit;
    }
    return raw;
}

double decode_signal(std::span<std::uint8_t const> data, Signal const &sig) noexcept {
    std::uint64_t const raw = extract_raw_bits(data, sig);
    double value;
    if (sig.sign == SignKind::Signed && sig.length_bits > 0 && sig.length_bits < 64) {
        std::uint64_t const sign_bit = 1ULL << (sig.length_bits - 1);
        if ((raw & sign_bit) != 0) {
            // Two's-complement sign extension into 64 bits.
            std::uint64_t const mask =
                ~((sig.length_bits == 64) ? 0ULL : ((1ULL << sig.length_bits) - 1));
            auto const signed_raw = static_cast<std::int64_t>(raw | mask);
            value = static_cast<double>(signed_raw);
        } else {
            value = static_cast<double>(raw);
        }
    } else if (sig.sign == SignKind::Signed && sig.length_bits == 64) {
        value = static_cast<double>(static_cast<std::int64_t>(raw));
    } else {
        value = static_cast<double>(raw);
    }
    return value * sig.factor + sig.offset;
}

} // namespace st::dbc
