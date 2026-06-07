// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/defs.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/rom.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <variant>
#include <vector>

namespace st {

namespace {

std::string read_file(std::filesystem::path const &path, std::error_code &ec) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

template<typename T>
Result<T> require(toml::node const *node, std::string_view path) {
    if (node == nullptr) {
        return failure(ErrorCode::ParseError, "missing required field: " + std::string{path});
    }
    auto const opt = node->value<T>();
    if (!opt.has_value()) {
        return failure(ErrorCode::ParseError, "wrong type for: " + std::string{path});
    }
    return *opt;
}

template<typename T>
T optional_value(toml::table const &t, std::string_view key, T fallback) {
    if (auto const opt = t[key].value<T>(); opt.has_value()) {
        return *opt;
    }
    return fallback;
}

std::vector<std::string> string_array(toml::table const &t, std::string_view key) {
    std::vector<std::string> out;
    if (auto const *arr = t[key].as_array(); arr != nullptr) {
        for (auto const &el : *arr) {
            if (auto const opt = el.value<std::string>(); opt.has_value()) {
                out.push_back(*opt);
            }
        }
    }
    return out;
}

std::vector<int> int_array(toml::table const &t, std::string_view key) {
    std::vector<int> out;
    if (auto const *arr = t[key].as_array(); arr != nullptr) {
        for (auto const &el : *arr) {
            if (auto const opt = el.value<std::int64_t>(); opt.has_value()) {
                out.push_back(static_cast<int>(*opt));
            }
        }
    }
    return out;
}

// Format a toml++ source region as "(line N)" when the parser tracked
// it (always true for tables parsed from text via toml::parse, false
// for nodes constructed in-memory). Empty suffix on no source so the
// caller can append unconditionally without dangling parentheses.
std::string source_suffix(toml::node const &node) {
    auto const &src = node.source();
    if (src.begin.line == 0 && src.begin.column == 0) {
        return {};
    }
    return " (line " + std::to_string(src.begin.line) + ")";
}

Result<toml::table> parse_toml(std::string_view text) {
    try {
        return toml::parse(text);
    } catch (toml::parse_error const &e) {
        std::string msg{"TOML parse error: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
}

std::vector<double> double_array(toml::table const &t, std::string_view key) {
    std::vector<double> out;
    if (auto const *arr = t[key].as_array(); arr != nullptr) {
        for (auto const &el : *arr) {
            if (auto const opt = el.value<double>(); opt.has_value()) {
                out.push_back(*opt);
            } else if (auto const opti = el.value<std::int64_t>(); opti.has_value()) {
                out.push_back(static_cast<double>(*opti));
            }
        }
    }
    return out;
}

Result<DataType> parse_data_type_from(toml::table const &t, std::string_view key) {
    auto const sv = t[key].value<std::string>();
    if (!sv.has_value()) {
        return failure(ErrorCode::ParseError,
                       "missing or non-string data_type at: " + std::string{key});
    }
    return parse_data_type(*sv);
}

Result<Pack> parse_pack(toml::node const &node) {
    auto const *t = node.as_table();
    if (t == nullptr) {
        return failure(ErrorCode::ParseError, "[pack] is not a table");
    }
    Pack p;
    p.schema_version = static_cast<int>(optional_value<std::int64_t>(*t, "schema_version", 1));
    if (auto const v = t->at_path("id").value<std::string>(); v.has_value()) {
        p.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[pack].id is required");
    }
    p.display_name = optional_value<std::string>(*t, "display_name", {});
    p.platform = optional_value<std::string>(*t, "platform", {});
    p.transmission = optional_value<std::string>(*t, "transmission", {});
    p.years = int_array(*t, "years");
    p.endianness = optional_value<std::string>(*t, "endianness", "big");
    p.rom_size_bytes =
        static_cast<std::size_t>(optional_value<std::int64_t>(*t, "rom_size_bytes", 0));
    p.checksum_type = optional_value<std::string>(*t, "checksum_type", {});
    p.authors = string_array(*t, "authors");
    p.data_sources = string_array(*t, "data_sources");
    p.license = optional_value<std::string>(*t, "license", {});
    if (auto const v = t->at_path("extends").value<std::string>(); v.has_value()) {
        p.extends = *v;
    }
    p.includes = string_array(*t, "includes");
    if (p.endianness != "big" && p.endianness != "little") {
        return failure(ErrorCode::ParseError,
                       "[pack].endianness must be 'big' or 'little', got: " + p.endianness +
                           source_suffix(*t));
    }
    return p;
}

Result<Identification> parse_identification(toml::table const &t) {
    Identification id;
    id.name = optional_value<std::string>(t, "name", {});
    id.cid_address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "cid_address", -1));
    id.cid_length = static_cast<std::size_t>(optional_value<std::int64_t>(t, "cid_length", 0));
    id.cid_match = optional_value<std::string>(t, "cid_match", {});
    id.ecu_part = optional_value<std::string>(t, "ecu_part", {});
    id.cid_scan = optional_value<bool>(t, "cid_scan", false);
    if (id.cid_match.empty()) {
        return failure(ErrorCode::ParseError,
                       "[[identification]] cid_match is required (name: " + id.name + ")" +
                           source_suffix(t));
    }
    if (id.cid_length == 0) {
        id.cid_length = id.cid_match.size();
    }
    return id;
}

Result<Axis> parse_axis(toml::table const &t) {
    Axis a;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        a.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[axis]] missing id" + source_suffix(t));
    }
    a.name = optional_value<std::string>(t, "name", {});
    a.unit = optional_value<std::string>(t, "unit", {});
    a.type = optional_value<std::string>(t, "type", "static");
    a.address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "address", 0));
    a.length = static_cast<std::size_t>(optional_value<std::int64_t>(t, "length", 0));
    auto const dt = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    a.data_type = *dt;
    a.scaling = optional_value<std::string>(t, "scaling", {});
    return a;
}

Result<Scaling> parse_scaling(toml::table const &t) {
    Scaling s;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        s.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[scaling]] missing id" + source_suffix(t));
    }
    auto const formula = optional_value<std::string>(t, "formula", "linear");
    if (formula == "linear") {
        LinearScaling lin;
        lin.factor = optional_value<double>(t, "factor", 1.0);
        lin.offset = optional_value<double>(t, "offset", 0.0);
        s.formula = lin;
    } else if (formula == "piecewise") {
        PiecewiseScaling pw;
        pw.breakpoints = double_array(t, "breakpoints");
        pw.values = double_array(t, "values");
        if (pw.breakpoints.empty() || pw.values.empty()) {
            return failure(ErrorCode::ParseError,
                           "piecewise scaling '" + s.id +
                               "' requires non-empty breakpoints and values" +
                               source_suffix(t));
        }
        if (pw.breakpoints.size() != pw.values.size()) {
            return failure(ErrorCode::ParseError,
                           "piecewise scaling '" + s.id +
                               "' breakpoints/values must be same length" +
                               source_suffix(t));
        }
        s.formula = std::move(pw);
    } else if (formula == "subaru_afr_enrichment") {
        // value = numerator / (1 + raw * k). Defaults are the canonical
        // 14.7 stoich / 0.0078125 slope; packs may override either.
        SubaruAfrEnrichment afr;
        afr.numerator = optional_value<double>(t, "numerator", 14.7);
        afr.k = optional_value<double>(t, "k", 0.0078125);
        s.formula = afr;
    } else if (formula == "inverse_divide") {
        // value = numerator / raw. Used by Subaru injector flow scaling
        // (numerator = 2707090) and gear-determination thresholds
        // (numerator = 96560.6).
        InverseDivideScaling inv;
        inv.numerator = optional_value<double>(t, "numerator", 1.0);
        s.formula = inv;
    } else {
        return failure(ErrorCode::ParseError,
                       "scaling '" + s.id + "' unknown formula: " + formula + source_suffix(t));
    }
    s.unit = optional_value<std::string>(t, "unit", {});
    s.min = optional_value<double>(t, "min", 0.0);
    s.max = optional_value<double>(t, "max", 0.0);
    s.precision = static_cast<int>(optional_value<std::int64_t>(t, "precision", 0));

    auto const dt = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    s.data_type = *dt;
    return s;
}

Result<Table> parse_table(toml::table const &t) {
    Table tab;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        tab.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[table]] missing id" + source_suffix(t));
    }
    tab.name = optional_value<std::string>(t, "name", {});
    tab.category = optional_value<std::string>(t, "category", {});
    tab.dimensions = static_cast<int>(optional_value<std::int64_t>(t, "dimensions", 2));
    tab.address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "address", 0));
    auto const dt = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    tab.data_type = *dt;
    tab.scaling = optional_value<std::string>(t, "scaling", {});

    if (auto const v = t["axis_x"].value<std::string>(); v.has_value()) {
        tab.axis_x = *v;
    }
    if (auto const v = t["axis_y"].value<std::string>(); v.has_value()) {
        tab.axis_y = *v;
    }
    if (auto const v = t["axis_z"].value<std::string>(); v.has_value()) {
        tab.axis_z = *v;
    }
    if (auto const v = t["notes"].value<std::string>(); v.has_value()) {
        tab.notes = *v;
    }
    if (auto const v = t["role"].value<std::string>(); v.has_value() && !v->empty()) {
        tab.role = *v;
    }
    tab.emissions_relevant = optional_value<bool>(t, "emissions_relevant", false);
    tab.engine_safety_critical = optional_value<bool>(t, "engine_safety_critical", false);

    if (tab.dimensions < 0 || tab.dimensions > 3) {
        return failure(ErrorCode::ParseError,
                       "table '" + tab.id + "' dimensions must be 0, 1, 2, or 3" +
                           source_suffix(t));
    }
    return tab;
}

Result<Pid> parse_pid(toml::table const &t) {
    Pid p;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        p.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[pid]] missing id" + source_suffix(t));
    }
    p.name = optional_value<std::string>(t, "name", {});
    p.ssm_address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "ssm_address", 0));
    p.length = static_cast<std::size_t>(optional_value<std::int64_t>(t, "length", 0));
    auto const dt = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    p.data_type = *dt;
    p.scaling = optional_value<std::string>(t, "scaling", {});
    p.unit = optional_value<std::string>(t, "unit", {});
    p.default_log = optional_value<bool>(t, "default_log", false);
    p.produces_table = optional_value<std::string>(t, "produces_table", {});
    return p;
}

Result<Switch> parse_switch(toml::table const &t) {
    Switch s;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        s.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[switch]] missing id" + source_suffix(t));
    }
    s.name = optional_value<std::string>(t, "name", {});
    s.ssm_address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "ssm_address", 0));
    s.bit = static_cast<int>(optional_value<std::int64_t>(t, "bit", 0));
    if (s.bit < 0 || s.bit > 7) {
        return failure(ErrorCode::ParseError,
                       "[[switch]] '" + s.id + "' bit must be 0..7" + source_suffix(t));
    }
    s.default_log = optional_value<bool>(t, "default_log", false);
    return s;
}

Result<DtcBitmap> parse_dtc_bitmap(toml::table const &t) {
    DtcBitmap b;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        b.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[dtc_bitmap]] missing id" + source_suffix(t));
    }
    b.name = optional_value<std::string>(t, "name", {});
    b.address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "address", 0));
    b.length_bytes = static_cast<std::size_t>(optional_value<std::int64_t>(t, "length_bytes", 0));
    if (b.length_bytes == 0) {
        return failure(ErrorCode::ParseError,
                       "[[dtc_bitmap]] '" + b.id + "' length_bytes must be > 0");
    }
    b.endianness = optional_value<std::string>(t, "endianness", "big");
    return b;
}

Result<Dtc> parse_dtc(toml::table const &t) {
    Dtc d;
    if (auto const v = t["code"].value<std::string>(); v.has_value()) {
        d.code = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[dtc]] missing code" + source_suffix(t));
    }
    d.name = optional_value<std::string>(t, "name", {});
    if (auto const v = t["bitmap_id"].value<std::string>(); v.has_value()) {
        d.bitmap_id = *v;
    } else {
        return failure(ErrorCode::ParseError,
                       "[[dtc]] '" + d.code + "' missing bitmap_id" + source_suffix(t));
    }
    d.byte_offset = static_cast<std::size_t>(optional_value<std::int64_t>(t, "byte_offset", 0));
    d.bit = static_cast<int>(optional_value<std::int64_t>(t, "bit", 0));
    if (d.bit < 0 || d.bit > 7) {
        return failure(ErrorCode::ParseError,
                       "[[dtc]] '" + d.code + "' bit must be 0..7" + source_suffix(t));
    }
    d.emissions_relevant = optional_value<bool>(t, "emissions_relevant", false);
    return d;
}

Result<HookSignal> parse_signal(toml::node const &n, std::string_view kind,
                                std::string_view owner_id, std::string_view side) {
    auto const *t = n.as_table();
    auto const prefix =
        "[[" + std::string{kind} + "]] '" + std::string{owner_id} + "' " + std::string{side};
    if (t == nullptr) {
        return failure(ErrorCode::ParseError,
                       prefix + " entry is not a table (expected "
                                "{ name = ..., type = ..., ... })" + source_suffix(n));
    }
    HookSignal s;
    if (auto const v = (*t)["name"].value<std::string>(); v.has_value() && !v->empty()) {
        s.name = *v;
    } else {
        return failure(ErrorCode::ParseError,
                       prefix + " entry missing name" + source_suffix(*t));
    }
    s.label = optional_value<std::string>(*t, "label", {});
    if (auto const v = (*t)["type"].value<std::string>(); v.has_value()) {
        s.type = *v;
    } else {
        return failure(ErrorCode::ParseError,
                       prefix + " entry '" + s.name + "' missing type" + source_suffix(*t));
    }
    if (s.type != "float" && s.type != "int" && s.type != "bool") {
        return failure(ErrorCode::ParseError,
                       prefix + " entry '" + s.name + "' type must be float|int|bool, got '" +
                           s.type + "'" + source_suffix(*t));
    }
    s.unit = optional_value<std::string>(*t, "unit", {});
    if (auto const v = (*t)["address"].value<std::int64_t>(); v.has_value()) {
        s.address = static_cast<std::size_t>(*v);
    }
    return s;
}

Result<Hook> parse_hook(toml::table const &t) {
    Hook h;
    if (auto const v = t["id"].value<std::string>(); v.has_value() && !v->empty()) {
        h.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[hook]] missing id" + source_suffix(t));
    }
    h.display_name = optional_value<std::string>(t, "display_name", {});
    h.description = optional_value<std::string>(t, "description", {});

    auto const parse_side = [&](char const *key, std::vector<HookSignal> &out) -> Status {
        auto const *arr = t[key].as_array();
        if (arr == nullptr)
            return ok(); // omitted side is just empty
        for (auto const &el : *arr) {
            auto sig = parse_signal(el, "hook", h.id, key);
            if (!sig.has_value())
                return failure(sig.error());
            out.push_back(std::move(*sig));
        }
        return ok();
    };
    if (auto r = parse_side("inputs", h.inputs); !r.has_value())
        return failure(r.error());
    if (auto r = parse_side("outputs", h.outputs); !r.has_value())
        return failure(r.error());

    if (auto const v = t["ecu_address"].value<std::int64_t>(); v.has_value()) {
        h.ecu_address = static_cast<std::size_t>(*v);
    }
    if (auto const *fr = t["free_ram"].as_table(); fr != nullptr) {
        if (auto const v = (*fr)["base"].value<std::int64_t>(); v.has_value()) {
            h.free_ram_base = static_cast<std::size_t>(*v);
        }
        if (auto const v = (*fr)["length"].value<std::int64_t>(); v.has_value()) {
            h.free_ram_length = static_cast<std::size_t>(*v);
        }
    }
    return h;
}

Result<WritableRegion> parse_writable_region(toml::table const &t) {
    WritableRegion w;
    if (auto const v = t["name"].value<std::string>(); v.has_value() && !v->empty()) {
        w.name = *v;
    } else {
        return failure(ErrorCode::ParseError,
                       "[[writable_region]] missing name" + source_suffix(t));
    }
    if (auto const v = t["kind"].value<std::string>(); v.has_value() && !v->empty()) {
        w.kind = *v;
        if (w.kind != "calibration" && w.kind != "code" && w.kind != "data") {
            std::string msg{"[[writable_region]] '"};
            msg.append(w.name);
            msg.append("' has unknown kind '");
            msg.append(w.kind);
            msg.append("' (expected: calibration | code | data)");
            msg.append(source_suffix(t));
            return failure(ErrorCode::ParseError, std::move(msg));
        }
    } else {
        std::string msg{"[[writable_region]] '"};
        msg.append(w.name);
        msg.append("' missing kind (expected: calibration | code | data)");
        msg.append(source_suffix(t));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    if (auto const v = t["address"].value<std::int64_t>(); v.has_value()) {
        if (*v < 0) {
            // Without this guard, a negative TOML integer would cast
            // to a near-SIZE_MAX size_t and trip the overflow check
            // below with a misleading "overflows the address space"
            // error. Surface the actual mistake.
            std::string msg{"[[writable_region]] '"};
            msg.append(w.name);
            msg.append("' address must be non-negative");
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        w.address = static_cast<std::size_t>(*v);
    } else {
        std::string msg{"[[writable_region]] '"};
        msg.append(w.name);
        msg.append("' missing address");
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    if (auto const v = t["length"].value<std::int64_t>(); v.has_value() && *v > 0) {
        w.length = static_cast<std::size_t>(*v);
    } else {
        std::string msg{"[[writable_region]] '"};
        msg.append(w.name);
        msg.append("' length must be a positive integer");
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    // Reject `address + length` that would overflow the size_t space. This
    // also catches authoring bugs where the user accidentally pasted a
    // 64-bit ROM dump address into a 32-bit firmware-address slot.
    if (w.address > std::numeric_limits<std::size_t>::max() - w.length) {
        std::string msg{"[[writable_region]] '"};
        msg.append(w.name);
        msg.append("' address + length overflows the address space");
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    w.bank = optional_value<std::string>(t, "bank", {});
    w.description = optional_value<std::string>(t, "description", {});
    return w;
}

Result<Primitive> parse_primitive(toml::table const &t) {
    Primitive p;
    if (auto const v = t["id"].value<std::string>(); v.has_value() && !v->empty()) {
        p.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[primitive]] missing id");
    }
    p.display_name = optional_value<std::string>(t, "display_name", {});
    p.description = optional_value<std::string>(t, "description", {});
    auto const parse_side = [&](char const *key, std::vector<HookSignal> &out) -> Status {
        auto const *arr = t[key].as_array();
        if (arr == nullptr)
            return ok();
        for (auto const &el : *arr) {
            auto sig = parse_signal(el, "primitive", p.id, key);
            if (!sig.has_value())
                return failure(sig.error());
            out.push_back(std::move(*sig));
        }
        return ok();
    };
    if (auto r = parse_side("inputs", p.inputs); !r.has_value())
        return failure(r.error());
    if (auto r = parse_side("outputs", p.outputs); !r.has_value())
        return failure(r.error());
    return p;
}

Result<Workflow> parse_workflow(toml::table const &t) {
    Workflow w;
    if (auto const v = t["id"].value<std::string>(); v.has_value() && !v->empty()) {
        w.id = *v;
    } else {
        return failure(ErrorCode::ParseError,
                       "[[workflow]] missing id" + source_suffix(t));
    }
    w.display_name = optional_value<std::string>(t, "display_name", {});
    w.modal = optional_value<std::string>(t, "modal", {});
    auto const *arr = t["required_tables"].as_array();
    if (arr == nullptr) {
        // Empty required_tables is legal — a workflow can declare
        // itself without table prerequisites (e.g. a recipe that
        // only writes ByteEdits or only touches DTC bitmaps). The
        // modal that owns the workflow gates harder if needed.
        return w;
    }
    w.required_tables.reserve(arr->size());
    std::size_t i = 0;
    for (auto const &el : *arr) {
        auto const s = el.value<std::string>();
        if (!s.has_value() || s->empty()) {
            std::string msg{"[[workflow]] '"};
            msg.append(w.id);
            msg.append("' required_tables[");
            msg.append(std::to_string(i));
            msg.append("] is not a non-empty string");
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        w.required_tables.push_back(*s);
        ++i;
    }
    return w;
}

} // namespace

// ---- DataType helpers ----------------------------------------------------

Result<DataType> parse_data_type(std::string_view s) {
    if (s == "uint8")
        return DataType::Uint8;
    if (s == "int8")
        return DataType::Int8;
    if (s == "uint16_be")
        return DataType::Uint16Be;
    if (s == "uint16_le")
        return DataType::Uint16Le;
    if (s == "int16_be")
        return DataType::Int16Be;
    if (s == "int16_le")
        return DataType::Int16Le;
    if (s == "uint32_be")
        return DataType::Uint32Be;
    if (s == "uint32_le")
        return DataType::Uint32Le;
    if (s == "int32_be")
        return DataType::Int32Be;
    if (s == "int32_le")
        return DataType::Int32Le;
    if (s == "float32_be")
        return DataType::Float32Be;
    if (s == "float32_le")
        return DataType::Float32Le;
    return failure(ErrorCode::ParseError, "unknown data_type: " + std::string{s});
}

std::string_view to_string(DataType dt) noexcept {
    switch (dt) {
    case DataType::Uint8:
        return "uint8";
    case DataType::Int8:
        return "int8";
    case DataType::Uint16Be:
        return "uint16_be";
    case DataType::Uint16Le:
        return "uint16_le";
    case DataType::Int16Be:
        return "int16_be";
    case DataType::Int16Le:
        return "int16_le";
    case DataType::Uint32Be:
        return "uint32_be";
    case DataType::Uint32Le:
        return "uint32_le";
    case DataType::Int32Be:
        return "int32_be";
    case DataType::Int32Le:
        return "int32_le";
    case DataType::Float32Be:
        return "float32_be";
    case DataType::Float32Le:
        return "float32_le";
    }
    return "?";
}

std::size_t byte_size(DataType dt) noexcept {
    switch (dt) {
    case DataType::Uint8:
    case DataType::Int8:
        return 1;
    case DataType::Uint16Be:
    case DataType::Uint16Le:
    case DataType::Int16Be:
    case DataType::Int16Le:
        return 2;
    case DataType::Uint32Be:
    case DataType::Uint32Le:
    case DataType::Int32Be:
    case DataType::Int32Le:
    case DataType::Float32Be:
    case DataType::Float32Le:
        return 4;
    }
    return 0;
}

namespace {

// IEEE 754 float32 bit-pattern -> float, host-endianness-agnostic.
float bits_to_float(std::uint32_t bits) noexcept {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

Result<double> read_typed(Rom const &rom, std::size_t offset, DataType dt) {
    switch (dt) {
    case DataType::Uint8: {
        auto const r = rom.read_u8(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(*r);
    }
    case DataType::Int8: {
        auto const r = rom.read_u8(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(static_cast<std::int8_t>(*r));
    }
    case DataType::Uint16Be: {
        auto const r = rom.read_u16_be(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(*r);
    }
    case DataType::Uint16Le: {
        auto const r = rom.read_u16_le(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(*r);
    }
    case DataType::Int16Be: {
        auto const r = rom.read_u16_be(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(static_cast<std::int16_t>(*r));
    }
    case DataType::Int16Le: {
        auto const r = rom.read_u16_le(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(static_cast<std::int16_t>(*r));
    }
    case DataType::Uint32Be: {
        auto const r = rom.read_u32_be(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(*r);
    }
    case DataType::Uint32Le: {
        auto const r = rom.read_u32_le(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(*r);
    }
    case DataType::Int32Be: {
        auto const r = rom.read_u32_be(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(static_cast<std::int32_t>(*r));
    }
    case DataType::Int32Le: {
        auto const r = rom.read_u32_le(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(static_cast<std::int32_t>(*r));
    }
    case DataType::Float32Be: {
        auto const r = rom.read_u32_be(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(bits_to_float(*r));
    }
    case DataType::Float32Le: {
        auto const r = rom.read_u32_le(offset);
        if (!r.has_value())
            return failure(r.error());
        return static_cast<double>(bits_to_float(*r));
    }
    }
    return failure(ErrorCode::InvalidArgument,
                   "unknown DataType enum value " +
                       std::to_string(static_cast<int>(dt)));
}

double apply_scaling(double raw, Scaling const &s) noexcept {
    if (auto const *lin = std::get_if<LinearScaling>(&s.formula); lin != nullptr) {
        return raw * lin->factor + lin->offset;
    }
    if (auto const *afr = std::get_if<SubaruAfrEnrichment>(&s.formula); afr != nullptr) {
        // value = numerator / (1 + raw*k). Guard against the singularity
        // at raw*k == -1 (impossible for the canonical 14.7/(1+x*.0078125)
        // family since raw is uint8 ≥ 0 and k > 0, but defensive anyway).
        double const denom = 1.0 + raw * afr->k;
        if (denom == 0.0)
            return 0.0;
        return afr->numerator / denom;
    }
    if (auto const *inv = std::get_if<InverseDivideScaling>(&s.formula); inv != nullptr) {
        // value = numerator / raw. Guard against the singularity at raw==0.
        if (raw == 0.0)
            return 0.0;
        return inv->numerator / raw;
    }
    auto const *pw = std::get_if<PiecewiseScaling>(&s.formula);
    if (pw == nullptr || pw->breakpoints.empty()) {
        return raw;
    }
    // Linear interpolation between the (breakpoints[i], values[i]) pairs.
    auto const &bp = pw->breakpoints;
    auto const &v = pw->values;
    if (raw <= bp.front())
        return v.front();
    if (raw >= bp.back())
        return v.back();
    for (std::size_t i = 1; i < bp.size(); ++i) {
        if (raw <= bp[i]) {
            double const t = (raw - bp[i - 1]) / (bp[i] - bp[i - 1]);
            return v[i - 1] + t * (v[i] - v[i - 1]);
        }
    }
    return v.back();
}

Result<double> invert_scaling(double engineering, Scaling const &s) {
    if (auto const *lin = std::get_if<LinearScaling>(&s.formula); lin != nullptr) {
        if (lin->factor == 0.0) {
            return failure(ErrorCode::InvalidArgument,
                           "scaling '" + s.id + "' has factor=0; not invertible");
        }
        return (engineering - lin->offset) / lin->factor;
    }
    if (auto const *afr = std::get_if<SubaruAfrEnrichment>(&s.formula); afr != nullptr) {
        // value = numerator / (1 + raw*k) → raw = (numerator/value - 1) / k.
        // Degenerates when k == 0 or engineering == 0.
        if (afr->k == 0.0) {
            return failure(ErrorCode::InvalidArgument,
                           "scaling '" + s.id + "' has k=0; not invertible");
        }
        if (engineering == 0.0) {
            return failure(ErrorCode::InvalidArgument,
                           "scaling '" + s.id + "' value=0 maps to raw=±∞; not invertible");
        }
        return (afr->numerator / engineering - 1.0) / afr->k;
    }
    if (auto const *inv = std::get_if<InverseDivideScaling>(&s.formula); inv != nullptr) {
        // value = numerator / raw → raw = numerator / value. Degenerates
        // at engineering == 0 (maps to raw=±∞).
        if (engineering == 0.0) {
            return failure(ErrorCode::InvalidArgument,
                           "scaling '" + s.id + "' value=0 maps to raw=±∞; not invertible");
        }
        return inv->numerator / engineering;
    }
    auto const *pw = std::get_if<PiecewiseScaling>(&s.formula);
    if (pw == nullptr || pw->values.size() < 2) {
        return failure(ErrorCode::InvalidArgument,
                       "scaling '" + s.id + "' is degenerate; not invertible");
    }
    // Piecewise inverse: locate the value segment containing `engineering`
    // and interpolate back to the breakpoint axis. Assumes a monotonic
    // values[] (otherwise the inverse is not a function).
    auto const &bp = pw->breakpoints;
    auto const &v = pw->values;
    bool const ascending = v.back() >= v.front();
    if (ascending) {
        if (engineering <= v.front())
            return bp.front();
        if (engineering >= v.back())
            return bp.back();
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (engineering <= v[i]) {
                double const span = v[i] - v[i - 1];
                if (span == 0.0)
                    return bp[i - 1];
                double const t = (engineering - v[i - 1]) / span;
                return bp[i - 1] + t * (bp[i] - bp[i - 1]);
            }
        }
        return bp.back();
    }
    // Descending: mirror the loop direction.
    if (engineering >= v.front())
        return bp.front();
    if (engineering <= v.back())
        return bp.back();
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (engineering >= v[i]) {
            double const span = v[i] - v[i - 1];
            if (span == 0.0)
                return bp[i - 1];
            double const t = (engineering - v[i - 1]) / span;
            return bp[i - 1] + t * (bp[i] - bp[i - 1]);
        }
    }
    return bp.back();
}

namespace {

// Clamp `value` to the inclusive integer range [lo, hi] and round to nearest.
template<typename T>
T clamp_round(double value, double lo, double hi) noexcept {
    if (value < lo)
        value = lo;
    if (value > hi)
        value = hi;
    // Round half away from zero, which matches what most calibrators expect.
    double const rounded = value >= 0 ? value + 0.5 : value - 0.5;
    return static_cast<T>(rounded);
}

} // namespace

Status write_typed(Rom &rom, std::size_t offset, DataType dt, double value) {
    switch (dt) {
    case DataType::Uint8:
        return rom.write_u8(offset, clamp_round<std::uint8_t>(value, 0.0, 255.0));
    case DataType::Int8: {
        auto const v = clamp_round<std::int8_t>(value, -128.0, 127.0);
        return rom.write_u8(offset, static_cast<std::uint8_t>(v));
    }
    case DataType::Uint16Be:
        return rom.write_u16_be(offset, clamp_round<std::uint16_t>(value, 0.0, 65535.0));
    case DataType::Uint16Le:
        return rom.write_u16_le(offset, clamp_round<std::uint16_t>(value, 0.0, 65535.0));
    case DataType::Int16Be: {
        auto const v = clamp_round<std::int16_t>(value, -32768.0, 32767.0);
        return rom.write_u16_be(offset, static_cast<std::uint16_t>(v));
    }
    case DataType::Int16Le: {
        auto const v = clamp_round<std::int16_t>(value, -32768.0, 32767.0);
        return rom.write_u16_le(offset, static_cast<std::uint16_t>(v));
    }
    case DataType::Uint32Be:
        return rom.write_u32_be(offset, clamp_round<std::uint32_t>(value, 0.0, 4294967295.0));
    case DataType::Uint32Le:
        return rom.write_u32_le(offset, clamp_round<std::uint32_t>(value, 0.0, 4294967295.0));
    case DataType::Int32Be: {
        auto const v = clamp_round<std::int32_t>(value, -2147483648.0, 2147483647.0);
        return rom.write_u32_be(offset, static_cast<std::uint32_t>(v));
    }
    case DataType::Int32Le: {
        auto const v = clamp_round<std::int32_t>(value, -2147483648.0, 2147483647.0);
        return rom.write_u32_le(offset, static_cast<std::uint32_t>(v));
    }
    case DataType::Float32Be: {
        std::uint32_t bits = 0;
        float const f = static_cast<float>(value);
        std::memcpy(&bits, &f, sizeof(bits));
        return rom.write_u32_be(offset, bits);
    }
    case DataType::Float32Le: {
        std::uint32_t bits = 0;
        float const f = static_cast<float>(value);
        std::memcpy(&bits, &f, sizeof(bits));
        return rom.write_u32_le(offset, bits);
    }
    }
    return failure(ErrorCode::InvalidArgument,
                   "unknown DataType enum value " +
                       std::to_string(static_cast<int>(dt)));
}

// ---- Definition ----------------------------------------------------------

// DefinitionBuilder is the friend that bridges free-function parse helpers
// in this TU and Definition's private state. Members are static so callers
// don't have to thread instances around.
class DefinitionBuilder {
public:
    // Parse a single TOML table and merge its arrays into `def`. If
    // accept_pack is true, also reads a top-level [pack] section; otherwise
    // [pack] in the table is silently ignored (so secondary files in a
    // directory pack can include one without effect).
    static Status merge(toml::table const &tbl, Definition &def, bool accept_pack,
                        bool require_pack) {
        auto const *pack_node = tbl.get("pack");
        if (pack_node != nullptr && accept_pack) {
            auto pack_r = parse_pack(*pack_node);
            if (!pack_r.has_value())
                return failure(pack_r.error());
            def.pack_ = std::move(*pack_r);
        } else if (accept_pack && require_pack && pack_node == nullptr) {
            return failure(ErrorCode::ParseError, "missing [pack] section");
        }

        auto const visit_array = [&](std::string_view key, auto parse_one, auto &dst) -> Status {
            auto const *arr = tbl[key].as_array();
            if (arr == nullptr)
                return ok();
            for (auto const &el : *arr) {
                auto const *t = el.as_table();
                if (t == nullptr) {
                    return failure(ErrorCode::ParseError,
                                   "element of " + std::string{key} + " is not a table");
                }
                auto r = parse_one(*t);
                if (!r.has_value())
                    return failure(r.error());
                dst.push_back(std::move(*r));
            }
            return ok();
        };

        if (auto r = visit_array("identification", parse_identification, def.ids_);
            !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("axis", parse_axis, def.axes_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("scaling", parse_scaling, def.scalings_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("table", parse_table, def.tables_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("pid", parse_pid, def.pids_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("switch", parse_switch, def.switches_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("dtc_bitmap", parse_dtc_bitmap, def.dtc_bitmaps_);
            !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("dtc", parse_dtc, def.dtcs_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("hook", parse_hook, def.hooks_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("primitive", parse_primitive, def.primitives_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("writable_region", parse_writable_region, def.writable_regions_);
            !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("workflow", parse_workflow, def.workflows_); !r.has_value()) {
            return failure(r.error());
        }
        return ok();
    }

    // Apply `child` on top of `parent` per the inheritance rules in
    // docs/11-definition-format.md: child's [pack] header wins entirely;
    // axes / scalings / tables / pids merge by `id` (child overrides
    // same-id parent entries; new ids are appended); identifications
    // append (no merge by name).
    static Definition merge_over(Definition parent, Definition &&child) {
        parent.pack_ = std::move(child.pack_);
        parent.pack_.extends.reset();

        for (auto &id : child.ids_) {
            parent.ids_.push_back(std::move(id));
        }

        auto const upsert = [](auto &dst, auto &src) {
            for (auto &el : src) {
                auto it = std::find_if(dst.begin(), dst.end(),
                                       [&](auto const &x) { return x.id == el.id; });
                if (it == dst.end()) {
                    dst.push_back(std::move(el));
                } else {
                    *it = std::move(el);
                }
            }
        };
        upsert(parent.axes_, child.axes_);
        upsert(parent.scalings_, child.scalings_);
        upsert(parent.tables_, child.tables_);
        upsert(parent.pids_, child.pids_);
        upsert(parent.switches_, child.switches_);
        upsert(parent.dtc_bitmaps_, child.dtc_bitmaps_);
        upsert(parent.hooks_, child.hooks_);
        upsert(parent.primitives_, child.primitives_);
        // Workflows merge by id — child override / append same as the
        // other id-keyed kinds. A child pack adding a workflow its
        // parent doesn't declare is the common case (FA24 swap on
        // lf79103p, inherited via extends by lf79101p).
        upsert(parent.workflows_, child.workflows_);
        // WritableRegion is keyed by `name` rather than `id`. Same upsert
        // semantics — child's same-named region overrides parent's, new
        // names append.
        for (auto &el : child.writable_regions_) {
            auto it = std::find_if(parent.writable_regions_.begin(),
                                   parent.writable_regions_.end(),
                                   [&](WritableRegion const &x) { return x.name == el.name; });
            if (it == parent.writable_regions_.end()) {
                parent.writable_regions_.push_back(std::move(el));
            } else {
                *it = std::move(el);
            }
        }
        // DTCs are keyed by `code` rather than `id`; same upsert semantics
        // but a different key field.
        for (auto &el : child.dtcs_) {
            auto it = std::find_if(parent.dtcs_.begin(), parent.dtcs_.end(),
                                   [&](Dtc const &x) { return x.code == el.code; });
            if (it == parent.dtcs_.end()) {
                parent.dtcs_.push_back(std::move(el));
            } else {
                *it = std::move(el);
            }
        }
        return parent;
    }

    // Scan sibling directories of `child_dir` for one whose pack.toml
    // declares `[pack].id == target_id`. Returns its path or an error.
    static Result<std::filesystem::path>
    find_sibling_pack_dir(std::filesystem::path const &child_dir, std::string_view target_id) {
        auto const parent = child_dir.parent_path();
        if (parent.empty()) {
            return failure(ErrorCode::FileNotFound,
                           "cannot resolve 'extends = " + std::string{target_id} +
                               "': no parent directory of " + child_dir.string());
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(parent, ec) || ec) {
            return failure(ErrorCode::FileNotFound,
                           "cannot resolve 'extends = " + std::string{target_id} +
                               "': search root is not a directory: " + parent.string());
        }

        for (auto const &entry : std::filesystem::directory_iterator{parent, ec}) {
            if (ec)
                break;
            if (!entry.is_directory(ec) || ec)
                continue;
            auto const candidate_manifest = entry.path() / "pack.toml";
            if (!std::filesystem::exists(candidate_manifest, ec) || ec)
                continue;

            std::error_code read_ec;
            std::string const contents = read_file(candidate_manifest, read_ec);
            if (read_ec)
                continue;

            auto tbl = parse_toml(contents);
            if (!tbl.has_value())
                continue;
            auto const id_node = (*tbl)["pack"]["id"].value<std::string>();
            if (id_node.has_value() && *id_node == target_id) {
                return entry.path();
            }
        }
        return failure(ErrorCode::FileNotFound,
                       "cannot resolve 'extends = " + std::string{target_id} +
                           "': no sibling pack with that id");
    }

    // Load a definition directory with cycle-protected `extends` resolution.
    // `visited` tracks pack ids reached so far in the inheritance chain.
    static Result<Definition> load_chain(std::filesystem::path const &path,
                                         std::vector<std::string> &visited) {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec) || ec) {
            return failure(ErrorCode::InvalidArgument, "not a directory: " + path.string());
        }

        auto const manifest = path / "pack.toml";
        if (!std::filesystem::exists(manifest, ec) || ec) {
            return failure(ErrorCode::FileNotFound, "pack.toml missing in: " + path.string());
        }

        Definition def;

        auto const ingest = [&](std::filesystem::path const &p, bool accept_pack,
                                bool require_pack) -> Status {
            std::error_code read_ec;
            std::string const contents = read_file(p, read_ec);
            if (read_ec) {
                return failure(ErrorCode::IoFailure, "cannot read " + p.string());
            }
            auto tbl = parse_toml(contents);
            if (!tbl.has_value())
                return failure(tbl.error());
            return merge(*tbl, def, accept_pack, require_pack);
        };

        if (auto r = ingest(manifest, /*accept_pack=*/true, /*require_pack=*/true);
            !r.has_value()) {
            return failure(r.error());
        }

        std::vector<std::filesystem::path> others;
        for (auto const &entry : std::filesystem::recursive_directory_iterator{path, ec}) {
            if (ec) {
                return failure(ErrorCode::IoFailure, "walk failed in: " + path.string());
            }
            if (!entry.is_regular_file(ec) || ec)
                continue;
            auto const ep = entry.path();
            if (ep.extension() != ".toml")
                continue;
            if (std::filesystem::equivalent(ep, manifest, ec))
                continue;
            others.push_back(ep);
        }
        std::sort(others.begin(), others.end());
        for (auto const &p : others) {
            if (auto r = ingest(p, /*accept_pack=*/false, /*require_pack=*/false); !r.has_value()) {
                return failure(r.error());
            }
        }

        if (!def.pack_.extends.has_value()) {
            return def;
        }

        auto const &parent_id = *def.pack_.extends;
        if (std::find(visited.begin(), visited.end(), parent_id) != visited.end()) {
            return failure(ErrorCode::ParseError, "'extends' cycle detected: pack '" +
                                                      def.pack_.id + "' extends already-visited '" +
                                                      parent_id + "'");
        }
        auto parent_path = find_sibling_pack_dir(path, parent_id);
        if (!parent_path.has_value())
            return failure(parent_path.error());

        visited.push_back(def.pack_.id);
        auto parent_def = load_chain(*parent_path, visited);
        visited.pop_back();
        if (!parent_def.has_value())
            return failure(parent_def.error());

        return merge_over(std::move(*parent_def), std::move(def));
    }
};

Result<Definition> Definition::from_toml_string(std::string_view toml) {
    auto tbl = parse_toml(toml);
    if (!tbl.has_value())
        return failure(tbl.error());

    Definition def;
    if (auto r = DefinitionBuilder::merge(*tbl, def, /*accept_pack=*/true,
                                          /*require_pack=*/true);
        !r.has_value()) {
        return failure(r.error());
    }
    return def;
}

namespace {
// Walk `pack.includes` relative to `base_dir`, parse each fragment, merge
// records-only into `def`. `visited` carries the canonical paths reached so
// far in the recursion; revisits raise ParseError. Fragments may themselves
// declare `includes` and we walk those depth-first.
Status resolve_includes(Definition &def, std::vector<std::string> const &includes,
                        std::filesystem::path const &base_dir,
                        std::vector<std::filesystem::path> &visited) {
    for (auto const &rel : includes) {
        std::error_code ec;
        auto const candidate = base_dir / rel;
        auto const canon = std::filesystem::weakly_canonical(candidate, ec);
        auto const resolved = ec ? candidate : canon;
        if (std::find(visited.begin(), visited.end(), resolved) != visited.end()) {
            return failure(ErrorCode::ParseError,
                           "include cycle detected at: " + resolved.string());
        }
        std::error_code read_ec;
        std::string const contents = read_file(resolved, read_ec);
        if (read_ec) {
            return failure(ErrorCode::FileNotFound, "include not found: " + resolved.string());
        }
        auto tbl = parse_toml(contents);
        if (!tbl.has_value()) {
            return failure(ErrorCode::ParseError, "include parse: " + resolved.string() + ": " +
                                                      std::string{tbl.error().message()});
        }
        // Record-level merge only — the fragment's own [pack] is informational
        // (gives the file a loadable id for stand-alone `pack-info`) but does
        // NOT replace the parent pack metadata. A malformed fragment [pack]
        // is still an error: we'd otherwise silently lose nested includes.
        Pack frag_pack;
        bool has_frag_includes = false;
        if (auto const *pack_node = tbl->get("pack"); pack_node != nullptr) {
            auto r = parse_pack(*pack_node);
            if (!r.has_value()) {
                return failure(ErrorCode::ParseError, "include " + resolved.string() + ": " +
                                                          std::string{r.error().message()});
            }
            frag_pack = std::move(*r);
            has_frag_includes = !frag_pack.includes.empty();
        }
        if (auto r = DefinitionBuilder::merge(*tbl, def, /*accept_pack=*/false,
                                              /*require_pack=*/false);
            !r.has_value()) {
            return failure(r.error());
        }
        if (has_frag_includes) {
            visited.push_back(resolved);
            auto status =
                resolve_includes(def, frag_pack.includes, resolved.parent_path(), visited);
            visited.pop_back();
            if (!status.has_value())
                return failure(status.error());
        }
    }
    return ok();
}
} // namespace

namespace {
// Scan sibling `*.toml` files in `file_path.parent_path()` for one whose
// `[pack].id` matches `target_id`. Used by from_file's extends-resolution
// step — counterpart to DefinitionBuilder::find_sibling_pack_dir for the
// flat-file layout (one .toml per pack, all in the same directory) that
// the impreza/ tree uses today. Subdirectory layouts continue to flow
// through from_directory's load_chain path.
Result<std::filesystem::path>
find_sibling_pack_file(std::filesystem::path const &file_path, std::string_view target_id) {
    auto const parent = file_path.parent_path();
    if (parent.empty()) {
        return failure(ErrorCode::FileNotFound,
                       "cannot resolve 'extends = " + std::string{target_id} +
                           "': no parent directory of " + file_path.string());
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec) || ec) {
        return failure(ErrorCode::FileNotFound,
                       "cannot resolve 'extends = " + std::string{target_id} +
                           "': search root is not a directory: " + parent.string());
    }
    for (auto const &entry : std::filesystem::directory_iterator{parent, ec}) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec) || ec)
            continue;
        auto const ep = entry.path();
        if (ep.extension() != ".toml")
            continue;
        // Skip self so we don't recurse into our own file.
        if (std::filesystem::equivalent(ep, file_path, ec))
            continue;
        std::error_code read_ec;
        std::string const contents = read_file(ep, read_ec);
        if (read_ec)
            continue;
        auto tbl = parse_toml(contents);
        if (!tbl.has_value())
            continue;
        auto const id_node = (*tbl)["pack"]["id"].value<std::string>();
        if (id_node.has_value() && *id_node == target_id) {
            return ep;
        }
    }
    return failure(ErrorCode::FileNotFound,
                   "cannot resolve 'extends = " + std::string{target_id} +
                       "': no sibling .toml file with [pack].id matching in " +
                       parent.string());
}

// from_file implementation parameterised by a `visited` chain for cycle
// protection across recursive extends resolution. Public from_file
// wraps this with a fresh visited list.
Result<Definition> from_file_impl(std::filesystem::path const &path,
                                  std::vector<std::string> &visited) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        return Definition::from_directory(path);
    }
    std::string const contents = read_file(path, ec);
    if (ec) {
        return failure(ErrorCode::FileNotFound, path.string());
    }
    auto def_r = Definition::from_toml_string(contents);
    if (!def_r.has_value())
        return def_r;
    auto def = std::move(*def_r);
    if (!def.pack().includes.empty()) {
        auto const canon = std::filesystem::weakly_canonical(path, ec);
        auto const self = ec ? path : canon;
        std::vector<std::filesystem::path> includes_visited{self};
        if (auto status =
                resolve_includes(def, def.pack().includes, self.parent_path(), includes_visited);
            !status.has_value()) {
            return failure(status.error());
        }
    }
    // Single-file extends resolution. Mirrors the directory loader's
    // chain semantics: parent is found by scanning siblings for a
    // matching [pack].id, then we recurse into it (so a multi-level
    // chain like A → B → C resolves cleanly), then merge over. Without
    // this, packs that lean on inheritance (e.g. lf79101p extends
    // lf79103p) silently lose every inherited table when loaded by
    // Project::open which uses from_file with a single-file path.
    if (!def.pack().extends.has_value()) {
        return def;
    }
    auto const parent_id = *def.pack().extends;
    if (std::find(visited.begin(), visited.end(), parent_id) != visited.end()) {
        return failure(ErrorCode::ParseError, "'extends' cycle detected: pack '" +
                                                  def.pack().id + "' extends already-visited '" +
                                                  parent_id + "'");
    }
    auto parent_path = find_sibling_pack_file(path, parent_id);
    if (!parent_path.has_value())
        return failure(parent_path.error());

    visited.push_back(def.pack().id);
    auto parent_def = from_file_impl(*parent_path, visited);
    visited.pop_back();
    if (!parent_def.has_value())
        return failure(parent_def.error());

    return DefinitionBuilder::merge_over(std::move(*parent_def), std::move(def));
}
} // namespace

Result<Definition> Definition::from_file(std::filesystem::path const &path) {
    std::vector<std::string> visited;
    return from_file_impl(path, visited);
}

Result<Definition> Definition::from_directory(std::filesystem::path const &path) {
    std::vector<std::string> visited;
    return DefinitionBuilder::load_chain(path, visited);
}

Status Definition::validate() const {
    std::string violations;
    auto const note = [&](std::string s) {
        if (!violations.empty()) {
            violations.push_back('\n');
        }
        violations.append(std::move(s));
    };

    auto const rom_size = pack_.rom_size_bytes;
    auto const fits = [rom_size](std::size_t addr, std::size_t bytes) {
        if (rom_size == 0) {
            return true; // unspecified ROM size; defer the check
        }
        return addr <= rom_size && rom_size - addr >= bytes;
    };

    for (auto const &a : axes_) {
        if (!a.scaling.empty() && find_scaling(a.scaling) == nullptr) {
            note("axis '" + a.id + "' references unknown scaling '" + a.scaling + "'");
        }
        if (!fits(a.address, a.length * byte_size(a.data_type))) {
            note("axis '" + a.id + "' extends past rom_size_bytes");
        }
    }
    for (auto const &t : tables_) {
        if (!t.scaling.empty() && find_scaling(t.scaling) == nullptr) {
            note("table '" + t.id + "' references unknown scaling '" + t.scaling + "'");
        }
        auto const check_axis = [&](char which, std::optional<std::string> const &ax) {
            if (ax.has_value() && !ax->empty() && find_axis(*ax) == nullptr) {
                note(std::string{"table '"} + t.id + "' axis_" + which +
                     " references unknown axis '" + *ax + "'");
            }
        };
        check_axis('x', t.axis_x);
        check_axis('y', t.axis_y);
        check_axis('z', t.axis_z);

        if (t.dimensions >= 1 && !t.axis_x.has_value()) {
            note("table '" + t.id +
                 "' requires axis_x for dimensions=" + std::to_string(t.dimensions));
        }
        if (t.dimensions >= 2 && !t.axis_y.has_value()) {
            note("table '" + t.id +
                 "' requires axis_y for dimensions=" + std::to_string(t.dimensions));
        }
        if (t.dimensions >= 3 && !t.axis_z.has_value()) {
            note("table '" + t.id +
                 "' requires axis_z for dimensions=" + std::to_string(t.dimensions));
        }
        // We can't compute table extent without resolved axes; bounds-check
        // the base address only.
        if (rom_size != 0 && t.address >= rom_size) {
            note("table '" + t.id + "' address is past rom_size_bytes");
        }
    }
    for (auto const &p : pids_) {
        if (!p.scaling.empty() && find_scaling(p.scaling) == nullptr) {
            note("pid '" + p.id + "' references unknown scaling '" + p.scaling + "'");
        }
        // Issue #15 — validate the live-to-table cross-reference. A
        // pid pointing at a non-existent table makes the gauge cluster
        // emit a "table not in pack" toast when the user right-clicks
        // it. Catch typos at load time.
        if (!p.produces_table.empty() && find_table(p.produces_table) == nullptr) {
            note("pid '" + p.id + "' produces_table references unknown table '" +
                 p.produces_table + "'");
        }
    }
    for (auto const &id : ids_) {
        // cid_scan mode searches the whole ROM, so cid_address is
        // ignored and the fixed-offset bounds check doesn't apply.
        if (!id.cid_scan && rom_size != 0 && !fits(id.cid_address, id.cid_length)) {
            note("identification '" + id.name + "' cid_address extends past rom_size_bytes");
        }
    }
    for (auto const &b : dtc_bitmaps_) {
        if (!fits(b.address, b.length_bytes)) {
            note("dtc_bitmap '" + b.id + "' extends past rom_size_bytes");
        }
    }
    for (auto const &d : dtcs_) {
        auto const *bm = find_dtc_bitmap(d.bitmap_id);
        if (bm == nullptr) {
            note("dtc '" + d.code + "' references unknown bitmap '" + d.bitmap_id + "'");
            continue;
        }
        if (d.byte_offset >= bm->length_bytes) {
            note("dtc '" + d.code + "' byte_offset " + std::to_string(d.byte_offset) +
                 " is outside bitmap '" + bm->id +
                 "' (length_bytes=" + std::to_string(bm->length_bytes) + ")");
        }
    }

    // Duplicate-id checks for collections whose lookup helpers
    // (`find_hook`, `find_primitive`) return the first match. A
    // duplicate would silently shadow the second entry, leaving the
    // pack author with hard-to-diagnose codegen / address-gate
    // surprises. Same goes for `writable_region.name` — the address
    // gate iterates the vector linearly and a duplicate name could
    // make an audit trail look correct while the actual gate decision
    // was made by a different entry than the one a user inspected.
    //
    // Empty ids are not flagged here — that's a different class of
    // bad pack, handled (or not) by the loader's per-entry parsing.
    auto const check_duplicates =
        [&](char const *kind, auto getter, auto const &collection) {
            std::unordered_set<std::string_view> seen;
            seen.reserve(collection.size());
            for (auto const &entry : collection) {
                std::string_view const key = getter(entry);
                if (key.empty()) {
                    continue;
                }
                auto const [_, inserted] = seen.emplace(key);
                if (!inserted) {
                    note(std::string{kind} + " '" + std::string{key} +
                         "' is defined more than once");
                }
            }
        };
    check_duplicates("hook", [](Hook const &h) { return std::string_view{h.id}; }, hooks_);
    check_duplicates(
        "primitive", [](Primitive const &p) { return std::string_view{p.id}; }, primitives_);
    check_duplicates(
        "writable_region", [](WritableRegion const &w) { return std::string_view{w.name}; },
        writable_regions_);
    // Same hazard for `role`: `find_table_by_role` returns the first
    // match, so a duplicate would silently shadow the second tagged
    // table. Tables without a role return std::string_view{} from the
    // getter and are skipped by the existing key.empty() guard.
    check_duplicates(
        "table role",
        [](Table const &t) {
            return t.role.has_value() ? std::string_view{*t.role} : std::string_view{};
        },
        tables_);

    if (!violations.empty()) {
        return failure(ErrorCode::ParseError, std::move(violations));
    }
    return ok();
}

Axis const *Definition::find_axis(std::string_view id) const noexcept {
    auto it = std::find_if(axes_.begin(), axes_.end(), [&](Axis const &a) { return a.id == id; });
    return it == axes_.end() ? nullptr : &*it;
}

Scaling const *Definition::find_scaling(std::string_view id) const noexcept {
    auto it = std::find_if(scalings_.begin(), scalings_.end(),
                           [&](Scaling const &s) { return s.id == id; });
    return it == scalings_.end() ? nullptr : &*it;
}

Table const *Definition::find_table(std::string_view id) const noexcept {
    auto it =
        std::find_if(tables_.begin(), tables_.end(), [&](Table const &t) { return t.id == id; });
    return it == tables_.end() ? nullptr : &*it;
}

Table const *Definition::find_table_by_role(std::string_view role) const noexcept {
    if (role.empty()) {
        return nullptr;
    }
    auto it = std::find_if(tables_.begin(), tables_.end(), [&](Table const &t) {
        return t.role.has_value() && *t.role == role;
    });
    return it == tables_.end() ? nullptr : &*it;
}

Pid const *Definition::find_pid(std::string_view id) const noexcept {
    auto it = std::find_if(pids_.begin(), pids_.end(), [&](Pid const &p) { return p.id == id; });
    return it == pids_.end() ? nullptr : &*it;
}

std::vector<Pid const *>
Definition::find_pids_producing(std::string_view table_id) const noexcept {
    std::vector<Pid const *> out;
    if (table_id.empty()) {
        return out;
    }
    for (auto const &p : pids_) {
        if (p.produces_table == table_id) {
            out.push_back(&p);
        }
    }
    return out;
}

Switch const *Definition::find_switch(std::string_view id) const noexcept {
    auto it = std::find_if(switches_.begin(), switches_.end(),
                           [&](Switch const &s) { return s.id == id; });
    return it == switches_.end() ? nullptr : &*it;
}

DtcBitmap const *Definition::find_dtc_bitmap(std::string_view id) const noexcept {
    auto it = std::find_if(dtc_bitmaps_.begin(), dtc_bitmaps_.end(),
                           [&](DtcBitmap const &b) { return b.id == id; });
    return it == dtc_bitmaps_.end() ? nullptr : &*it;
}

Dtc const *Definition::find_dtc(std::string_view code) const noexcept {
    auto it =
        std::find_if(dtcs_.begin(), dtcs_.end(), [&](Dtc const &d) { return d.code == code; });
    return it == dtcs_.end() ? nullptr : &*it;
}

Hook const *Definition::find_hook(std::string_view id) const noexcept {
    auto it = std::find_if(hooks_.begin(), hooks_.end(), [&](Hook const &h) { return h.id == id; });
    return it == hooks_.end() ? nullptr : &*it;
}

Primitive const *Definition::find_primitive(std::string_view id) const noexcept {
    auto it = std::find_if(primitives_.begin(), primitives_.end(),
                           [&](Primitive const &p) { return p.id == id; });
    return it == primitives_.end() ? nullptr : &*it;
}

Workflow const *Definition::find_workflow(std::string_view id) const noexcept {
    auto it = std::find_if(workflows_.begin(), workflows_.end(),
                           [&](Workflow const &w) { return w.id == id; });
    return it == workflows_.end() ? nullptr : &*it;
}

bool Definition::supports_workflow(std::string_view id) const noexcept {
    auto const *w = find_workflow(id);
    if (w == nullptr) {
        return false;
    }
    for (auto const &table_id : w->required_tables) {
        if (find_table(table_id) == nullptr) {
            return false;
        }
    }
    return true;
}

namespace {
Result<std::size_t> dtc_byte_offset(Rom const &rom, DtcBitmap const &bitmap, Dtc const &dtc) {
    if (dtc.byte_offset >= bitmap.length_bytes) {
        return failure(ErrorCode::OutOfRange,
                       "dtc '" + dtc.code + "' byte_offset is outside its bitmap");
    }
    auto const offset = bitmap.address + dtc.byte_offset;
    if (offset >= rom.size()) {
        return failure(ErrorCode::OutOfRange,
                       "dtc '" + dtc.code + "' bitmap byte is past end of ROM");
    }
    return offset;
}
} // namespace

Result<bool> is_dtc_enabled(Rom const &rom, DtcBitmap const &bitmap, Dtc const &dtc) {
    auto const off = dtc_byte_offset(rom, bitmap, dtc);
    if (!off.has_value())
        return failure(off.error());
    auto const byte = rom.read_u8(*off);
    if (!byte.has_value())
        return failure(byte.error());
    return ((*byte >> dtc.bit) & 1U) != 0U;
}

Result<DtcBitChange> set_dtc_enabled(Rom &rom, DtcBitmap const &bitmap, Dtc const &dtc,
                                     bool enabled) {
    auto const off = dtc_byte_offset(rom, bitmap, dtc);
    if (!off.has_value())
        return failure(off.error());
    auto const cur = rom.read_u8(*off);
    if (!cur.has_value())
        return failure(cur.error());
    DtcBitChange ch{};
    ch.address = *off;
    ch.before = *cur;
    auto const mask = static_cast<std::uint8_t>(1U << dtc.bit);
    ch.after = enabled ? static_cast<std::uint8_t>(ch.before | mask)
                       : static_cast<std::uint8_t>(ch.before & ~mask);
    if (auto s = rom.write_u8(*off, ch.after); !s.has_value()) {
        return failure(s.error());
    }
    return ch;
}

Result<std::vector<double>> Definition::read_axis_values(Rom const &rom, Axis const &axis) const {
    std::vector<double> out;
    out.reserve(axis.length);

    auto const step = byte_size(axis.data_type);
    auto const scaling = find_scaling(axis.scaling);

    for (std::size_t i = 0; i < axis.length; ++i) {
        auto const raw = read_typed(rom, axis.address + i * step, axis.data_type);
        if (!raw.has_value()) {
            return failure(raw.error());
        }
        out.push_back(scaling != nullptr ? apply_scaling(*raw, *scaling) : *raw);
    }
    return out;
}

Result<Definition::TableData> Definition::read_table_values(Rom const &rom,
                                                            Table const &table) const {
    TableData td;

    // Resolve axes that this table actually uses.
    auto const resolve_axis = [&](std::optional<std::string> const &name,
                                  char ch) -> Result<std::vector<double>> {
        if (!name.has_value() || name->empty())
            return std::vector<double>{};
        auto const *a = find_axis(*name);
        if (a == nullptr) {
            return failure(ErrorCode::ParseError, std::string{"table '"} + table.id +
                                                      "' references unknown axis_" + ch + " '" +
                                                      *name + "'");
        }
        return read_axis_values(rom, *a);
    };

    if (table.dimensions >= 1) {
        auto xs = resolve_axis(table.axis_x, 'x');
        if (!xs.has_value())
            return failure(xs.error());
        td.axis_x = std::move(*xs);
    }
    if (table.dimensions >= 2) {
        auto ys = resolve_axis(table.axis_y, 'y');
        if (!ys.has_value())
            return failure(ys.error());
        td.axis_y = std::move(*ys);
    }
    if (table.dimensions >= 3) {
        auto zs = resolve_axis(table.axis_z, 'z');
        if (!zs.has_value())
            return failure(zs.error());
        td.axis_z = std::move(*zs);
    }

    auto const cols = td.axis_x.empty() ? std::size_t{1} : td.axis_x.size();
    auto const rows = td.axis_y.empty() ? std::size_t{1} : td.axis_y.size();
    auto const step = byte_size(table.data_type);
    auto const scal = find_scaling(table.scaling);

    auto const read_cell = [&](std::size_t off, double &out) -> Status {
        auto const raw = read_typed(rom, off, table.data_type);
        if (!raw.has_value())
            return failure(raw.error());
        out = (scal != nullptr) ? apply_scaling(*raw, *scal) : *raw;
        return ok();
    };

    if (table.dimensions == 3) {
        auto const depth = td.axis_z.empty() ? std::size_t{1} : td.axis_z.size();
        td.slices.assign(depth,
                         std::vector<std::vector<double>>(rows, std::vector<double>(cols, 0.0)));
        for (std::size_t z = 0; z < depth; ++z) {
            for (std::size_t r = 0; r < rows; ++r) {
                for (std::size_t c = 0; c < cols; ++c) {
                    auto const off = table.address + ((z * rows + r) * cols + c) * step;
                    if (auto s = read_cell(off, td.slices[z][r][c]); !s.has_value()) {
                        return failure(s.error());
                    }
                }
            }
        }
        return td;
    }

    td.values.assign(rows, std::vector<double>(cols, 0.0));
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            auto const off = table.address + (r * cols + c) * step;
            if (auto s = read_cell(off, td.values[r][c]); !s.has_value()) {
                return failure(s.error());
            }
        }
    }
    return td;
}

Status Definition::write_table_values(Rom &rom, Table const &table, TableData const &td) const {
    Axis const *ax = nullptr;
    Axis const *ay = nullptr;
    Axis const *az = nullptr;
    auto const resolve = [&](std::optional<std::string> const &name,
                             char ch) -> Result<Axis const *> {
        if (!name.has_value() || name->empty())
            return static_cast<Axis const *>(nullptr);
        auto const *a = find_axis(*name);
        if (a == nullptr) {
            return failure(ErrorCode::ParseError, std::string{"table '"} + table.id +
                                                      "' references unknown axis_" + ch + " '" +
                                                      *name + "'");
        }
        return a;
    };
    if (table.dimensions >= 1) {
        auto r = resolve(table.axis_x, 'x');
        if (!r.has_value())
            return failure(r.error());
        ax = *r;
    }
    if (table.dimensions >= 2) {
        auto r = resolve(table.axis_y, 'y');
        if (!r.has_value())
            return failure(r.error());
        ay = *r;
    }
    if (table.dimensions >= 3) {
        auto r = resolve(table.axis_z, 'z');
        if (!r.has_value())
            return failure(r.error());
        az = *r;
    }

    auto const cols = ax == nullptr ? std::size_t{1} : ax->length;
    auto const rows = ay == nullptr ? std::size_t{1} : ay->length;
    auto const depth = az == nullptr ? std::size_t{1} : az->length;

    auto const step = byte_size(table.data_type);
    auto const *scal = find_scaling(table.scaling);

    auto const write_cell = [&](std::size_t off, double eng) -> Status {
        double raw = eng;
        if (scal != nullptr) {
            auto const inv = invert_scaling(eng, *scal);
            if (!inv.has_value())
                return failure(inv.error());
            raw = *inv;
        }
        return write_typed(rom, off, table.data_type, raw);
    };

    if (table.dimensions == 3) {
        if (td.slices.size() != depth) {
            return failure(ErrorCode::InvalidArgument,
                           "TableData has " + std::to_string(td.slices.size()) +
                               " slices but table expects " + std::to_string(depth));
        }
        for (std::size_t z = 0; z < depth; ++z) {
            if (td.slices[z].size() != rows) {
                return failure(ErrorCode::InvalidArgument,
                               "TableData slice " + std::to_string(z) + " has " +
                                   std::to_string(td.slices[z].size()) +
                                   " rows but table expects " + std::to_string(rows));
            }
            for (std::size_t r = 0; r < rows; ++r) {
                if (td.slices[z][r].size() != cols) {
                    return failure(ErrorCode::InvalidArgument,
                                   "TableData slice " + std::to_string(z) + " row " +
                                       std::to_string(r) + " has " +
                                       std::to_string(td.slices[z][r].size()) +
                                       " cols but table expects " + std::to_string(cols));
                }
                for (std::size_t c = 0; c < cols; ++c) {
                    auto const off = table.address + ((z * rows + r) * cols + c) * step;
                    if (auto s = write_cell(off, td.slices[z][r][c]); !s.has_value()) {
                        return s;
                    }
                }
            }
        }
        return ok();
    }

    if (td.values.size() != rows) {
        return failure(ErrorCode::InvalidArgument,
                       "TableData has " + std::to_string(td.values.size()) +
                           " rows but table expects " + std::to_string(rows));
    }
    for (std::size_t r = 0; r < rows; ++r) {
        if (td.values[r].size() != cols) {
            return failure(ErrorCode::InvalidArgument,
                           "TableData row " + std::to_string(r) + " has " +
                               std::to_string(td.values[r].size()) + " cols but table expects " +
                               std::to_string(cols));
        }
        for (std::size_t c = 0; c < cols; ++c) {
            auto const off = table.address + (r * cols + c) * step;
            if (auto s = write_cell(off, td.values[r][c]); !s.has_value()) {
                return s;
            }
        }
    }
    return ok();
}

Result<Definition::TableDiff> Definition::diff_table(Rom const &a, Rom const &b,
                                                     Table const &table) const {
    auto ta = read_table_values(a, table);
    if (!ta.has_value())
        return failure(ta.error());
    auto tb = read_table_values(b, table);
    if (!tb.has_value())
        return failure(tb.error());

    if (ta->values.size() != tb->values.size()) {
        return failure(ErrorCode::Unknown, "table '" + table.id + "': row count mismatch");
    }

    TableDiff diff;
    double abs_sum = 0.0;
    for (std::size_t r = 0; r < ta->values.size(); ++r) {
        if (ta->values[r].size() != tb->values[r].size()) {
            return failure(ErrorCode::Unknown, "table '" + table.id +
                                                   "': column count mismatch on row " +
                                                   std::to_string(r));
        }
        for (std::size_t c = 0; c < ta->values[r].size(); ++c) {
            ++diff.total_cells;
            double const d = tb->values[r][c] - ta->values[r][c];
            if (d != 0.0) {
                ++diff.cells_changed;
                double const ad = d < 0 ? -d : d;
                abs_sum += ad;
                if (ad > diff.max_abs_delta)
                    diff.max_abs_delta = ad;
            }
        }
    }
    if (diff.cells_changed > 0) {
        diff.mean_abs_delta = abs_sum / static_cast<double>(diff.cells_changed);
    }
    return diff;
}

std::optional<Definition::MatchInfo> Definition::match_info(Rom const &rom) const {
    for (auto const &id : ids_) {
        if (id.cid_scan) {
            // Scan mode: search the ROM for any occurrence of cid_match.
            // Used by FA-DIT WRX firmware where the CID descriptor lives
            // at a variable per-firmware offset rather than a fixed
            // 0x2000-style address. The first occurrence anywhere in
            // the ROM bytes counts as a match; the offset is reported
            // so callers can show where it was found.
            auto const haystack = rom.slice(0, rom.size());
            if (!haystack.has_value() || id.cid_match.empty())
                continue;
            std::string_view const hay{reinterpret_cast<char const *>(haystack->data()),
                                       haystack->size()};
            auto const pos = hay.find(id.cid_match);
            if (pos != std::string_view::npos) {
                return MatchInfo{id.name, pos, /*scanned=*/true};
            }
            continue;
        }
        auto const slice = rom.slice(id.cid_address, id.cid_length);
        if (!slice.has_value()) {
            continue;
        }
        std::string_view const got{reinterpret_cast<char const *>(slice->data()), slice->size()};
        if (got == id.cid_match) {
            return MatchInfo{id.name, id.cid_address, /*scanned=*/false};
        }
    }
    return std::nullopt;
}

std::optional<std::string> Definition::matches(Rom const &rom) const {
    if (auto info = match_info(rom); info.has_value()) {
        return info->name;
    }
    return std::nullopt;
}

} // namespace st
