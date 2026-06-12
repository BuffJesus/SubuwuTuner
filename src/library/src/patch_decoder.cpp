// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/patch_decoder.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/devices/ap3/ptm_cipher.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::library {

namespace {

bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view s) noexcept {
    std::size_t b = 0;
    while (b < s.size() && is_ws(s[b])) ++b;
    std::size_t e = s.size();
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool is_name_char(char c) noexcept {
    auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == ':' || c == '-' || c == '.';
}

// Skip XML prologue / comments / processing instructions starting at
// `pos`. Returns the position past them. Tolerates a leading BOM.
std::size_t skip_meta(std::string_view xml, std::size_t pos) {
    if (pos + 3 <= xml.size() && static_cast<unsigned char>(xml[pos]) == 0xEFU &&
        static_cast<unsigned char>(xml[pos + 1]) == 0xBBU &&
        static_cast<unsigned char>(xml[pos + 2]) == 0xBFU) {
        pos += 3; // UTF-8 BOM
    }
    while (pos < xml.size()) {
        while (pos < xml.size() && is_ws(xml[pos])) ++pos;
        if (pos + 5 <= xml.size() && xml.substr(pos, 4) == "<!--") {
            auto end = xml.find("-->", pos + 4);
            if (end == std::string_view::npos) return xml.size();
            pos = end + 3;
            continue;
        }
        if (pos + 2 <= xml.size() && xml[pos] == '<' && xml[pos + 1] == '?') {
            auto end = xml.find("?>", pos + 2);
            if (end == std::string_view::npos) return xml.size();
            pos = end + 2;
            continue;
        }
        if (pos + 2 <= xml.size() && xml[pos] == '<' && xml[pos + 1] == '!') {
            // <!DOCTYPE ...> etc.
            auto end = xml.find('>', pos + 2);
            if (end == std::string_view::npos) return xml.size();
            pos = end + 1;
            continue;
        }
        break;
    }
    return pos;
}

struct ParsedTag {
    std::string_view name;
    std::vector<std::pair<std::string_view, std::string_view>> attrs;
    bool self_closing{false};
    // For non-self-closing tags: range [content_start, content_end)
    // of the text/children between the opening and closing tag.
    std::size_t content_start{0};
    std::size_t content_end{0};
    // Position immediately past the closing `>` (or `/>` for
    // self-closing) — useful for "advance to next sibling".
    std::size_t end_of_close{0};
};

std::optional<std::string_view> attr(ParsedTag const &t, std::string_view key) noexcept {
    for (auto const &kv : t.attrs) {
        if (kv.first == key) return kv.second;
    }
    return std::nullopt;
}

// Parse the next start-tag at or after `pos`. Skips intervening
// whitespace, comments, and processing instructions. Returns
// `nullopt` if the next non-meta character is not `<` (e.g. EOF) —
// the caller decides whether that's an error.
//
// For non-self-closing tags this also locates the matching `</name>`
// closing tag and sets the content range. Nested same-name elements
// are NOT supported (the PrivateData schema has no recursive nesting).
Result<std::optional<ParsedTag>> next_tag(std::string_view xml, std::size_t pos) {
    pos = skip_meta(xml, pos);
    if (pos >= xml.size()) return std::optional<ParsedTag>{};
    if (xml[pos] != '<') {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: expected '<' at offset " + std::to_string(pos));
    }
    if (pos + 1 < xml.size() && xml[pos + 1] == '/') {
        // Closing tag where a start tag was expected.
        return std::optional<ParsedTag>{};
    }
    ParsedTag tag{};
    std::size_t cur = pos + 1;
    std::size_t const name_start = cur;
    while (cur < xml.size() && is_name_char(xml[cur])) ++cur;
    if (cur == name_start) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: empty tag name at offset " + std::to_string(pos));
    }
    tag.name = xml.substr(name_start, cur - name_start);
    while (cur < xml.size()) {
        while (cur < xml.size() && is_ws(xml[cur])) ++cur;
        if (cur >= xml.size()) {
            return st::failure(st::ErrorCode::ParseError,
                               "patch_decoder: unterminated <" + std::string(tag.name) + ">");
        }
        if (xml[cur] == '/') {
            if (cur + 1 >= xml.size() || xml[cur + 1] != '>') {
                return st::failure(st::ErrorCode::ParseError,
                                   "patch_decoder: malformed self-close in <" +
                                       std::string(tag.name) + ">");
            }
            tag.self_closing = true;
            cur += 2;
            tag.content_start = cur;
            tag.content_end = cur;
            tag.end_of_close = cur;
            return std::optional<ParsedTag>{std::move(tag)};
        }
        if (xml[cur] == '>') {
            ++cur;
            tag.content_start = cur;
            // Find closing </name>. We don't handle same-name nesting.
            std::string const closer = std::string("</") + std::string(tag.name);
            auto end = xml.find(closer, cur);
            if (end == std::string_view::npos) {
                return st::failure(st::ErrorCode::ParseError,
                                   "patch_decoder: unterminated <" + std::string(tag.name) + ">");
            }
            tag.content_end = end;
            // Skip past `</name>` allowing whitespace before `>`.
            cur = end + closer.size();
            while (cur < xml.size() && is_ws(xml[cur])) ++cur;
            if (cur >= xml.size() || xml[cur] != '>') {
                return st::failure(st::ErrorCode::ParseError,
                                   "patch_decoder: malformed </" + std::string(tag.name) +
                                       "> closing tag");
            }
            ++cur;
            tag.end_of_close = cur;
            return std::optional<ParsedTag>{std::move(tag)};
        }
        // Attribute: name="value" or name='value'.
        std::size_t const an_start = cur;
        while (cur < xml.size() && is_name_char(xml[cur])) ++cur;
        if (cur == an_start) {
            return st::failure(st::ErrorCode::ParseError,
                               "patch_decoder: malformed attribute in <" +
                                   std::string(tag.name) + ">");
        }
        auto attr_name = xml.substr(an_start, cur - an_start);
        if (cur >= xml.size() || xml[cur] != '=') {
            return st::failure(st::ErrorCode::ParseError,
                               "patch_decoder: missing '=' after attribute in <" +
                                   std::string(tag.name) + ">");
        }
        ++cur;
        char const quote = (cur < xml.size()) ? xml[cur] : '\0';
        if (quote != '"' && quote != '\'') {
            return st::failure(st::ErrorCode::ParseError,
                               "patch_decoder: missing quote on attribute value in <" +
                                   std::string(tag.name) + ">");
        }
        ++cur;
        std::size_t const av_start = cur;
        while (cur < xml.size() && xml[cur] != quote) ++cur;
        if (cur >= xml.size()) {
            return st::failure(st::ErrorCode::ParseError,
                               "patch_decoder: unterminated attribute value in <" +
                                   std::string(tag.name) + ">");
        }
        tag.attrs.emplace_back(attr_name, xml.substr(av_start, cur - av_start));
        ++cur;
    }
    return st::failure(st::ErrorCode::ParseError,
                       "patch_decoder: unexpected EOF inside <" + std::string(tag.name) + ">");
}

template <typename T>
Result<T> parse_int(std::string_view s, std::string_view what) {
    auto trimmed = trim(s);
    if (trimmed.empty()) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: " + std::string(what) + " is empty");
    }
    T value{};
    auto const *first = trimmed.data();
    auto const *last = trimmed.data() + trimmed.size();
    auto [p, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || p != last) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: " + std::string(what) + " not an integer ('" +
                               std::string(trimmed) + "')");
    }
    return value;
}

} // namespace

Result<DecodedPtm> decode_ptm_xml(std::string_view xml) {
    if (xml.empty()) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: empty input");
    }

    auto root_r = next_tag(xml, 0);
    if (!root_r.has_value()) {
        return st::failure(std::move(root_r).error());
    }
    if (!root_r->has_value()) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: no root element");
    }
    auto const &root = root_r->value();
    if (root.name != "PrivateData") {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: root is <" + std::string(root.name) +
                               ">, expected <PrivateData>");
    }

    DecodedPtm out{};
    // Defaults per spec §step-1.
    out.metadata.save_date_time = "0";

    bool saw_vendor = false;
    bool saw_vehicle = false;
    bool saw_rom_sum = false;
    bool saw_patches = false;

    std::size_t cur = root.content_start;
    while (cur < root.content_end) {
        // Skip leading whitespace; stop if we hit </PrivateData>.
        while (cur < root.content_end && is_ws(xml[cur])) ++cur;
        if (cur >= root.content_end) break;
        if (xml[cur] != '<') {
            return st::failure(st::ErrorCode::ParseError,
                               "patch_decoder: stray text inside <PrivateData>");
        }
        if (cur + 1 < root.content_end && xml[cur + 1] == '/') break;

        auto child_r = next_tag(xml.substr(0, root.content_end), cur);
        if (!child_r.has_value()) {
            return st::failure(std::move(child_r).error());
        }
        if (!child_r->has_value()) break;
        auto const &child = child_r->value();
        cur = child.end_of_close;

        if (child.name == "vendorID") {
            out.metadata.vendor_id = std::string(trim(
                xml.substr(child.content_start, child.content_end - child.content_start)));
            saw_vendor = true;
        } else if (child.name == "vehicleID") {
            out.metadata.vehicle_id = std::string(trim(
                xml.substr(child.content_start, child.content_end - child.content_start)));
            saw_vehicle = true;
        } else if (child.name == "lock") {
            auto m = attr(child, "mask");
            if (m.has_value()) {
                auto parsed = parse_int<std::uint32_t>(*m, "<lock mask>");
                if (!parsed.has_value()) {
                    return st::failure(std::move(parsed).error());
                }
                out.metadata.lock_mask = *parsed;
            }
        } else if (child.name == "romSum") {
            auto v = attr(child, "value");
            if (!v.has_value()) {
                return st::failure(st::ErrorCode::ParseError,
                                   "patch_decoder: <romSum> missing value attribute");
            }
            out.metadata.rom_sum = std::string(trim(*v));
            saw_rom_sum = true;
        } else if (child.name == "saveDateTime") {
            auto v = attr(child, "value");
            out.metadata.save_date_time = v.has_value() ? std::string(trim(*v)) : "0";
        } else if (child.name == "patches") {
            saw_patches = true;
            std::size_t pcur = child.content_start;
            while (pcur < child.content_end) {
                while (pcur < child.content_end && is_ws(xml[pcur])) ++pcur;
                if (pcur >= child.content_end) break;
                if (xml[pcur] != '<') {
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: stray text inside <patches>");
                }
                if (pcur + 1 < child.content_end && xml[pcur + 1] == '/') break;
                auto patch_r = next_tag(xml.substr(0, child.content_end), pcur);
                if (!patch_r.has_value()) {
                    return st::failure(std::move(patch_r).error());
                }
                if (!patch_r->has_value()) break;
                auto const &patch_tag = patch_r->value();
                pcur = patch_tag.end_of_close;

                if (patch_tag.name != "patch") {
                    // Unknown element inside <patches>; skip but
                    // surface as parse error to stay strict.
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: unexpected <" +
                                           std::string(patch_tag.name) + "> inside <patches>");
                }

                auto rom_off_s = attr(patch_tag, "romOffset");
                auto ram_off_s = attr(patch_tag, "ramOffset");
                auto len_s = attr(patch_tag, "length");
                if (!rom_off_s.has_value() || !ram_off_s.has_value() || !len_s.has_value()) {
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: <patch> missing required attribute "
                                       "(romOffset / ramOffset / length)");
                }
                PatchEntry entry{};
                auto rom_off = parse_int<std::uint32_t>(*rom_off_s, "<patch romOffset>");
                if (!rom_off.has_value()) {
                    return st::failure(std::move(rom_off).error());
                }
                entry.rom_offset = *rom_off;
                auto ram_off = parse_int<std::int32_t>(*ram_off_s, "<patch ramOffset>");
                if (!ram_off.has_value()) {
                    return st::failure(std::move(ram_off).error());
                }
                entry.ram_offset = *ram_off;
                auto len = parse_int<std::uint32_t>(*len_s, "<patch length>");
                if (!len.has_value()) {
                    return st::failure(std::move(len).error());
                }
                entry.length = *len;
                if (entry.length == 0U) {
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: <patch length=\"0\"> at rom_offset " +
                                           std::to_string(entry.rom_offset));
                }
                if (entry.length > kMaxPatchLengthBytes) {
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: <patch length=" +
                                           std::to_string(entry.length) + "> exceeds the " +
                                           std::to_string(kMaxPatchLengthBytes) +
                                           "-byte sanity cap at rom_offset " +
                                           std::to_string(entry.rom_offset));
                }
                auto text = trim(xml.substr(patch_tag.content_start,
                                            patch_tag.content_end - patch_tag.content_start));
                auto decoded = st::devices::ap3::cipher::base64_decode(text);
                if (!decoded.has_value()) {
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: <patch> body base64-decode failed at "
                                       "rom_offset " +
                                           std::to_string(entry.rom_offset));
                }
                if (decoded->size() != entry.length) {
                    return st::failure(st::ErrorCode::ParseError,
                                       "patch_decoder: <patch> decoded " +
                                           std::to_string(decoded->size()) +
                                           " bytes but length attribute says " +
                                           std::to_string(entry.length) + " at rom_offset " +
                                           std::to_string(entry.rom_offset));
                }
                entry.bytes = std::move(*decoded);
                out.patches.push_back(std::move(entry));
            }
        } else {
            // Tolerate unknown PrivateData child elements (firmware
            // versions may add new metadata fields).
        }
    }

    if (!saw_vendor) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: <vendorID> missing");
    }
    if (!saw_vehicle) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: <vehicleID> missing");
    }
    if (!saw_rom_sum) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: <romSum> missing");
    }
    if (!saw_patches) {
        return st::failure(st::ErrorCode::ParseError,
                           "patch_decoder: <patches> missing");
    }

    return out;
}

} // namespace st::library
