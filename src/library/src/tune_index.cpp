// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/tune_index.hpp"

#include "st/config.hpp"
#include "st/core/error.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

namespace st::library {

namespace {

constexpr std::string_view kSupportedSchema = "subuwutuner.library-index.v1";

std::string read_text(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        return {};
    }
    auto const len = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::string buf(len, '\0');
    if (len > 0) {
        in.read(buf.data(), static_cast<std::streamsize>(len));
    }
    return buf;
}

std::optional<std::string> environment_value(char const *name) {
#ifdef _WIN32
    char *value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr || *value == '\0') {
        std::free(value);
        return std::nullopt;
    }
    std::string result{value};
    std::free(value);
    return result;
#else
    char const *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

std::string lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    auto const hay_lower = lower_ascii(hay);
    auto const needle_lower = lower_ascii(needle);
    return hay_lower.find(needle_lower) != std::string::npos;
}

std::string take_string(toml::table const &t, char const *key) {
    auto const v = t[key].value<std::string>();
    return v.value_or(std::string{});
}

std::uint64_t take_u64(toml::table const &t, char const *key) {
    auto const v = t[key].value<std::int64_t>();
    if (!v.has_value() || *v < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(*v);
}

Result<TuneIndex> parse_text_into_index(std::string_view text,
                                         std::filesystem::path const &source_path) {
    toml::table doc;
    try {
        doc = toml::parse(text);
    } catch (toml::parse_error const &e) {
        std::string msg = "tune index parse error: ";
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    auto const schema = doc["schema"].value_or<std::string>("");
    if (schema != kSupportedSchema) {
        std::string msg = "tune index schema mismatch: expected '";
        msg.append(kSupportedSchema);
        msg.append("', got '");
        msg.append(schema.empty() ? "<missing>" : schema);
        msg.append("'");
        return failure(ErrorCode::UnsupportedVersion, std::move(msg));
    }
    // Build local vectors then construct the TuneIndex via its
    // factory. Construction itself goes through a path with a public
    // "ingest" helper to keep private state encapsulated.
    std::vector<TuneIndexEntry> ptms;
    std::vector<StuneIndexEntry> stunes;
    if (auto const *arr = doc["ptm"].as_array(); arr != nullptr) {
        ptms.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *tbl = node.as_table();
            if (tbl == nullptr) {
                continue;
            }
            TuneIndexEntry e;
            e.path = take_string(*tbl, "path");
            e.size = take_u64(*tbl, "size");
            e.mtime = take_u64(*tbl, "mtime");
            e.md5 = lower_ascii(take_string(*tbl, "md5"));
            e.vendor = take_string(*tbl, "vendor");
            e.stage = take_string(*tbl, "stage");
            e.fuel_grade = take_string(*tbl, "fuel_grade");
            e.variant = take_string(*tbl, "variant");
            e.unlocked_sibling = take_string(*tbl, "unlocked_sibling");
            if (e.path.empty()) {
                continue;
            }
            ptms.push_back(std::move(e));
        }
    }
    if (auto const *arr = doc["stune"].as_array(); arr != nullptr) {
        stunes.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *tbl = node.as_table();
            if (tbl == nullptr) {
                continue;
            }
            StuneIndexEntry e;
            e.path = take_string(*tbl, "path");
            e.project_name = take_string(*tbl, "project_name");
            e.ptm_vendor_id = take_string(*tbl, "ptm_vendor_id");
            e.ptm_vehicle_id = take_string(*tbl, "ptm_vehicle_id");
            e.ptm_rom_sum = take_string(*tbl, "ptm_rom_sum");
            e.source_bin = take_string(*tbl, "source_bin");
            e.source_bin_md5 = lower_ascii(take_string(*tbl, "source_bin_md5"));
            if (e.path.empty()) {
                continue;
            }
            stunes.push_back(std::move(e));
        }
    }
    return TuneIndex::construct_(schema, source_path,
                                  std::move(ptms), std::move(stunes));
}

} // namespace

Result<TuneIndex> TuneIndex::load_from_file(std::filesystem::path const &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return failure(ErrorCode::FileNotFound,
                       "tune index not found: " + path.string());
    }
    auto const text = read_text(path);
    if (text.empty()) {
        return failure(ErrorCode::IoFailure,
                       "tune index read failed: " + path.string());
    }
    return parse_text_into_index(text, path);
}

Result<TuneIndex> TuneIndex::load_from_string(std::string_view toml_text) {
    return parse_text_into_index(toml_text, std::filesystem::path{});
}

TuneIndex TuneIndex::construct_(std::string schema,
                                 std::filesystem::path source_path,
                                 std::vector<TuneIndexEntry> ptm_entries,
                                 std::vector<StuneIndexEntry> stune_entries) {
    TuneIndex idx;
    idx.schema_ = std::move(schema);
    idx.source_path_ = std::move(source_path);
    idx.ptm_entries_ = std::move(ptm_entries);
    idx.stune_entries_ = std::move(stune_entries);
    return idx;
}

TuneIndexEntry const *TuneIndex::lookup_by_md5(std::string_view md5) const noexcept {
    if (md5.empty()) {
        return nullptr;
    }
    auto const needle = lower_ascii(md5);
    for (auto const &e : ptm_entries_) {
        if (e.md5 == needle) {
            return &e;
        }
    }
    return nullptr;
}

std::vector<std::size_t> TuneIndex::filter_by_name(std::string_view needle) const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < ptm_entries_.size(); ++i) {
        // Filter against the basename only — full paths are noisy and
        // every entry in the same library dir shares the same prefix.
        auto const &p = ptm_entries_[i].path;
        auto const slash = p.find_last_of("/\\");
        std::string_view const basename =
            (slash == std::string::npos) ? std::string_view{p}
                                          : std::string_view{p}.substr(slash + 1);
        if (needle.empty() || icontains(basename, needle)) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<std::size_t> TuneIndex::filter_by_vendor(std::string_view vendor) const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < ptm_entries_.size(); ++i) {
        if (vendor.empty() || icontains(ptm_entries_[i].vendor, vendor)) {
            out.push_back(i);
        }
    }
    return out;
}

std::optional<std::filesystem::path> default_index_path() {
    std::error_code ec;
    // 1. Env override first — explicit absolute-path-to-index. Lets a
    // user with a non-standard layout point us at any index file.
    if (auto const raw = environment_value("ST_LIBRARY_INDEX")) {
        std::filesystem::path candidate{*raw};
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
    }
    // 2. Config-driven library root. `st::config::library_root()` walks
    // override > ST_LIBRARY_ROOT env > config-file value > built-in
    // default. We look for `library_index.toml` inside that dir.
    {
        auto const root = st::config::library_root();
        if (!root.empty()) {
            auto candidate = root / "library_index.toml";
            if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                return candidate;
            }
        }
    }
    // 3. CWD-relative as a final fallback for users running from a
    // checkout with the index written next to them.
    std::filesystem::path cwd_path{"library_index.toml"};
    if (std::filesystem::is_regular_file(cwd_path, ec) && !ec) {
        return std::filesystem::absolute(cwd_path, ec);
    }
    return std::nullopt;
}

} // namespace st::library
