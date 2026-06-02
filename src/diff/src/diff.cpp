// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/diff.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace st::diff {

namespace {

// Compare a 2D `values` grid; emit changes + update aggregates.
void diff_2d(std::vector<std::vector<double>> const &a,
             std::vector<std::vector<double>> const &b, double epsilon,
             bool include_cell_list, TableDelta &out) {
    if (a.empty() || a.size() != b.size()) {
        return;
    }
    out.rows = a.size();
    out.cols = a.front().size();
    double sum_abs = 0.0;
    for (std::size_t r = 0; r < a.size(); ++r) {
        if (a[r].size() != b[r].size())
            return;
        for (std::size_t c = 0; c < a[r].size(); ++c) {
            ++out.total_cells;
            double const delta = b[r][c] - a[r][c];
            double const abs_delta = std::fabs(delta);
            if (abs_delta <= epsilon)
                continue;
            ++out.cells_changed;
            sum_abs += abs_delta;
            if (abs_delta > out.max_abs_delta)
                out.max_abs_delta = abs_delta;
            if (include_cell_list) {
                out.changes.push_back({r, c, a[r][c], b[r][c]});
            }
        }
    }
    if (out.cells_changed > 0) {
        out.mean_abs_delta = sum_abs / static_cast<double>(out.cells_changed);
    }
}

// Compare a 3D `slices` grid (third axis × rows × cols). Encodes the
// 3D coordinate into row = slice * rows_per_slice + r, col = c so the
// flat CellChange row/col still makes sense. Future renderers that
// care about the 3D structure can re-split by `out.rows / slice_count`.
void diff_3d(std::vector<std::vector<std::vector<double>>> const &a,
             std::vector<std::vector<std::vector<double>>> const &b, double epsilon,
             bool include_cell_list, TableDelta &out) {
    if (a.empty() || a.size() != b.size() || a.front().empty()) {
        return;
    }
    auto const slice_count = a.size();
    auto const rows_per_slice = a.front().size();
    out.rows = slice_count * rows_per_slice;
    out.cols = a.front().front().size();
    double sum_abs = 0.0;
    for (std::size_t s = 0; s < a.size(); ++s) {
        if (a[s].size() != b[s].size())
            return;
        for (std::size_t r = 0; r < a[s].size(); ++r) {
            if (a[s][r].size() != b[s][r].size())
                return;
            for (std::size_t c = 0; c < a[s][r].size(); ++c) {
                ++out.total_cells;
                double const delta = b[s][r][c] - a[s][r][c];
                double const abs_delta = std::fabs(delta);
                if (abs_delta <= epsilon)
                    continue;
                ++out.cells_changed;
                sum_abs += abs_delta;
                if (abs_delta > out.max_abs_delta)
                    out.max_abs_delta = abs_delta;
                if (include_cell_list) {
                    out.changes.push_back(
                        {s * rows_per_slice + r, c, a[s][r][c], b[s][r][c]});
                }
            }
        }
    }
    if (out.cells_changed > 0) {
        out.mean_abs_delta = sum_abs / static_cast<double>(out.cells_changed);
    }
}

[[nodiscard]] std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

void json_escape(std::string &out, std::string_view s) {
    out.push_back('"');
    for (char ch : s) {
        auto const u = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (u < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04X", u);
                out.append(buf);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
}

} // namespace

Result<DiffSet> compare(Rom const &a, Rom const &b, Definition const &def,
                        Options const &opts) {
    if (def.tables().empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "diff::compare: definition has no tables to compare");
    }
    if (a.size() == 0 && b.size() == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "diff::compare: both ROMs are empty");
    }

    DiffSet result;
    result.pack_id = def.pack().id;
    result.rom_a_crc32 = a.crc32();
    result.rom_b_crc32 = b.crc32();
    result.rom_a_size = a.size();
    result.rom_b_size = b.size();

    for (auto const &table : def.tables()) {
        auto const data_a = def.read_table_values(a, table);
        if (!data_a.has_value()) {
            result.skipped.push_back(
                {table.id, std::string{"A: "} + data_a.error().to_string()});
            continue;
        }
        auto const data_b = def.read_table_values(b, table);
        if (!data_b.has_value()) {
            result.skipped.push_back(
                {table.id, std::string{"B: "} + data_b.error().to_string()});
            continue;
        }

        TableDelta delta;
        delta.table_id = table.id;
        delta.table_name = table.name;
        delta.engine_safety_critical = table.engine_safety_critical;
        delta.emissions_relevant = table.emissions_relevant;

        if (table.dimensions == 3) {
            diff_3d(data_a->slices, data_b->slices, opts.cell_epsilon,
                    opts.include_cell_list, delta);
        } else {
            // 1D + 2D both flow through the values vector — a 1D table
            // is a single-row 2D in TableData.
            diff_2d(data_a->values, data_b->values, opts.cell_epsilon,
                    opts.include_cell_list, delta);
        }

        ++result.tables_compared;
        result.total_cells_compared += delta.total_cells;
        result.total_cells_changed += delta.cells_changed;
        if (delta.changed()) {
            ++result.tables_changed;
        }
        if (delta.changed() || opts.include_identical) {
            result.tables.push_back(std::move(delta));
        }
    }

    return result;
}

std::string render_text(DiffSet const &d) {
    std::ostringstream s;
    s << "DiffSet — pack " << d.pack_id << "\n";
    s << "  ROM A: " << hex32(d.rom_a_crc32) << "  (" << d.rom_a_size << " bytes)\n";
    s << "  ROM B: " << hex32(d.rom_b_crc32) << "  (" << d.rom_b_size << " bytes)\n";
    s << "  Tables compared: " << d.tables_compared << "  changed: " << d.tables_changed
      << "  skipped: " << d.skipped.size() << "\n";
    s << "  Cells compared:  " << d.total_cells_compared
      << "  changed: " << d.total_cells_changed << "\n";

    if (d.tables_changed == 0 && d.skipped.empty()) {
        s << "\nIdentical — every table compared equal.\n";
        return s.str();
    }

    if (!d.tables.empty()) {
        s << "\nChanged tables:\n";
        // Sort by cells_changed desc for visual scan; ties by name.
        auto sorted = d.tables;
        std::sort(sorted.begin(), sorted.end(),
                  [](TableDelta const &x, TableDelta const &y) {
                      if (x.cells_changed != y.cells_changed)
                          return x.cells_changed > y.cells_changed;
                      return x.table_id < y.table_id;
                  });
        for (auto const &t : sorted) {
            if (!t.changed())
                continue;
            char flags[3] = {' ', ' ', '\0'};
            if (t.engine_safety_critical)
                flags[0] = 'S';
            if (t.emissions_relevant)
                flags[1] = 'E';
            s << "  " << flags << "  " << t.table_id << "  (" << t.table_name << ")\n";
            s << "       " << t.cells_changed << "/" << t.total_cells
              << " cells changed; max |Δ|=" << t.max_abs_delta
              << "  mean |Δ|=" << t.mean_abs_delta << "\n";
        }
    }
    if (!d.skipped.empty()) {
        s << "\nSkipped tables:\n";
        for (auto const &k : d.skipped) {
            s << "  " << k.table_id << "  — " << k.reason << "\n";
        }
    }
    return s.str();
}

std::string render_csv(DiffSet const &d, bool include_unchanged) {
    std::string out;
    out.append("table_id,table_name,row,col,value_a,value_b,delta\n");
    char buf[64];
    for (auto const &t : d.tables) {
        if (!t.changed() && !include_unchanged)
            continue;
        if (t.changes.empty() && include_unchanged) {
            // Emit one row to mark the table as compared-but-unchanged.
            out.append(t.table_id);
            out.append(",");
            out.append(t.table_name);
            out.append(",,,,,\n");
            continue;
        }
        for (auto const &c : t.changes) {
            out.append(t.table_id);
            out.append(",");
            // Naive CSV-quote the name when it contains a comma.
            if (t.table_name.find(',') != std::string::npos) {
                out.push_back('"');
                out.append(t.table_name);
                out.push_back('"');
            } else {
                out.append(t.table_name);
            }
            out.append(",");
            out.append(std::to_string(c.row));
            out.append(",");
            out.append(std::to_string(c.col));
            out.append(",");
            std::snprintf(buf, sizeof buf, "%g", c.value_a);
            out.append(buf);
            out.append(",");
            std::snprintf(buf, sizeof buf, "%g", c.value_b);
            out.append(buf);
            out.append(",");
            std::snprintf(buf, sizeof buf, "%g", c.delta());
            out.append(buf);
            out.append("\n");
        }
    }
    return out;
}

std::string render_json(DiffSet const &d) {
    std::string out;
    out.reserve(4096);
    out.append("{\"schema\":\"subuwutuner.diff.v");
    out.append(std::to_string(d.schema_version));
    out.append("\",\"pack_id\":");
    json_escape(out, d.pack_id);
    out.append(",\"rom_a\":{\"crc32\":");
    out.append(std::to_string(d.rom_a_crc32));
    out.append(",\"size\":");
    out.append(std::to_string(d.rom_a_size));
    out.append("},\"rom_b\":{\"crc32\":");
    out.append(std::to_string(d.rom_b_crc32));
    out.append(",\"size\":");
    out.append(std::to_string(d.rom_b_size));
    out.append("},\"summary\":{");
    out.append("\"tables_compared\":");
    out.append(std::to_string(d.tables_compared));
    out.append(",\"tables_changed\":");
    out.append(std::to_string(d.tables_changed));
    out.append(",\"total_cells_compared\":");
    out.append(std::to_string(d.total_cells_compared));
    out.append(",\"total_cells_changed\":");
    out.append(std::to_string(d.total_cells_changed));
    out.append(",\"identical\":");
    out.append(d.identical() ? "true" : "false");
    out.append("},\"tables\":[");
    char buf[64];
    for (std::size_t i = 0; i < d.tables.size(); ++i) {
        if (i != 0)
            out.append(",");
        auto const &t = d.tables[i];
        out.append("{\"table_id\":");
        json_escape(out, t.table_id);
        out.append(",\"table_name\":");
        json_escape(out, t.table_name);
        out.append(",\"rows\":");
        out.append(std::to_string(t.rows));
        out.append(",\"cols\":");
        out.append(std::to_string(t.cols));
        out.append(",\"total_cells\":");
        out.append(std::to_string(t.total_cells));
        out.append(",\"cells_changed\":");
        out.append(std::to_string(t.cells_changed));
        std::snprintf(buf, sizeof buf, "%g", t.max_abs_delta);
        out.append(",\"max_abs_delta\":");
        out.append(buf);
        std::snprintf(buf, sizeof buf, "%g", t.mean_abs_delta);
        out.append(",\"mean_abs_delta\":");
        out.append(buf);
        out.append(",\"engine_safety_critical\":");
        out.append(t.engine_safety_critical ? "true" : "false");
        out.append(",\"emissions_relevant\":");
        out.append(t.emissions_relevant ? "true" : "false");
        out.append(",\"changes\":[");
        for (std::size_t j = 0; j < t.changes.size(); ++j) {
            if (j != 0)
                out.append(",");
            auto const &c = t.changes[j];
            out.append("{\"row\":");
            out.append(std::to_string(c.row));
            out.append(",\"col\":");
            out.append(std::to_string(c.col));
            std::snprintf(buf, sizeof buf, "%g", c.value_a);
            out.append(",\"value_a\":");
            out.append(buf);
            std::snprintf(buf, sizeof buf, "%g", c.value_b);
            out.append(",\"value_b\":");
            out.append(buf);
            std::snprintf(buf, sizeof buf, "%g", c.delta());
            out.append(",\"delta\":");
            out.append(buf);
            out.append("}");
        }
        out.append("]}");
    }
    out.append("],\"skipped\":[");
    for (std::size_t i = 0; i < d.skipped.size(); ++i) {
        if (i != 0)
            out.append(",");
        auto const &k = d.skipped[i];
        out.append("{\"table_id\":");
        json_escape(out, k.table_id);
        out.append(",\"reason\":");
        json_escape(out, k.reason);
        out.append("}");
    }
    out.append("]}");
    return out;
}

// ---- CompareSession persistence ----------------------------------------

namespace {

// Helper for the few `as_*` accesses we need from tomlplusplus.
template <typename T>
[[nodiscard]] std::optional<T> get(toml::table const &tbl, std::string_view key) {
    auto const node = tbl.get(key);
    if (node == nullptr)
        return std::nullopt;
    auto const v = node->value<T>();
    if (!v.has_value())
        return std::nullopt;
    return *v;
}

} // namespace

Status save_compare_session(CompareSession const &session,
                            std::filesystem::path const &path) {
    toml::table root;

    toml::table session_tbl;
    session_tbl.insert("schema_version", static_cast<std::int64_t>(session.schema_version));
    if (!session.created_at.empty()) {
        session_tbl.insert("created_at", session.created_at);
    }
    session_tbl.insert("pack_id", session.pack_id);

    toml::table rom_a_tbl;
    rom_a_tbl.insert("path", session.rom_a_path);
    rom_a_tbl.insert("crc32", static_cast<std::int64_t>(session.rom_a_crc32));
    rom_a_tbl.insert("size", static_cast<std::int64_t>(session.rom_a_size));
    session_tbl.insert("rom_a", std::move(rom_a_tbl));

    toml::table rom_b_tbl;
    rom_b_tbl.insert("path", session.rom_b_path);
    rom_b_tbl.insert("crc32", static_cast<std::int64_t>(session.rom_b_crc32));
    rom_b_tbl.insert("size", static_cast<std::int64_t>(session.rom_b_size));
    session_tbl.insert("rom_b", std::move(rom_b_tbl));

    toml::table opts_tbl;
    opts_tbl.insert("cell_epsilon", session.options.cell_epsilon);
    opts_tbl.insert("include_identical", session.options.include_identical);
    opts_tbl.insert("include_cell_list", session.options.include_cell_list);
    session_tbl.insert("options", std::move(opts_tbl));

    if (!session.annotations.empty()) {
        toml::array ann_arr;
        for (auto const &a : session.annotations) {
            toml::table ann;
            ann.insert("table_id", a.table_id);
            ann.insert("row", static_cast<std::int64_t>(a.row));
            ann.insert("col", static_cast<std::int64_t>(a.col));
            ann.insert("text", a.text);
            ann_arr.push_back(std::move(ann));
        }
        session_tbl.insert("annotations", std::move(ann_arr));
    }

    root.insert("stcompare", std::move(session_tbl));

    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::string msg{"save_compare_session: cannot open output: "};
        msg.append(path.string());
        return failure(ErrorCode::IoFailure, std::move(msg));
    }
    out << root;
    return ok();
}

Result<CompareSession> load_compare_session(std::filesystem::path const &path) {
    toml::table parsed;
    try {
        parsed = toml::parse_file(path.string());
    } catch (toml::parse_error const &e) {
        std::string msg{"load_compare_session: TOML parse: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    auto const stcompare = parsed["stcompare"].as_table();
    if (stcompare == nullptr) {
        return failure(ErrorCode::ParseError,
                       "load_compare_session: missing [stcompare] table");
    }

    CompareSession session;
    session.schema_version = static_cast<int>(
        get<std::int64_t>(*stcompare, "schema_version").value_or(kCompareSessionSchemaVersion));
    if (session.schema_version > kCompareSessionSchemaVersion) {
        std::string msg{"load_compare_session: file schema_version "};
        msg.append(std::to_string(session.schema_version));
        msg.append(" newer than supported ");
        msg.append(std::to_string(kCompareSessionSchemaVersion));
        msg.append(" — upgrade SubuwuTuner to read this session");
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    session.pack_id = get<std::string>(*stcompare, "pack_id").value_or(std::string{});
    session.created_at = get<std::string>(*stcompare, "created_at").value_or(std::string{});

    auto const load_rom_tbl = [&](char const *key, std::string &path_out,
                                  std::uint32_t &crc_out,
                                  std::size_t &size_out) -> Status {
        auto const tbl = (*stcompare)[key].as_table();
        if (tbl == nullptr) {
            std::string msg{"load_compare_session: missing [stcompare."};
            msg.append(key);
            msg.append("] table");
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        path_out = get<std::string>(*tbl, "path").value_or(std::string{});
        crc_out = static_cast<std::uint32_t>(
            get<std::int64_t>(*tbl, "crc32").value_or(0));
        size_out = static_cast<std::size_t>(
            get<std::int64_t>(*tbl, "size").value_or(0));
        if (path_out.empty()) {
            std::string msg{"load_compare_session: [stcompare."};
            msg.append(key);
            msg.append("] missing path");
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        return ok();
    };
    if (auto s = load_rom_tbl("rom_a", session.rom_a_path, session.rom_a_crc32,
                              session.rom_a_size);
        !s.has_value()) {
        return failure(s.error());
    }
    if (auto s = load_rom_tbl("rom_b", session.rom_b_path, session.rom_b_crc32,
                              session.rom_b_size);
        !s.has_value()) {
        return failure(s.error());
    }

    if (auto const opts = (*stcompare)["options"].as_table(); opts != nullptr) {
        session.options.cell_epsilon =
            get<double>(*opts, "cell_epsilon").value_or(0.0);
        session.options.include_identical =
            get<bool>(*opts, "include_identical").value_or(false);
        session.options.include_cell_list =
            get<bool>(*opts, "include_cell_list").value_or(true);
    }

    if (auto const ann_arr = (*stcompare)["annotations"].as_array(); ann_arr != nullptr) {
        for (auto const &node : *ann_arr) {
            auto const ann_tbl = node.as_table();
            if (ann_tbl == nullptr)
                continue;
            CompareAnnotation a;
            a.table_id = get<std::string>(*ann_tbl, "table_id").value_or(std::string{});
            a.row = static_cast<std::size_t>(
                get<std::int64_t>(*ann_tbl, "row").value_or(0));
            a.col = static_cast<std::size_t>(
                get<std::int64_t>(*ann_tbl, "col").value_or(0));
            a.text = get<std::string>(*ann_tbl, "text").value_or(std::string{});
            session.annotations.push_back(std::move(a));
        }
    }

    return session;
}

Status verify_compare_inputs(CompareSession const &session,
                             std::filesystem::path const &base_dir) {
    auto const resolve = [&](std::string const &p) -> std::filesystem::path {
        std::filesystem::path pp{p};
        if (pp.is_absolute())
            return pp;
        return base_dir / pp;
    };

    auto const check = [&](char const *which, std::string const &path_str,
                           std::uint32_t expected_crc,
                           std::size_t expected_size) -> Status {
        auto const resolved = resolve(path_str);
        auto rom = Rom::from_file(resolved);
        if (!rom.has_value()) {
            std::string msg{"verify_compare_inputs: "};
            msg.append(which);
            msg.append(": cannot read ");
            msg.append(resolved.string());
            msg.append(" — ");
            msg.append(rom.error().to_string());
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        if (rom->size() != expected_size) {
            std::string msg{"verify_compare_inputs: "};
            msg.append(which);
            msg.append(": size mismatch (expected ");
            msg.append(std::to_string(expected_size));
            msg.append(", got ");
            msg.append(std::to_string(rom->size()));
            msg.append(")");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        auto const actual_crc = rom->crc32();
        if (actual_crc != expected_crc) {
            char buf[24];
            std::string msg{"verify_compare_inputs: "};
            msg.append(which);
            msg.append(": CRC32 mismatch (expected ");
            std::snprintf(buf, sizeof buf, "0x%08X", expected_crc);
            msg.append(buf);
            msg.append(", got ");
            std::snprintf(buf, sizeof buf, "0x%08X", actual_crc);
            msg.append(buf);
            msg.append(") — ROM has changed since the session was saved");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        return ok();
    };
    if (auto s = check("ROM A", session.rom_a_path, session.rom_a_crc32,
                       session.rom_a_size);
        !s.has_value()) {
        return failure(s.error());
    }
    if (auto s = check("ROM B", session.rom_b_path, session.rom_b_crc32,
                       session.rom_b_size);
        !s.has_value()) {
        return failure(s.error());
    }
    return ok();
}

} // namespace st::diff
