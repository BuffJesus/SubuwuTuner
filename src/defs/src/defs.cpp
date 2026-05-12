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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

template <typename T>
Result<T> require(toml::node const *node, std::string_view path) {
    if (node == nullptr) {
        return failure(ErrorCode::ParseError,
                       "missing required field: " + std::string{path});
    }
    auto const opt = node->value<T>();
    if (!opt.has_value()) {
        return failure(ErrorCode::ParseError, "wrong type for: " + std::string{path});
    }
    return *opt;
}

template <typename T>
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
    p.display_name   = optional_value<std::string>(*t, "display_name", {});
    p.platform       = optional_value<std::string>(*t, "platform", {});
    p.transmission   = optional_value<std::string>(*t, "transmission", {});
    p.years          = int_array(*t, "years");
    p.endianness     = optional_value<std::string>(*t, "endianness", "big");
    p.rom_size_bytes = static_cast<std::size_t>(
        optional_value<std::int64_t>(*t, "rom_size_bytes", 0));
    p.authors      = string_array(*t, "authors");
    p.data_sources = string_array(*t, "data_sources");
    p.license      = optional_value<std::string>(*t, "license", {});
    if (auto const v = t->at_path("extends").value<std::string>(); v.has_value()) {
        p.extends = *v;
    }
    if (p.endianness != "big" && p.endianness != "little") {
        return failure(ErrorCode::ParseError,
                       "[pack].endianness must be 'big' or 'little', got: " + p.endianness);
    }
    return p;
}

Result<Identification> parse_identification(toml::table const &t) {
    Identification id;
    id.name        = optional_value<std::string>(t, "name", {});
    id.cid_address = static_cast<std::size_t>(
        optional_value<std::int64_t>(t, "cid_address", -1));
    id.cid_length  = static_cast<std::size_t>(optional_value<std::int64_t>(t, "cid_length", 0));
    id.cid_match   = optional_value<std::string>(t, "cid_match", {});
    id.ecu_part    = optional_value<std::string>(t, "ecu_part", {});
    if (id.cid_match.empty()) {
        return failure(ErrorCode::ParseError,
                       "[[identification]] cid_match is required (name: " + id.name + ")");
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
        return failure(ErrorCode::ParseError, "[[axis]] missing id");
    }
    a.name    = optional_value<std::string>(t, "name", {});
    a.unit    = optional_value<std::string>(t, "unit", {});
    a.type    = optional_value<std::string>(t, "type", "static");
    a.address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "address", 0));
    a.length  = static_cast<std::size_t>(optional_value<std::int64_t>(t, "length", 0));
    auto const dt = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    a.data_type = *dt;
    a.scaling   = optional_value<std::string>(t, "scaling", {});
    return a;
}

Result<Scaling> parse_scaling(toml::table const &t) {
    Scaling s;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        s.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[scaling]] missing id");
    }
    auto const formula = optional_value<std::string>(t, "formula", "linear");
    if (formula == "linear") {
        LinearScaling lin;
        lin.factor = optional_value<double>(t, "factor", 1.0);
        lin.offset = optional_value<double>(t, "offset", 0.0);
        s.formula  = lin;
    } else if (formula == "piecewise") {
        PiecewiseScaling pw;
        pw.breakpoints = double_array(t, "breakpoints");
        pw.values      = double_array(t, "values");
        if (pw.breakpoints.empty() || pw.values.empty()) {
            return failure(ErrorCode::ParseError,
                           "piecewise scaling '" + s.id
                               + "' requires non-empty breakpoints and values");
        }
        if (pw.breakpoints.size() != pw.values.size()) {
            return failure(ErrorCode::ParseError,
                           "piecewise scaling '" + s.id
                               + "' breakpoints/values must be same length");
        }
        s.formula = std::move(pw);
    } else {
        return failure(ErrorCode::ParseError,
                       "scaling '" + s.id + "' unknown formula: " + formula);
    }
    s.unit      = optional_value<std::string>(t, "unit", {});
    s.min       = optional_value<double>(t, "min", 0.0);
    s.max       = optional_value<double>(t, "max", 0.0);
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
        return failure(ErrorCode::ParseError, "[[table]] missing id");
    }
    tab.name       = optional_value<std::string>(t, "name", {});
    tab.category   = optional_value<std::string>(t, "category", {});
    tab.dimensions = static_cast<int>(optional_value<std::int64_t>(t, "dimensions", 2));
    tab.address    = static_cast<std::size_t>(optional_value<std::int64_t>(t, "address", 0));
    auto const dt  = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    tab.data_type = *dt;
    tab.scaling   = optional_value<std::string>(t, "scaling", {});

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
    tab.emissions_relevant     = optional_value<bool>(t, "emissions_relevant", false);
    tab.engine_safety_critical = optional_value<bool>(t, "engine_safety_critical", false);

    if (tab.dimensions < 1 || tab.dimensions > 3) {
        return failure(ErrorCode::ParseError,
                       "table '" + tab.id + "' dimensions must be 1, 2, or 3");
    }
    return tab;
}

Result<Pid> parse_pid(toml::table const &t) {
    Pid p;
    if (auto const v = t["id"].value<std::string>(); v.has_value()) {
        p.id = *v;
    } else {
        return failure(ErrorCode::ParseError, "[[pid]] missing id");
    }
    p.name        = optional_value<std::string>(t, "name", {});
    p.ssm_address = static_cast<std::size_t>(optional_value<std::int64_t>(t, "ssm_address", 0));
    p.length      = static_cast<std::size_t>(optional_value<std::int64_t>(t, "length", 0));
    auto const dt = parse_data_type_from(t, "data_type");
    if (!dt.has_value()) {
        return failure(dt.error());
    }
    p.data_type   = *dt;
    p.scaling     = optional_value<std::string>(t, "scaling", {});
    p.unit        = optional_value<std::string>(t, "unit", {});
    p.default_log = optional_value<bool>(t, "default_log", false);
    return p;
}

} // namespace

// ---- DataType helpers ----------------------------------------------------

Result<DataType> parse_data_type(std::string_view s) {
    if (s == "uint8")      return DataType::Uint8;
    if (s == "int8")       return DataType::Int8;
    if (s == "uint16_be")  return DataType::Uint16Be;
    if (s == "uint16_le")  return DataType::Uint16Le;
    if (s == "int16_be")   return DataType::Int16Be;
    if (s == "int16_le")   return DataType::Int16Le;
    if (s == "uint32_be")  return DataType::Uint32Be;
    if (s == "uint32_le")  return DataType::Uint32Le;
    if (s == "int32_be")   return DataType::Int32Be;
    if (s == "int32_le")   return DataType::Int32Le;
    if (s == "float32_be") return DataType::Float32Be;
    if (s == "float32_le") return DataType::Float32Le;
    return failure(ErrorCode::ParseError, "unknown data_type: " + std::string{s});
}

std::string_view to_string(DataType dt) noexcept {
    switch (dt) {
        case DataType::Uint8:      return "uint8";
        case DataType::Int8:       return "int8";
        case DataType::Uint16Be:   return "uint16_be";
        case DataType::Uint16Le:   return "uint16_le";
        case DataType::Int16Be:    return "int16_be";
        case DataType::Int16Le:    return "int16_le";
        case DataType::Uint32Be:   return "uint32_be";
        case DataType::Uint32Le:   return "uint32_le";
        case DataType::Int32Be:    return "int32_be";
        case DataType::Int32Le:    return "int32_le";
        case DataType::Float32Be:  return "float32_be";
        case DataType::Float32Le:  return "float32_le";
    }
    return "?";
}

std::size_t byte_size(DataType dt) noexcept {
    switch (dt) {
        case DataType::Uint8:
        case DataType::Int8:       return 1;
        case DataType::Uint16Be:
        case DataType::Uint16Le:
        case DataType::Int16Be:
        case DataType::Int16Le:    return 2;
        case DataType::Uint32Be:
        case DataType::Uint32Le:
        case DataType::Int32Be:
        case DataType::Int32Le:
        case DataType::Float32Be:
        case DataType::Float32Le:  return 4;
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
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(*r);
        }
        case DataType::Int8: {
            auto const r = rom.read_u8(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(static_cast<std::int8_t>(*r));
        }
        case DataType::Uint16Be: {
            auto const r = rom.read_u16_be(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(*r);
        }
        case DataType::Uint16Le: {
            auto const r = rom.read_u16_le(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(*r);
        }
        case DataType::Int16Be: {
            auto const r = rom.read_u16_be(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(static_cast<std::int16_t>(*r));
        }
        case DataType::Int16Le: {
            auto const r = rom.read_u16_le(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(static_cast<std::int16_t>(*r));
        }
        case DataType::Uint32Be: {
            auto const r = rom.read_u32_be(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(*r);
        }
        case DataType::Uint32Le: {
            auto const r = rom.read_u32_le(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(*r);
        }
        case DataType::Int32Be: {
            auto const r = rom.read_u32_be(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(static_cast<std::int32_t>(*r));
        }
        case DataType::Int32Le: {
            auto const r = rom.read_u32_le(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(static_cast<std::int32_t>(*r));
        }
        case DataType::Float32Be: {
            auto const r = rom.read_u32_be(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(bits_to_float(*r));
        }
        case DataType::Float32Le: {
            auto const r = rom.read_u32_le(offset);
            if (!r.has_value()) return failure(r.error());
            return static_cast<double>(bits_to_float(*r));
        }
    }
    return failure(ErrorCode::InvalidArgument, "unknown DataType");
}

double apply_scaling(double raw, Scaling const &s) noexcept {
    if (auto const *lin = std::get_if<LinearScaling>(&s.formula); lin != nullptr) {
        return raw * lin->factor + lin->offset;
    }
    auto const *pw = std::get_if<PiecewiseScaling>(&s.formula);
    if (pw == nullptr || pw->breakpoints.empty()) {
        return raw;
    }
    // Linear interpolation between the (breakpoints[i], values[i]) pairs.
    auto const &bp = pw->breakpoints;
    auto const &v  = pw->values;
    if (raw <= bp.front()) return v.front();
    if (raw >= bp.back())  return v.back();
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
    auto const *pw = std::get_if<PiecewiseScaling>(&s.formula);
    if (pw == nullptr || pw->values.size() < 2) {
        return failure(ErrorCode::InvalidArgument,
                       "scaling '" + s.id + "' is degenerate; not invertible");
    }
    // Piecewise inverse: locate the value segment containing `engineering`
    // and interpolate back to the breakpoint axis. Assumes a monotonic
    // values[] (otherwise the inverse is not a function).
    auto const &bp = pw->breakpoints;
    auto const &v  = pw->values;
    bool const  ascending = v.back() >= v.front();
    if (ascending) {
        if (engineering <= v.front()) return bp.front();
        if (engineering >= v.back())  return bp.back();
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (engineering <= v[i]) {
                double const span = v[i] - v[i - 1];
                if (span == 0.0) return bp[i - 1];
                double const t = (engineering - v[i - 1]) / span;
                return bp[i - 1] + t * (bp[i] - bp[i - 1]);
            }
        }
        return bp.back();
    }
    // Descending: mirror the loop direction.
    if (engineering >= v.front()) return bp.front();
    if (engineering <= v.back())  return bp.back();
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (engineering >= v[i]) {
            double const span = v[i] - v[i - 1];
            if (span == 0.0) return bp[i - 1];
            double const t = (engineering - v[i - 1]) / span;
            return bp[i - 1] + t * (bp[i] - bp[i - 1]);
        }
    }
    return bp.back();
}

namespace {

// Clamp `value` to the inclusive integer range [lo, hi] and round to nearest.
template <typename T>
T clamp_round(double value, double lo, double hi) noexcept {
    if (value < lo) value = lo;
    if (value > hi) value = hi;
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
            return rom.write_u32_be(offset,
                                    clamp_round<std::uint32_t>(value, 0.0, 4294967295.0));
        case DataType::Uint32Le:
            return rom.write_u32_le(offset,
                                    clamp_round<std::uint32_t>(value, 0.0, 4294967295.0));
        case DataType::Int32Be: {
            auto const v =
                clamp_round<std::int32_t>(value, -2147483648.0, 2147483647.0);
            return rom.write_u32_be(offset, static_cast<std::uint32_t>(v));
        }
        case DataType::Int32Le: {
            auto const v =
                clamp_round<std::int32_t>(value, -2147483648.0, 2147483647.0);
            return rom.write_u32_le(offset, static_cast<std::uint32_t>(v));
        }
        case DataType::Float32Be: {
            std::uint32_t bits = 0;
            float const   f    = static_cast<float>(value);
            std::memcpy(&bits, &f, sizeof(bits));
            return rom.write_u32_be(offset, bits);
        }
        case DataType::Float32Le: {
            std::uint32_t bits = 0;
            float const   f    = static_cast<float>(value);
            std::memcpy(&bits, &f, sizeof(bits));
            return rom.write_u32_le(offset, bits);
        }
    }
    return failure(ErrorCode::InvalidArgument, "unknown DataType");
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
            if (!pack_r.has_value()) return failure(pack_r.error());
            def.pack_ = std::move(*pack_r);
        } else if (accept_pack && require_pack && pack_node == nullptr) {
            return failure(ErrorCode::ParseError, "missing [pack] section");
        }

        auto const visit_array = [&](std::string_view key, auto parse_one,
                                     auto &dst) -> Status {
            auto const *arr = tbl[key].as_array();
            if (arr == nullptr) return ok();
            for (auto const &el : *arr) {
                auto const *t = el.as_table();
                if (t == nullptr) {
                    return failure(ErrorCode::ParseError,
                                   "element of " + std::string{key}
                                       + " is not a table");
                }
                auto r = parse_one(*t);
                if (!r.has_value()) return failure(r.error());
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
        if (auto r = visit_array("scaling", parse_scaling, def.scalings_);
            !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("table", parse_table, def.tables_); !r.has_value()) {
            return failure(r.error());
        }
        if (auto r = visit_array("pid", parse_pid, def.pids_); !r.has_value()) {
            return failure(r.error());
        }
        return ok();
    }
};

namespace {

Result<toml::table> parse_toml(std::string_view text) {
    try {
        return toml::parse(text);
    } catch (toml::parse_error const &e) {
        std::string msg{"TOML parse error: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
}

} // namespace

Result<Definition> Definition::from_toml_string(std::string_view toml) {
    auto tbl = parse_toml(toml);
    if (!tbl.has_value()) return failure(tbl.error());

    Definition def;
    if (auto r = DefinitionBuilder::merge(*tbl, def, /*accept_pack=*/true,
                                          /*require_pack=*/true);
        !r.has_value()) {
        return failure(r.error());
    }
    return def;
}

Result<Definition> Definition::from_file(std::filesystem::path const &path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        return from_directory(path);
    }
    std::string const contents = read_file(path, ec);
    if (ec) {
        return failure(ErrorCode::FileNotFound, path.string());
    }
    return from_toml_string(contents);
}

Result<Definition> Definition::from_directory(std::filesystem::path const &path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) {
        return failure(ErrorCode::InvalidArgument,
                       "not a directory: " + path.string());
    }

    auto const manifest = path / "pack.toml";
    if (!std::filesystem::exists(manifest, ec) || ec) {
        return failure(ErrorCode::FileNotFound,
                       "pack.toml missing in: " + path.string());
    }

    Definition def;

    auto const ingest = [&](std::filesystem::path const &p, bool accept_pack,
                            bool require_pack) -> Status {
        std::error_code   read_ec;
        std::string const contents = read_file(p, read_ec);
        if (read_ec) {
            return failure(ErrorCode::IoFailure, "cannot read " + p.string());
        }
        auto tbl = parse_toml(contents);
        if (!tbl.has_value()) return failure(tbl.error());
        return DefinitionBuilder::merge(*tbl, def, accept_pack, require_pack);
    };

    if (auto r = ingest(manifest, /*accept_pack=*/true, /*require_pack=*/true);
        !r.has_value()) {
        return failure(r.error());
    }

    // Walk the rest of the directory and merge every other *.toml.
    std::vector<std::filesystem::path> others;
    for (auto const &entry :
         std::filesystem::recursive_directory_iterator{path, ec}) {
        if (ec) {
            return failure(ErrorCode::IoFailure,
                           "walk failed in: " + path.string());
        }
        if (!entry.is_regular_file(ec) || ec) continue;
        auto const ep = entry.path();
        if (ep.extension() != ".toml") continue;
        if (std::filesystem::equivalent(ep, manifest, ec)) continue;
        others.push_back(ep);
    }
    // Sort for deterministic ordering across platforms.
    std::sort(others.begin(), others.end());
    for (auto const &p : others) {
        if (auto r = ingest(p, /*accept_pack=*/false, /*require_pack=*/false);
            !r.has_value()) {
            return failure(r.error());
        }
    }
    return def;
}

Status Definition::validate() const {
    std::string violations;
    auto const  note = [&](std::string s) {
        if (!violations.empty()) {
            violations.push_back('\n');
        }
        violations.append(std::move(s));
    };

    auto const rom_size = pack_.rom_size_bytes;
    auto const fits     = [rom_size](std::size_t addr, std::size_t bytes) {
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
                note(std::string{"table '"} + t.id + "' axis_" + which + " references unknown axis '"
                     + *ax + "'");
            }
        };
        check_axis('x', t.axis_x);
        check_axis('y', t.axis_y);
        check_axis('z', t.axis_z);

        if (t.dimensions >= 1 && !t.axis_x.has_value()) {
            note("table '" + t.id + "' requires axis_x for dimensions=" + std::to_string(t.dimensions));
        }
        if (t.dimensions >= 2 && !t.axis_y.has_value()) {
            note("table '" + t.id + "' requires axis_y for dimensions=" + std::to_string(t.dimensions));
        }
        if (t.dimensions >= 3 && !t.axis_z.has_value()) {
            note("table '" + t.id + "' requires axis_z for dimensions=" + std::to_string(t.dimensions));
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
    }
    for (auto const &id : ids_) {
        if (rom_size != 0 && !fits(id.cid_address, id.cid_length)) {
            note("identification '" + id.name + "' cid_address extends past rom_size_bytes");
        }
    }

    if (!violations.empty()) {
        return failure(ErrorCode::ParseError, std::move(violations));
    }
    return ok();
}

Axis const *Definition::find_axis(std::string_view id) const noexcept {
    auto it = std::find_if(axes_.begin(), axes_.end(),
                           [&](Axis const &a) { return a.id == id; });
    return it == axes_.end() ? nullptr : &*it;
}

Scaling const *Definition::find_scaling(std::string_view id) const noexcept {
    auto it = std::find_if(scalings_.begin(), scalings_.end(),
                           [&](Scaling const &s) { return s.id == id; });
    return it == scalings_.end() ? nullptr : &*it;
}

Table const *Definition::find_table(std::string_view id) const noexcept {
    auto it = std::find_if(tables_.begin(), tables_.end(),
                           [&](Table const &t) { return t.id == id; });
    return it == tables_.end() ? nullptr : &*it;
}

Pid const *Definition::find_pid(std::string_view id) const noexcept {
    auto it = std::find_if(pids_.begin(), pids_.end(),
                           [&](Pid const &p) { return p.id == id; });
    return it == pids_.end() ? nullptr : &*it;
}

Result<std::vector<double>> Definition::read_axis_values(Rom const &rom,
                                                          Axis const &axis) const {
    std::vector<double> out;
    out.reserve(axis.length);

    auto const step    = byte_size(axis.data_type);
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

Result<Definition::TableData> Definition::read_table_values(Rom const &  rom,
                                                            Table const &table) const {
    TableData td;

    // Resolve axes.
    Axis const *ax = nullptr;
    Axis const *ay = nullptr;
    if (table.axis_x.has_value() && !table.axis_x->empty()) {
        ax = find_axis(*table.axis_x);
        if (ax == nullptr) {
            return failure(ErrorCode::ParseError,
                           "table '" + table.id + "' references unknown axis_x '"
                               + *table.axis_x + "'");
        }
        auto xs = read_axis_values(rom, *ax);
        if (!xs.has_value()) return failure(xs.error());
        td.axis_x = std::move(*xs);
    }
    if (table.dimensions >= 2 && table.axis_y.has_value() && !table.axis_y->empty()) {
        ay = find_axis(*table.axis_y);
        if (ay == nullptr) {
            return failure(ErrorCode::ParseError,
                           "table '" + table.id + "' references unknown axis_y '"
                               + *table.axis_y + "'");
        }
        auto ys = read_axis_values(rom, *ay);
        if (!ys.has_value()) return failure(ys.error());
        td.axis_y = std::move(*ys);
    }

    auto const cols  = td.axis_x.empty() ? std::size_t{1} : td.axis_x.size();
    auto const rows  = td.axis_y.empty() ? std::size_t{1} : td.axis_y.size();
    auto const step  = byte_size(table.data_type);
    auto const scal  = find_scaling(table.scaling);

    td.values.assign(rows, std::vector<double>(cols, 0.0));
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            auto const off = table.address + (r * cols + c) * step;
            auto const raw = read_typed(rom, off, table.data_type);
            if (!raw.has_value()) return failure(raw.error());
            td.values[r][c] = (scal != nullptr) ? apply_scaling(*raw, *scal) : *raw;
        }
    }
    return td;
}

Status Definition::write_table_values(Rom &rom, Table const &table,
                                       TableData const &td) const {
    Axis const *ax = nullptr;
    Axis const *ay = nullptr;
    if (table.axis_x.has_value() && !table.axis_x->empty()) {
        ax = find_axis(*table.axis_x);
        if (ax == nullptr) {
            return failure(ErrorCode::ParseError,
                           "table '" + table.id + "' references unknown axis_x '"
                               + *table.axis_x + "'");
        }
    }
    if (table.dimensions >= 2 && table.axis_y.has_value() && !table.axis_y->empty()) {
        ay = find_axis(*table.axis_y);
        if (ay == nullptr) {
            return failure(ErrorCode::ParseError,
                           "table '" + table.id + "' references unknown axis_y '"
                               + *table.axis_y + "'");
        }
    }

    auto const cols = ax == nullptr ? std::size_t{1} : ax->length;
    auto const rows = ay == nullptr ? std::size_t{1} : ay->length;
    if (td.values.size() != rows) {
        return failure(ErrorCode::InvalidArgument,
                       "TableData has " + std::to_string(td.values.size())
                           + " rows but table expects " + std::to_string(rows));
    }
    for (std::size_t r = 0; r < rows; ++r) {
        if (td.values[r].size() != cols) {
            return failure(ErrorCode::InvalidArgument,
                           "TableData row " + std::to_string(r) + " has "
                               + std::to_string(td.values[r].size())
                               + " cols but table expects " + std::to_string(cols));
        }
    }

    auto const  step = byte_size(table.data_type);
    auto const *scal = find_scaling(table.scaling);

    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            double eng = td.values[r][c];
            double raw = eng;
            if (scal != nullptr) {
                auto const inv = invert_scaling(eng, *scal);
                if (!inv.has_value()) return failure(inv.error());
                raw = *inv;
            }
            auto const off = table.address + (r * cols + c) * step;
            if (auto s = write_typed(rom, off, table.data_type, raw); !s.has_value()) {
                return s;
            }
        }
    }
    return ok();
}

Result<Definition::TableDiff> Definition::diff_table(Rom const &  a,
                                                     Rom const &  b,
                                                     Table const &table) const {
    auto ta = read_table_values(a, table);
    if (!ta.has_value()) return failure(ta.error());
    auto tb = read_table_values(b, table);
    if (!tb.has_value()) return failure(tb.error());

    if (ta->values.size() != tb->values.size()) {
        return failure(ErrorCode::Unknown, "table '" + table.id + "': row count mismatch");
    }

    TableDiff diff;
    double    abs_sum = 0.0;
    for (std::size_t r = 0; r < ta->values.size(); ++r) {
        if (ta->values[r].size() != tb->values[r].size()) {
            return failure(ErrorCode::Unknown,
                           "table '" + table.id + "': column count mismatch on row "
                               + std::to_string(r));
        }
        for (std::size_t c = 0; c < ta->values[r].size(); ++c) {
            ++diff.total_cells;
            double const d = tb->values[r][c] - ta->values[r][c];
            if (d != 0.0) {
                ++diff.cells_changed;
                double const ad = d < 0 ? -d : d;
                abs_sum += ad;
                if (ad > diff.max_abs_delta) diff.max_abs_delta = ad;
            }
        }
    }
    if (diff.cells_changed > 0) {
        diff.mean_abs_delta = abs_sum / static_cast<double>(diff.cells_changed);
    }
    return diff;
}

std::optional<std::string> Definition::matches(Rom const &rom) const {
    for (auto const &id : ids_) {
        auto const slice = rom.slice(id.cid_address, id.cid_length);
        if (!slice.has_value()) {
            continue;
        }
        std::string_view const got{reinterpret_cast<char const *>(slice->data()),
                                   slice->size()};
        if (got == id.cid_match) {
            return id.name;
        }
    }
    return std::nullopt;
}

} // namespace st
