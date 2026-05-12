// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/project.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"
#include "st/rom.hpp"

#include <toml++/toml.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace st {

namespace {

std::string iso8601_utc_now() {
    auto const now = std::chrono::system_clock::now();
    auto const tt  = std::chrono::system_clock::to_time_t(now);
    std::tm    tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

Status write_file(std::filesystem::path const &path, std::string_view contents) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure, "cannot open for write: " + path.string());
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "write failed: " + path.string());
    }
    return ok();
}

Status copy_bytes(std::filesystem::path const &src, std::filesystem::path const &dst) {
    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return failure(ErrorCode::IoFailure,
                       "copy failed: " + src.string() + " -> " + dst.string());
    }
    return ok();
}


// ---- History serialization ---------------------------------------------
// edits.toml schema:
//
//   schema_version = 1
//   cursor         = N           # how many edits are "applied"
//
//   [[edit]]
//   table_id    = "..."
//   description = "..."
//   [edit.before]
//     r_start = ..  r_end = ..  c_start = ..  c_end = ..
//     values  = [[...], [...]]   # 2D row-major
//   [edit.after]
//     r_start = ..  r_end = ..  c_start = ..  c_end = ..
//     values  = [[...], [...]]

void render_snapshot(std::ostringstream &ss, char const *name,
                     edit::Snapshot const &s) {
    ss << "  [edit." << name << "]\n";
    ss << "  r_start = " << s.rect.r_start << "\n";
    ss << "  r_end   = " << s.rect.r_end << "\n";
    ss << "  c_start = " << s.rect.c_start << "\n";
    ss << "  c_end   = " << s.rect.c_end << "\n";
    ss << "  values = [\n";
    for (auto const &row : s.values) {
        ss << "    [";
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << row[i];
        }
        ss << "],\n";
    }
    ss << "  ]\n";
}

std::string render_history_toml(edit::History const &h) {
    std::ostringstream ss;
    ss << "# Edit history for this SubuwuTuner project. Generated\n";
    ss << "# automatically; hand-edits are discouraged but possible.\n";
    ss << "schema_version = 1\n";
    ss << "cursor = " << h.cursor() << "\n";
    for (auto const &e : h.records()) {
        ss << "\n[[edit]]\n";
        ss << "  table_id    = \"" << e.table_id << "\"\n";
        ss << "  description = \"" << e.description << "\"\n";
        render_snapshot(ss, "before", e.before);
        render_snapshot(ss, "after", e.after);
    }
    return std::move(ss).str();
}

Result<edit::Snapshot> parse_snapshot(toml::table const &t) {
    edit::Snapshot s;
    s.rect.r_start = static_cast<std::size_t>(t["r_start"].value_or<std::int64_t>(0));
    s.rect.r_end   = static_cast<std::size_t>(t["r_end"].value_or<std::int64_t>(0));
    s.rect.c_start = static_cast<std::size_t>(t["c_start"].value_or<std::int64_t>(0));
    s.rect.c_end   = static_cast<std::size_t>(t["c_end"].value_or<std::int64_t>(0));

    auto const *rows = t["values"].as_array();
    if (rows == nullptr) {
        return failure(ErrorCode::ParseError, "edit snapshot missing values array");
    }
    for (auto const &row_node : *rows) {
        auto const *row_arr = row_node.as_array();
        if (row_arr == nullptr) {
            return failure(ErrorCode::ParseError, "edit snapshot row is not an array");
        }
        std::vector<double> row;
        row.reserve(row_arr->size());
        for (auto const &cell : *row_arr) {
            if (auto v = cell.value<double>(); v.has_value()) {
                row.push_back(*v);
            } else if (auto i = cell.value<std::int64_t>(); i.has_value()) {
                row.push_back(static_cast<double>(*i));
            } else {
                return failure(ErrorCode::ParseError,
                               "edit snapshot cell is neither float nor int");
            }
        }
        s.values.push_back(std::move(row));
    }
    return s;
}

Result<edit::History> parse_history_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (toml::parse_error const &e) {
        std::string msg{"edits.toml parse: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    int const schema = static_cast<int>(tbl["schema_version"].value_or<std::int64_t>(0));
    if (schema > 1) {
        return failure(ErrorCode::UnsupportedVersion,
                       "edits.toml schema_version " + std::to_string(schema) + " > 1");
    }
    auto const cursor =
        static_cast<std::size_t>(tbl["cursor"].value_or<std::int64_t>(0));

    std::vector<edit::Edit> edits;
    if (auto const *arr = tbl["edit"].as_array(); arr != nullptr) {
        for (auto const &el : *arr) {
            auto const *et = el.as_table();
            if (et == nullptr) {
                return failure(ErrorCode::ParseError, "[[edit]] element is not a table");
            }
            edit::Edit e;
            e.table_id    = (*et)["table_id"].value_or<std::string>("");
            e.description = (*et)["description"].value_or<std::string>("");

            auto const *before_t = (*et)["before"].as_table();
            auto const *after_t  = (*et)["after"].as_table();
            if (before_t == nullptr || after_t == nullptr) {
                return failure(ErrorCode::ParseError,
                               "[[edit]] missing [edit.before] or [edit.after]");
            }
            auto before_r = parse_snapshot(*before_t);
            if (!before_r.has_value()) return failure(before_r.error());
            auto after_r = parse_snapshot(*after_t);
            if (!after_r.has_value()) return failure(after_r.error());
            e.before = std::move(*before_r);
            e.after  = std::move(*after_r);
            edits.push_back(std::move(e));
        }
    }

    edit::History h;
    h.load(std::move(edits), cursor);
    return h;
}

std::string render_project_toml(Project const &p, std::uint32_t source_crc32,
                                std::uint32_t              working_crc32,
                                std::string const &        created,
                                std::filesystem::path const &source_rel,
                                std::filesystem::path const &working_rel,
                                std::filesystem::path const &def_rel) {
    std::ostringstream ss;
    auto const         emit_string = [&](std::string_view k, std::string_view v) {
        ss << k << " = \"";
        for (char c : v) {
            if (c == '"' || c == '\\') {
                ss << '\\';
            }
            ss << c;
        }
        ss << "\"\n";
    };

    ss << "[project]\n";
    ss << "schema_version = " << Project::kSchemaVersion << "\n";
    emit_string("display_name ", p.display_name());
    emit_string("created      ", created);
    emit_string("notes        ", p.notes());
    ss << "\n";
    ss << "[project.source_rom]\n";
    emit_string("path  ", source_rel.generic_string());
    ss << "crc32 = " << source_crc32 << "\n";
    ss << "\n";
    ss << "[project.working_rom]\n";
    emit_string("path  ", working_rel.generic_string());
    ss << "crc32 = " << working_crc32 << "\n";
    ss << "\n";
    ss << "[project.definition]\n";
    emit_string("path", def_rel.generic_string());
    return std::move(ss).str();
}

} // namespace

Result<Project> Project::create(std::filesystem::path const &project_dir,
                                std::filesystem::path const &source_rom_path,
                                std::filesystem::path const &definition_path,
                                std::string                  display_name) {
    std::error_code ec;
    if (std::filesystem::exists(project_dir, ec)) {
        if (!std::filesystem::is_directory(project_dir, ec) || ec) {
            return failure(ErrorCode::InvalidArgument,
                           "project path exists and is not a directory: "
                               + project_dir.string());
        }
        if (!std::filesystem::is_empty(project_dir, ec) || ec) {
            return failure(ErrorCode::InvalidArgument,
                           "project directory is not empty: " + project_dir.string());
        }
    } else {
        std::filesystem::create_directories(project_dir, ec);
        if (ec) {
            return failure(ErrorCode::IoFailure,
                           "cannot create project dir: " + project_dir.string());
        }
    }

    auto source_load = Rom::from_file(source_rom_path);
    if (!source_load.has_value()) return failure(source_load.error());

    auto def_load = Definition::from_file(definition_path);
    if (!def_load.has_value()) return failure(def_load.error());

    Project p;
    p.dir_           = project_dir;
    p.display_name_  = std::move(display_name);
    p.notes_         = "";
    p.created_       = iso8601_utc_now();
    p.source_        = std::move(*source_load);
    p.source_crc32_  = p.source_.crc32();
    // Working starts as a byte-for-byte copy of source.
    p.working_       = Rom::from_bytes(std::vector<std::uint8_t>(p.source_.data().begin(),
                                                                 p.source_.data().end()));
    p.def_           = std::move(*def_load);

    // Compute a relative path to the definition. If the definition is outside
    // the project dir we store the relative (possibly going up with ..) so
    // the project remains portable as long as the user moves both.
    std::error_code def_ec;
    auto const      def_abs = std::filesystem::weakly_canonical(definition_path, def_ec);
    auto const      proj_abs =
        std::filesystem::weakly_canonical(project_dir, def_ec);
    auto rel = std::filesystem::relative(def_abs, proj_abs, def_ec);
    if (def_ec || rel.empty()) {
        // Fall back to absolute if we can't compute a relative path.
        p.def_rel_ = def_abs;
    } else {
        p.def_rel_ = std::move(rel);
    }

    if (auto s = copy_bytes(source_rom_path, project_dir / p.source_rel_); !s.has_value()) {
        return failure(s.error());
    }
    if (auto s = copy_bytes(source_rom_path, project_dir / p.working_rel_); !s.has_value()) {
        return failure(s.error());
    }

    if (auto s = p.save_metadata(); !s.has_value()) return failure(s.error());
    return p;
}

Result<Project> Project::open(std::filesystem::path const &project_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(project_dir, ec) || ec) {
        return failure(ErrorCode::InvalidArgument,
                       "not a project directory: " + project_dir.string());
    }
    auto const manifest = project_dir / "project.toml";
    if (!std::filesystem::exists(manifest, ec) || ec) {
        return failure(ErrorCode::FileNotFound,
                       "project.toml missing in: " + project_dir.string());
    }

    std::ifstream in{manifest};
    std::ostringstream contents;
    contents << in.rdbuf();
    if (!in.good() && !in.eof()) {
        return failure(ErrorCode::IoFailure, "read failed: " + manifest.string());
    }

    toml::table tbl;
    try {
        tbl = toml::parse(contents.str());
    } catch (toml::parse_error const &e) {
        std::string msg{"project.toml parse: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    auto const *project = tbl["project"].as_table();
    if (project == nullptr) {
        return failure(ErrorCode::ParseError, "missing [project] section");
    }
    int const schema = static_cast<int>((*project)["schema_version"].value_or<std::int64_t>(0));
    if (schema > kSchemaVersion) {
        return failure(ErrorCode::UnsupportedVersion,
                       "project schema_version " + std::to_string(schema)
                           + " is newer than this build supports ("
                           + std::to_string(kSchemaVersion) + ")");
    }

    Project p;
    p.dir_          = project_dir;
    p.display_name_ = (*project)["display_name"].value_or<std::string>("");
    p.notes_        = (*project)["notes"].value_or<std::string>("");
    p.created_      = (*project)["created"].value_or<std::string>("");

    auto const get_path = [&](toml::table const &t, char const *key) -> std::string {
        return t[key].value_or<std::string>("");
    };

    auto const *src_tbl = (*project)["source_rom"].as_table();
    auto const *wrk_tbl = (*project)["working_rom"].as_table();
    auto const *def_tbl = (*project)["definition"].as_table();
    if (src_tbl == nullptr || wrk_tbl == nullptr || def_tbl == nullptr) {
        return failure(ErrorCode::ParseError,
                       "[project.source_rom], [project.working_rom], and "
                       "[project.definition] are all required");
    }

    p.source_rel_  = get_path(*src_tbl, "path");
    p.working_rel_ = get_path(*wrk_tbl, "path");
    p.def_rel_     = get_path(*def_tbl, "path");
    p.source_crc32_ =
        static_cast<std::uint32_t>((*src_tbl)["crc32"].value_or<std::int64_t>(0));

    auto src = Rom::from_file(project_dir / p.source_rel_);
    if (!src.has_value()) return failure(src.error());
    p.source_ = std::move(*src);

    auto wrk = Rom::from_file(project_dir / p.working_rel_);
    if (!wrk.has_value()) return failure(wrk.error());
    p.working_ = std::move(*wrk);

    auto const def_resolved = std::filesystem::weakly_canonical(project_dir / p.def_rel_, ec);
    auto def = Definition::from_file(ec ? (project_dir / p.def_rel_) : def_resolved);
    if (!def.has_value()) return failure(def.error());
    p.def_ = std::move(*def);

    // edits.toml is optional. If present, restore the edit history so
    // cross-session undo works.
    auto const edits_path = project_dir / "edits.toml";
    if (std::filesystem::exists(edits_path, ec) && !ec) {
        std::ifstream      ein{edits_path};
        std::ostringstream econtents;
        econtents << ein.rdbuf();
        auto hist = parse_history_toml(econtents.str());
        if (!hist.has_value()) return failure(hist.error());
        p.history_ = std::move(*hist);
    }

    return p;
}

Status Project::save_working_rom() {
    auto const path = dir_ / working_rel_;
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return failure(ErrorCode::IoFailure, "cannot open: " + path.string());
    }
    out.write(reinterpret_cast<char const *>(working_.data().data()),
              static_cast<std::streamsize>(working_.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "write failed: " + path.string());
    }

    // Persist edit history alongside, if there's anything to save.
    if (history_.size() > 0) {
        auto const edits_text = render_history_toml(history_);
        if (auto s = write_file(dir_ / "edits.toml", edits_text); !s.has_value()) {
            return s;
        }
    }
    return save_metadata();
}

Status Project::save_metadata() const {
    auto const toml_text = render_project_toml(*this, source_crc32_, working_.crc32(),
                                               created_, source_rel_, working_rel_,
                                               def_rel_);
    return write_file(dir_ / "project.toml", toml_text);
}

} // namespace st
