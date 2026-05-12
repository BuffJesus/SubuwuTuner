// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/project.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/defs.hpp"
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
    return save_metadata();
}

Status Project::save_metadata() const {
    auto const toml_text = render_project_toml(*this, source_crc32_, working_.crc32(),
                                               created_, source_rel_, working_rel_,
                                               def_rel_);
    return write_file(dir_ / "project.toml", toml_text);
}

} // namespace st
