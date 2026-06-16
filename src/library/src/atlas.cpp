// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/atlas.hpp"

#include "st/core/error.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

namespace st::library {

namespace {

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

std::vector<std::string> read_string_array(toml::table const &t,
                                            std::string_view key) {
    std::vector<std::string> out;
    auto const *arr = t[key].as_array();
    if (arr == nullptr) {
        return out;
    }
    out.reserve(arr->size());
    for (auto const &node : *arr) {
        if (auto sv = node.value<std::string>(); sv.has_value()) {
            out.push_back(*sv);
        }
    }
    return out;
}

template <typename T>
T read_int(toml::table const &t, std::string_view key, T fallback = 0) {
    auto const v = t[key].value_or<std::int64_t>(static_cast<std::int64_t>(fallback));
    return static_cast<T>(v);
}

std::string read_string(toml::table const &t, std::string_view key) {
    return t[key].value_or<std::string>("");
}

} // namespace

Result<Atlas> Atlas::load_from_file(std::filesystem::path const &path) {
    if (!std::filesystem::exists(path)) {
        return failure(ErrorCode::IoFailure,
                       "atlas: file not found: " + path.string());
    }
    auto const text = read_text(path);
    return load_from_string(text);
}

Result<Atlas> Atlas::load_from_string(std::string_view toml_text) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_text);
    } catch (toml::parse_error const &e) {
        std::string msg{"atlas: TOML parse error: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    Atlas a;
    auto const *header = tbl["atlas"].as_table();
    if (header == nullptr) {
        return failure(ErrorCode::ParseError,
                       "atlas: missing [atlas] header table");
    }
    a.schema_version_ = read_int<int>(*header, "schema_version");
    if (a.schema_version_ > schema_version_supported()) {
        return failure(ErrorCode::UnsupportedVersion,
                       "atlas: schema_version " +
                           std::to_string(a.schema_version_) + " > " +
                           std::to_string(schema_version_supported()));
    }
    a.generated_ = read_string(*header, "generated");
    a.platform_ = read_string(*header, "platform");
    a.primary_cid_ = read_string(*header, "primary_cid");

    if (auto const *arr = tbl["table"].as_array(); arr != nullptr) {
        a.tables_.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *t = node.as_table();
            if (t == nullptr) {
                continue;
            }
            AtlasTable e;
            e.id = read_string(*t, "id");
            e.display_name = read_string(*t, "display_name");
            e.address = read_int<std::uint32_t>(*t, "address");
            e.size_bytes = read_int<std::uint32_t>(*t, "size_bytes");
            e.dimension = read_string(*t, "dimension");
            e.storage = read_string(*t, "storage");
            e.units = read_string(*t, "units");
            e.purpose = read_string(*t, "purpose");
            e.fa24_portability = read_string(*t, "fa24_portability");
            e.needs_def_promotion =
                (*t)["needs_def_promotion"].value_or<bool>(false);
            e.common_core = (*t)["common_core"].value_or<bool>(false);
            e.high_variance = (*t)["high_variance"].value_or<bool>(false);
            e.clusters = read_string_array(*t, "clusters");
            e.co_edits = read_string_array(*t, "co_edits");
            // v2: optional per-table primary_cid override. v1 atlases omit
            // this entirely — caller will fall back to atlas-level
            // primary_cid().
            e.primary_cid = read_string(*t, "primary_cid");
            // v2: optional per-CID address bindings. Schema:
            //   [[table.cid_address]]
            //   cid = "LF9C102P"
            //   address = 0x12345
            //   size_bytes = 16
            if (auto const *cid_arr = (*t)["cid_address"].as_array();
                cid_arr != nullptr) {
                e.cid_addresses.reserve(cid_arr->size());
                for (auto const &cnode : *cid_arr) {
                    auto const *ct = cnode.as_table();
                    if (ct == nullptr) {
                        continue;
                    }
                    AtlasTablePerCid pc;
                    pc.cid = read_string(*ct, "cid");
                    pc.address = read_int<std::uint32_t>(*ct, "address");
                    pc.size_bytes = read_int<std::uint32_t>(*ct, "size_bytes");
                    e.cid_addresses.push_back(std::move(pc));
                }
            }
            a.tables_.push_back(std::move(e));
        }
    }

    if (auto const *arr = tbl["tuner"].as_array(); arr != nullptr) {
        a.tuners_.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *t = node.as_table();
            if (t == nullptr) {
                continue;
            }
            AtlasTuner e;
            e.id = read_string(*t, "id");
            e.display_name = read_string(*t, "display_name");
            e.patches_typical =
                read_int<std::uint32_t>(*t, "patches_typical");
            e.mean_tables_touched =
                read_int<std::uint32_t>(*t, "mean_tables_touched");
            e.philosophy = read_string(*t, "philosophy");
            e.signature_tables = read_string_array(*t, "signature_tables");
            e.skipped_tables = read_string_array(*t, "skipped_tables");
            a.tuners_.push_back(std::move(e));
        }
    }

    if (auto const *arr = tbl["safety_pair"].as_array(); arr != nullptr) {
        a.safety_pairs_.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *t = node.as_table();
            if (t == nullptr) {
                continue;
            }
            AtlasSafetyPair e;
            e.id = read_string(*t, "id");
            e.title = read_string(*t, "title");
            e.severity = read_string(*t, "severity");
            e.lhs_patterns = read_string_array(*t, "lhs_patterns");
            e.rhs_patterns = read_string_array(*t, "rhs_patterns");
            e.rationale = read_string(*t, "rationale");
            a.safety_pairs_.push_back(std::move(e));
        }
    }

    if (auto const *arr = tbl["anchor"].as_array(); arr != nullptr) {
        a.anchors_.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *t = node.as_table();
            if (t == nullptr) {
                continue;
            }
            AtlasAnchor e;
            e.id = read_string(*t, "id");
            e.display_name = read_string(*t, "display_name");
            e.rom_offset_start =
                read_int<std::uint32_t>(*t, "rom_offset_start");
            e.rom_offset_end = read_int<std::uint32_t>(*t, "rom_offset_end");
            e.length = read_int<std::uint32_t>(*t, "length");
            e.prose = read_string(*t, "prose");
            a.anchors_.push_back(std::move(e));
        }
    }

    // v2: top-level [[cid_coverage]] summary. Absent on v1 atlases.
    if (auto const *arr = tbl["cid_coverage"].as_array(); arr != nullptr) {
        a.cid_coverage_.reserve(arr->size());
        for (auto const &node : *arr) {
            auto const *t = node.as_table();
            if (t == nullptr) {
                continue;
            }
            AtlasCidCoverage e;
            e.cid = read_string(*t, "cid");
            e.table_count = read_int<std::uint32_t>(*t, "table_count");
            e.note = read_string(*t, "note");
            a.cid_coverage_.push_back(std::move(e));
        }
    }

    return a;
}

AtlasTable const *Atlas::find_table(std::string_view id) const noexcept {
    for (auto const &e : tables_) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

AtlasTuner const *Atlas::find_tuner(std::string_view id) const noexcept {
    for (auto const &e : tuners_) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

AtlasSafetyPair const *Atlas::find_safety_pair(std::string_view id) const noexcept {
    for (auto const &e : safety_pairs_) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

AtlasAnchor const *Atlas::find_anchor(std::string_view id) const noexcept {
    for (auto const &e : anchors_) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

AtlasCidCoverage const *
Atlas::find_cid_coverage(std::string_view cid) const noexcept {
    for (auto const &e : cid_coverage_) {
        if (e.cid == cid) {
            return &e;
        }
    }
    return nullptr;
}

std::optional<AtlasTablePerCid>
Atlas::find_table_for_cid(std::string_view table_id,
                          std::string_view cid) const noexcept {
    auto const *t = find_table(table_id);
    if (t == nullptr) {
        return std::nullopt;
    }
    // The table's own address binds to its primary_cid (v2) or the
    // atlas-level primary_cid (v1 fallback).
    std::string_view const own_cid =
        t->primary_cid.empty() ? std::string_view{primary_cid_}
                               : std::string_view{t->primary_cid};
    if (own_cid == cid) {
        AtlasTablePerCid out;
        out.cid.assign(own_cid.data(), own_cid.size());
        out.address = t->address;
        out.size_bytes = t->size_bytes;
        return out;
    }
    for (auto const &pc : t->cid_addresses) {
        if (pc.cid == cid) {
            return pc;
        }
    }
    return std::nullopt;
}

AtlasAnchor const *Atlas::anchor_for_offset(std::uint32_t rom_offset) const noexcept {
    for (auto const &e : anchors_) {
        if (e.rom_offset_start == 0 && e.rom_offset_end == 0) {
            // Placeholder anchor (e.g., engine displacement constant on
            // NTM — address varies, not yet pinned). Skip.
            continue;
        }
        if (rom_offset >= e.rom_offset_start && rom_offset < e.rom_offset_end) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace st::library
