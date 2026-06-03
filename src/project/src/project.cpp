// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/project.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"
#include "st/rom.hpp"

#include <toml++/toml.hpp>

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
    auto const tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
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
//   schema_version = 2          # v1 = TableEdit-only; v2 adds ByteEdit
//   cursor         = N          # how many edits are "applied"
//
// TableEdit (rect-scoped):
//
//   [[edit]]
//   description = "..."
//   table_id    = "..."
//   [edit.before]
//     r_start = ..  r_end = ..  c_start = ..  c_end = ..
//     values  = [[...], [...]]   # 2D row-major
//   [edit.after]
//     r_start = ..  r_end = ..  c_start = ..  c_end = ..
//     values  = [[...], [...]]
//
// ByteEdit (byte-scoped — used for DTC enable-bit toggles and other
// edits that don't map onto a Definition table):
//
//   [[edit]]
//   description = "..."
//   [[edit.byte_changes]]
//     address = 12345
//     before  = 255       # 0..255
//     after   = 254
//
// Schema discrimination on read: presence of `table_id` AND
// `[edit.before]`/`[edit.after]` ⇒ TableEdit; presence of
// `byte_changes` ⇒ ByteEdit. v1 files are pure TableEdit and load
// unchanged.

void render_snapshot(std::ostringstream &ss, char const *name, edit::Snapshot const &s) {
    ss << "  [edit." << name << "]\n";
    ss << "  r_start = " << s.rect.r_start << "\n";
    ss << "  r_end   = " << s.rect.r_end << "\n";
    ss << "  c_start = " << s.rect.c_start << "\n";
    ss << "  c_end   = " << s.rect.c_end << "\n";
    ss << "  values = [\n";
    for (auto const &row : s.values) {
        ss << "    [";
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0)
                ss << ", ";
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
    ss << "schema_version = 2\n";
    ss << "cursor = " << h.cursor() << "\n";
    for (auto const &e : h.records()) {
        ss << "\n[[edit]]\n";
        ss << "  description = \"" << e.description << "\"\n";
        if (auto const *t = e.as_table(); t != nullptr) {
            ss << "  table_id    = \"" << t->table_id << "\"\n";
            render_snapshot(ss, "before", t->before);
            render_snapshot(ss, "after", t->after);
        } else if (auto const *b = e.as_byte(); b != nullptr) {
            for (auto const &c : b->changes) {
                ss << "  [[edit.byte_changes]]\n";
                ss << "    address = " << c.address << "\n";
                ss << "    before  = " << static_cast<unsigned>(c.before) << "\n";
                ss << "    after   = " << static_cast<unsigned>(c.after) << "\n";
            }
        }
    }
    return std::move(ss).str();
}

Result<edit::Snapshot> parse_snapshot(toml::table const &t) {
    edit::Snapshot s;
    s.rect.r_start = static_cast<std::size_t>(t["r_start"].value_or<std::int64_t>(0));
    s.rect.r_end = static_cast<std::size_t>(t["r_end"].value_or<std::int64_t>(0));
    s.rect.c_start = static_cast<std::size_t>(t["c_start"].value_or<std::int64_t>(0));
    s.rect.c_end = static_cast<std::size_t>(t["c_end"].value_or<std::int64_t>(0));

    auto const *rows = t["values"].as_array();
    if (rows == nullptr) {
        return failure(ErrorCode::ParseError, "edit snapshot missing values array");
    }
    std::size_t row_idx = 0;
    for (auto const &row_node : *rows) {
        auto const *row_arr = row_node.as_array();
        if (row_arr == nullptr) {
            return failure(ErrorCode::ParseError, "edit snapshot row " +
                                                      std::to_string(row_idx) + " is not an array");
        }
        std::vector<double> row;
        row.reserve(row_arr->size());
        std::size_t col_idx = 0;
        for (auto const &cell : *row_arr) {
            if (auto v = cell.value<double>(); v.has_value()) {
                row.push_back(*v);
            } else if (auto i = cell.value<std::int64_t>(); i.has_value()) {
                row.push_back(static_cast<double>(*i));
            } else {
                return failure(ErrorCode::ParseError,
                               "edit snapshot cell at row " + std::to_string(row_idx) + ", col " +
                                   std::to_string(col_idx) + " is neither float nor int");
            }
            ++col_idx;
        }
        s.values.push_back(std::move(row));
        ++row_idx;
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
    if (schema > 2) {
        return failure(ErrorCode::UnsupportedVersion,
                       "edits.toml schema_version " + std::to_string(schema) + " > 2");
    }
    auto const cursor = static_cast<std::size_t>(tbl["cursor"].value_or<std::int64_t>(0));

    std::vector<edit::Edit> edits;
    if (auto const *arr = tbl["edit"].as_array(); arr != nullptr) {
        std::size_t edit_idx = 0;
        for (auto const &el : *arr) {
            auto const *et = el.as_table();
            if (et == nullptr) {
                return failure(ErrorCode::ParseError,
                               "[[edit]] element at index " + std::to_string(edit_idx) +
                                   " is not a table");
            }
            edit::Edit e;
            e.description = (*et)["description"].value_or<std::string>("");

            // Discriminate kind by which fields are present. ByteEdit's
            // `byte_changes` array is the v2 addition; absence of it plus
            // presence of [edit.before]/[edit.after] is the v1 TableEdit
            // shape that we still accept verbatim.
            auto const *byte_arr = (*et)["byte_changes"].as_array();
            if (byte_arr != nullptr) {
                edit::ByteEdit b;
                b.changes.reserve(byte_arr->size());
                std::size_t change_idx = 0;
                for (auto const &cn : *byte_arr) {
                    auto const *ct = cn.as_table();
                    if (ct == nullptr) {
                        return failure(ErrorCode::ParseError,
                                       "[[edit.byte_changes]] element at index " +
                                           std::to_string(change_idx) + " (edit #" +
                                           std::to_string(edit_idx) + ") is not a table");
                    }
                    edit::ByteEdit::Change c{};
                    c.address =
                        static_cast<std::size_t>((*ct)["address"].value_or<std::int64_t>(0));
                    auto const before_raw = (*ct)["before"].value_or<std::int64_t>(-1);
                    auto const after_raw = (*ct)["after"].value_or<std::int64_t>(-1);
                    if (before_raw < 0 || before_raw > 255 || after_raw < 0 || after_raw > 255) {
                        return failure(ErrorCode::ParseError,
                                       "[[edit.byte_changes]] before/after must be 0..255 "
                                       "(edit #" +
                                           std::to_string(edit_idx) + " change #" +
                                           std::to_string(change_idx) +
                                           " got before=" + std::to_string(before_raw) +
                                           " after=" + std::to_string(after_raw) + ")");
                    }
                    c.before = static_cast<std::uint8_t>(before_raw);
                    c.after = static_cast<std::uint8_t>(after_raw);
                    b.changes.push_back(c);
                    ++change_idx;
                }
                e.payload = std::move(b);
            } else {
                edit::TableEdit t;
                t.table_id = (*et)["table_id"].value_or<std::string>("");

                auto const *before_t = (*et)["before"].as_table();
                auto const *after_t = (*et)["after"].as_table();
                if (before_t == nullptr || after_t == nullptr) {
                    return failure(ErrorCode::ParseError, "[[edit]] #" +
                                                              std::to_string(edit_idx) +
                                                              " missing [edit.before] or "
                                                              "[edit.after]");
                }
                auto before_r = parse_snapshot(*before_t);
                if (!before_r.has_value())
                    return failure(before_r.error());
                auto after_r = parse_snapshot(*after_t);
                if (!after_r.has_value())
                    return failure(after_r.error());
                t.before = std::move(*before_r);
                t.after = std::move(*after_r);
                e.payload = std::move(t);
            }
            edits.push_back(std::move(e));
            ++edit_idx;
        }
    }

    edit::History h;
    h.load(std::move(edits), cursor);
    return h;
}

std::string render_project_toml(Project const &p, std::uint32_t source_crc32,
                                std::uint32_t working_crc32, std::string const &created,
                                std::filesystem::path const &source_rel,
                                std::filesystem::path const &working_rel,
                                std::filesystem::path const &def_rel) {
    std::ostringstream ss;
    auto const emit_string = [&](std::string_view k, std::string_view v) {
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
    emit_string("display_name  ", p.display_name());
    emit_string("created       ", created);
    emit_string("notes         ", p.notes());
    emit_string("policy_profile", policy::profile_name(p.policy_profile()));
    // active_rom_id is emitted unconditionally so an empty value
    // round-trips identically to an absent field; older loaders that
    // don't know the key silently ignore it.
    emit_string("active_rom_id ", p.active_rom_id());
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

    // Optional aftermarket-vendor handheld serial. Lives in its own
    // top-level table per docs/21-stune-format.md's forward-compat rule
    // (new optional tables are silently ignored by older loaders). The
    // table is emitted unconditionally so a present-but-empty value
    // round-trips identically to an absent value.
    ss << "\n";
    ss << "[security_access]\n";
    emit_string("handheld_serial", p.handheld_serial());

    // Additional ROMs (Issue #10). Same forward-compat rule: array of
    // tables; older loaders ignore. Each entry is its own [[rom]]
    // table so the file stays grep-friendly.
    for (auto const &r : p.additional_roms()) {
        ss << "\n";
        ss << "[[rom]]\n";
        emit_string("id          ", r.id);
        emit_string("display_name", r.display_name);
        emit_string("path        ", r.path_rel.generic_string());
        if (r.crc32 != 0) {
            ss << "crc32        = " << r.crc32 << "\n";
        }
        if (!r.notes.empty()) {
            emit_string("notes       ", r.notes);
        }
    }
    return std::move(ss).str();
}

} // namespace

Result<Project> Project::create(std::filesystem::path const &project_dir,
                                std::filesystem::path const &source_rom_path,
                                std::filesystem::path const &definition_path,
                                std::string display_name) {
    std::error_code ec;
    if (std::filesystem::exists(project_dir, ec)) {
        if (!std::filesystem::is_directory(project_dir, ec) || ec) {
            return failure(ErrorCode::InvalidArgument,
                           "project path exists and is not a directory: " + project_dir.string());
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
    if (!source_load.has_value())
        return failure(source_load.error());

    auto def_load = Definition::from_file(definition_path);
    if (!def_load.has_value())
        return failure(def_load.error());

    Project p;
    p.dir_ = project_dir;
    p.display_name_ = std::move(display_name);
    p.notes_ = "";
    p.created_ = iso8601_utc_now();
    p.source_ = std::move(*source_load);
    p.source_crc32_ = p.source_.crc32();
    // Working starts as a byte-for-byte copy of source.
    p.working_ = Rom::from_bytes(
        std::vector<std::uint8_t>(p.source_.data().begin(), p.source_.data().end()));
    p.def_ = std::move(*def_load);

    // Compute a relative path to the definition. If the definition is outside
    // the project dir we store the relative (possibly going up with ..) so
    // the project remains portable as long as the user moves both.
    std::error_code def_ec;
    auto const def_abs = std::filesystem::weakly_canonical(definition_path, def_ec);
    auto const proj_abs = std::filesystem::weakly_canonical(project_dir, def_ec);
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

    if (auto s = p.save_metadata(); !s.has_value())
        return failure(s.error());
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
        return failure(ErrorCode::FileNotFound, "project.toml missing in: " + project_dir.string());
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
        return failure(ErrorCode::UnsupportedVersion, "project schema_version " +
                                                          std::to_string(schema) +
                                                          " is newer than this build supports (" +
                                                          std::to_string(kSchemaVersion) + ")");
    }

    Project p;
    p.dir_ = project_dir;
    p.display_name_ = (*project)["display_name"].value_or<std::string>("");
    p.notes_ = (*project)["notes"].value_or<std::string>("");
    p.created_ = (*project)["created"].value_or<std::string>("");
    if (auto const pp = (*project)["policy_profile"].value<std::string>();
        pp.has_value() && !pp->empty()) {
        auto const parsed = policy::parse_profile(*pp);
        if (!parsed.has_value()) {
            return failure(ErrorCode::ParseError, "project.policy_profile unknown: '" + *pp +
                                                      "' (valid: motorsport-only, alberta-ca, "
                                                      "eu-roadworthy, california-us)");
        }
        p.policy_profile_ = *parsed;
    }
    // else: default is MotorsportOnly via member init — silently OK for
    // older projects that pre-date the field.

    p.active_rom_id_ = (*project)["active_rom_id"].value_or<std::string>("");
    // Don't validate against additional_roms_ here — that vector hasn't
    // been populated yet at this point in open(). A stale id that no
    // longer matches an additional rom is tolerated: read-side
    // consumers fall back to the working slot when find_rom_by_id
    // returns nullptr.

    if (auto const *sa = tbl["security_access"].as_table(); sa != nullptr) {
        p.handheld_serial_ = (*sa)["handheld_serial"].value_or<std::string>("");
    }
    // else: [security_access] table absent (older project, or no SA-related
    // state to persist yet) — handheld_serial_ stays default-empty.

    auto const get_path = [&](toml::table const &t, char const *key) -> std::string {
        return t[key].value_or<std::string>("");
    };

    auto const *src_tbl = (*project)["source_rom"].as_table();
    auto const *wrk_tbl = (*project)["working_rom"].as_table();
    auto const *def_tbl = (*project)["definition"].as_table();
    if (src_tbl == nullptr || wrk_tbl == nullptr || def_tbl == nullptr) {
        return failure(ErrorCode::ParseError, "[project.source_rom], [project.working_rom], and "
                                              "[project.definition] are all required");
    }

    p.source_rel_ = get_path(*src_tbl, "path");
    p.working_rel_ = get_path(*wrk_tbl, "path");
    p.def_rel_ = get_path(*def_tbl, "path");
    p.source_crc32_ = static_cast<std::uint32_t>((*src_tbl)["crc32"].value_or<std::int64_t>(0));

    auto src = Rom::from_file(project_dir / p.source_rel_);
    if (!src.has_value())
        return failure(src.error());
    p.source_ = std::move(*src);

    auto wrk = Rom::from_file(project_dir / p.working_rel_);
    if (!wrk.has_value())
        return failure(wrk.error());
    p.working_ = std::move(*wrk);

    auto const def_resolved = std::filesystem::weakly_canonical(project_dir / p.def_rel_, ec);
    auto def = Definition::from_file(ec ? (project_dir / p.def_rel_) : def_resolved);
    if (!def.has_value())
        return failure(def.error());
    p.def_ = std::move(*def);

    // Additional ROMs (Issue #10 — multi-ROM read slice). Optional
    // [[rom]] array; each entry references a ROM file relative to the
    // project dir. Failures here are non-fatal: load whatever we can,
    // skip entries with missing files. The user's main source+working
    // workflow keeps working even if a referenced extra ROM has been
    // moved or deleted.
    if (auto const *arr = tbl["rom"].as_array(); arr != nullptr) {
        p.additional_roms_.reserve(arr->size());
        std::size_t entry_idx = 0;
        for (auto const &node : *arr) {
            ++entry_idx;
            auto const *rt = node.as_table();
            if (rt == nullptr) {
                p.additional_rom_warnings_.push_back(
                    "[[rom]] entry #" + std::to_string(entry_idx) +
                    " is not a table; skipped");
                continue;
            }
            Project::AdditionalRom entry;
            entry.id = (*rt)["id"].value_or<std::string>("");
            if (entry.id.empty()) {
                p.additional_rom_warnings_.push_back(
                    "[[rom]] entry #" + std::to_string(entry_idx) +
                    " missing required field 'id'; skipped");
                continue;
            }
            entry.display_name = (*rt)["display_name"].value_or<std::string>("");
            if (entry.display_name.empty()) {
                entry.display_name = entry.id;
            }
            entry.notes = (*rt)["notes"].value_or<std::string>("");
            entry.crc32 = static_cast<std::uint32_t>(
                (*rt)["crc32"].value_or<std::int64_t>(0));
            entry.path_rel = get_path(*rt, "path");
            if (entry.path_rel.empty()) {
                p.additional_rom_warnings_.push_back(
                    "[[rom]] '" + entry.id + "' missing required field 'path'; skipped");
                continue;
            }
            auto rom_r = Rom::from_file(project_dir / entry.path_rel);
            if (!rom_r.has_value()) {
                p.additional_rom_warnings_.push_back(
                    "[[rom]] '" + entry.id + "' could not load '" + entry.path_rel.string() +
                    "': " + rom_r.error().to_string() + "; skipped");
                continue;
            }
            entry.rom = std::move(*rom_r);
            // Per-ROM history is optional. <id>.edits.toml lives
            // alongside the ROM bytes; absence = empty history (the
            // common case for a freshly-registered additional). A
            // parse failure on the history file is non-fatal: the
            // ROM still loads, the history starts empty, and the
            // user gets a warning. This matches the additional-rom
            // shape where bad metadata doesn't sink the whole open.
            auto const hist_path = project_dir / (entry.id + ".edits.toml");
            if (std::filesystem::exists(hist_path, ec) && !ec) {
                std::ifstream hin{hist_path};
                std::ostringstream hcontents;
                hcontents << hin.rdbuf();
                auto hist_r = parse_history_toml(hcontents.str());
                if (hist_r.has_value()) {
                    entry.history = std::move(*hist_r);
                } else {
                    p.additional_rom_warnings_.push_back(
                        "[[rom]] '" + entry.id + "' edits.toml failed to parse: " +
                        hist_r.error().to_string() + "; history reset to empty");
                }
            }
            p.additional_roms_.push_back(std::move(entry));
        }
    }

    // edits.toml is optional. If present, restore the edit history so
    // cross-session undo works.
    auto const edits_path = project_dir / "edits.toml";
    if (std::filesystem::exists(edits_path, ec) && !ec) {
        std::ifstream ein{edits_path};
        std::ostringstream econtents;
        econtents << ein.rdbuf();
        auto hist = parse_history_toml(econtents.str());
        if (!hist.has_value())
            return failure(hist.error());
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
    auto const toml_text = render_project_toml(*this, source_crc32_, working_.crc32(), created_,
                                               source_rel_, working_rel_, def_rel_);
    return write_file(dir_ / "project.toml", toml_text);
}

Rom const *Project::find_rom_by_id(std::string_view id) const noexcept {
    if (id.empty() || id == "working") {
        return &working_;
    }
    if (id == "source") {
        return &source_;
    }
    for (auto const &r : additional_roms_) {
        if (r.id == id) {
            return &r.rom;
        }
    }
    return nullptr;
}

edit::History const &Project::active_history() const noexcept {
    // Walk additional_roms_ first so an additional id that happens to
    // share a name with the (legal) empty/working/source vocabulary
    // can't slip through — add_additional_rom rejects those slugs, but
    // a fall-through is the right shape regardless.
    if (!active_rom_id_.empty() && active_rom_id_ != "working" &&
        active_rom_id_ != "source") {
        for (auto const &r : additional_roms_) {
            if (r.id == active_rom_id_) {
                return r.history;
            }
        }
    }
    return history_;
}

edit::History &Project::active_history() noexcept {
    if (!active_rom_id_.empty() && active_rom_id_ != "working" &&
        active_rom_id_ != "source") {
        for (auto &r : additional_roms_) {
            if (r.id == active_rom_id_) {
                return r.history;
            }
        }
    }
    return history_;
}

Rom *Project::active_rom_mut() noexcept {
    if (active_rom_id_.empty() || active_rom_id_ == "working") {
        return &working_;
    }
    if (active_rom_id_ == "source") {
        return nullptr; // immutable by contract
    }
    for (auto &r : additional_roms_) {
        if (r.id == active_rom_id_) {
            return &r.rom;
        }
    }
    return nullptr;
}

Status Project::set_active_rom_id(std::string_view id) {
    if (id.empty() || id == "working" || id == "source") {
        active_rom_id_.assign(id);
        return {};
    }
    for (auto const &r : additional_roms_) {
        if (r.id == id) {
            active_rom_id_.assign(id);
            return {};
        }
    }
    return failure(ErrorCode::InvalidArgument,
                   "no ROM with id '" + std::string{id} +
                       "' in this project (use 'working', 'source', or an id from "
                       "additional_roms())");
}

Status Project::save_active_rom() {
    // Working slot keeps the v1 file layout (working.bin + edits.toml)
    // so opening a project that only ever edited working stays
    // byte-identical on disk.
    if (active_rom_id_.empty() || active_rom_id_ == "working") {
        return save_working_rom();
    }
    if (active_rom_id_ == "source") {
        // Source is immutable. Asking to save it is a programmer
        // error — UI surfaces gate this off before the call. Reject
        // explicitly rather than silently no-op so a stray call site
        // surfaces in tests.
        return failure(ErrorCode::InvalidArgument,
                       "save_active_rom: source ROM is immutable");
    }
    // Additional ROM. Locate by id, write its bytes to path_rel, write
    // its history alongside as <id>.edits.toml. CRC32 in the in-
    // memory record is refreshed so save_metadata emits the current
    // value into project.toml.
    AdditionalRom *target = nullptr;
    for (auto &r : additional_roms_) {
        if (r.id == active_rom_id_) {
            target = &r;
            break;
        }
    }
    if (target == nullptr) {
        return failure(ErrorCode::InvalidArgument,
                       "save_active_rom: no additional ROM with id '" +
                           active_rom_id_ + "'");
    }

    auto const rom_path = dir_ / target->path_rel;
    {
        std::ofstream out{rom_path, std::ios::binary};
        if (!out) {
            return failure(ErrorCode::IoFailure, "cannot open: " + rom_path.string());
        }
        out.write(reinterpret_cast<char const *>(target->rom.data().data()),
                  static_cast<std::streamsize>(target->rom.size()));
        if (!out) {
            return failure(ErrorCode::IoFailure, "write failed: " + rom_path.string());
        }
    }
    target->crc32 = target->rom.crc32();

    // History toml is optional — only emit when there's something to
    // record. A pre-existing file for a now-empty history is left in
    // place so the user can recover it manually; the loader simply
    // restores an empty cursor in that case. This keeps the
    // surprise-resistance the working slot already enjoys.
    if (target->history.size() > 0) {
        auto const edits_text = render_history_toml(target->history);
        auto const edits_path = dir_ / (active_rom_id_ + ".edits.toml");
        if (auto s = write_file(edits_path, edits_text); !s.has_value()) {
            return s;
        }
    }
    return save_metadata();
}

Status Project::save_all() {
    // Always save working — even when history is empty, the user may
    // have just hit Ctrl+S to "checkpoint" the project. Cheap on a
    // 2 MB ROM and matches the v1 save_working_rom contract.
    if (auto s = save_working_rom(); !s.has_value()) {
        return s;
    }
    // Save additional ROMs that carry edit history. A ROM with no
    // history hasn't been edited via the GUI — skip to avoid
    // rewriting bytes the loader will read identically.
    for (auto &r : additional_roms_) {
        if (r.history.size() == 0) {
            continue;
        }
        auto const rom_path = dir_ / r.path_rel;
        std::ofstream out{rom_path, std::ios::binary};
        if (!out) {
            return failure(ErrorCode::IoFailure, "cannot open: " + rom_path.string());
        }
        out.write(reinterpret_cast<char const *>(r.rom.data().data()),
                  static_cast<std::streamsize>(r.rom.size()));
        if (!out) {
            return failure(ErrorCode::IoFailure, "write failed: " + rom_path.string());
        }
        auto const edits_text = render_history_toml(r.history);
        auto const edits_path = dir_ / (r.id + ".edits.toml");
        if (auto s = write_file(edits_path, edits_text); !s.has_value()) {
            return s;
        }
        // crc32 in the in-memory record was set at registration time;
        // refresh it so save_metadata below emits the value matching
        // what we just wrote to disk.
        r.crc32 = r.rom.crc32();
    }
    return save_metadata();
}

Status Project::add_additional_rom(AdditionalRom entry) {
    if (entry.id.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "additional rom id must not be empty");
    }
    // Reserved ids: find_rom_by_id privileges these for the built-in
    // slots, so accepting an additional ROM under the same id would
    // create an unreachable record. Reject early with a clear
    // explanation instead.
    if (entry.id == "source" || entry.id == "working") {
        return failure(ErrorCode::InvalidArgument,
                       "additional rom id '" + entry.id +
                           "' is reserved (use a different slug — 'source' and "
                           "'working' name the built-in project ROM slots)");
    }
    for (auto const &r : additional_roms_) {
        if (r.id == entry.id) {
            return failure(ErrorCode::InvalidArgument,
                           "additional rom id '" + entry.id +
                               "' already exists in this project");
        }
    }
    if (entry.display_name.empty()) {
        entry.display_name = entry.id;
    }
    if (entry.crc32 == 0) {
        entry.crc32 = entry.rom.crc32();
    }
    additional_roms_.push_back(std::move(entry));
    return {};
}

// --------------------------------------------------------------------------
// parse_edit_csv — bulk-edit CSV parser shared by the CLI and the GUI.
//
// Rules (kept identical to the prior CLI-embedded version):
//   * `# pack_id = "X"` and `# table = "Y"` are identity headers that live
//     INSIDE a `#` comment so the file still parses cleanly through generic
//     CSV tools (Excel, Python's csv module, etc.).
//   * pack_id mismatch → warning (returned in result.warnings).
//   * table mismatch   → hard error (InvalidArgument).
//   * Comment portion of a line (`#` and onward) is stripped before
//     field-splitting; blank/comment-only lines skip.
//   * A non-numeric first row is tolerated as a CSV header row IFF no
//     edits have been parsed yet — protects against `row,col,value` being
//     read as a literal data row.
//   * `row,col,value` — value is parsed as double in engineering units
//     (the pack's scaling applies during writeback by the caller).
//
namespace {

// "Separator" = whitespace or `;`. Spreadsheet round-trips through
// Calc / Excel in non-US locales splatter `;` through the comment lines
// (e.g. `#;pack_id;=;demo;;`); treating `;` as a separator keeps the
// identity-header checks alive on those files.
bool is_sep(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == ';';
}

std::string extract_quoted(std::string_view line) {
    auto const eq = line.find('=');
    if (eq == std::string_view::npos)
        return {};
    auto rest = line.substr(eq + 1);
    while (!rest.empty() && is_sep(rest.front()))
        rest.remove_prefix(1);
    if (rest.size() >= 2 && rest.front() == '"') {
        auto const close = rest.find('"', 1);
        if (close != std::string_view::npos) {
            return std::string{rest.substr(1, close - 1)};
        }
    }
    // Strip trailing separators so a value like `demo;;` reads as `demo`.
    while (!rest.empty() && is_sep(rest.back()))
        rest.remove_suffix(1);
    return std::string{rest};
}

bool parse_size_field(std::string_view sv, std::size_t &out) {
    char const *first = sv.data();
    char const *last = sv.data() + sv.size();
    while (first < last && std::isspace(static_cast<unsigned char>(*first)))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(*(last - 1))))
        --last;
    std::size_t v = 0;
    auto const res = std::from_chars(first, last, v);
    if (res.ec != std::errc{} || res.ptr != last)
        return false;
    out = v;
    return true;
}

bool parse_double_field(std::string_view sv, double &out) {
    // Trim surrounding whitespace to match parse_size_field semantics — a
    // value like " 5.0 " or "5.0 # ..." (after comment strip → "5.0 ")
    // should parse cleanly.
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    if (sv.empty())
        return false;
    std::string s{sv};
    char *end = nullptr;
    double const d = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0')
        return false;
    out = d;
    return true;
}

} // namespace

Result<EditCsvParseResult> parse_edit_csv(std::string_view text, EditCsvParseOptions const &opts) {
    EditCsvParseResult result;

    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        ++line_no;
        std::size_t const eol = text.find('\n', pos);
        std::size_t const end = (eol == std::string_view::npos) ? text.size() : eol;
        std::string_view line = text.substr(pos, end - pos);
        pos = (eol == std::string_view::npos) ? text.size() + 1 : eol + 1;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Identity-header detection — pre-comment-strip so the `# pack_id`
        // and `# table` markers stay inside a CSV comment.
        if (!line.empty() && line.front() == '#') {
            std::string_view rest = line.substr(1);
            while (!rest.empty() && is_sep(rest.front()))
                rest.remove_prefix(1);
            if (rest.starts_with("pack_id")) {
                auto const declared = extract_quoted(rest);
                if (!opts.expected_pack_id.empty() && !declared.empty() &&
                    declared != opts.expected_pack_id) {
                    result.warnings.push_back({"CSV pack_id=\"" + declared +
                                               "\" differs from project pack=\"" +
                                               std::string{opts.expected_pack_id} +
                                               "\"; scaling and addresses may not match"});
                }
            } else if (rest.starts_with("table")) {
                auto const declared = extract_quoted(rest);
                if (!opts.expected_table_id.empty() && !declared.empty() &&
                    declared != opts.expected_table_id) {
                    return failure(ErrorCode::InvalidArgument,
                                   "CSV table=\"" + declared + "\" differs from target table \"" +
                                       std::string{opts.expected_table_id} + "\"");
                }
            }
        }

        // Strip the inline-comment tail.
        std::string clean{line};
        if (auto p = clean.find('#'); p != std::string::npos) {
            clean.resize(p);
        }

        // Skip blank / whitespace-only lines.
        bool blank = true;
        for (char c : clean) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                blank = false;
                break;
            }
        }
        if (blank)
            continue;

        // Split on commas OR semicolons. Spreadsheet round-trips
        // (Excel / LibreOffice / OpenOffice in non-US locales) rewrite
        // comma-delimited CSVs with `;` and pad rows with trailing empty
        // separators; accepting both keeps the export → edit-in-Calc →
        // re-import workflow alive. Numeric values can't contain either
        // character so there's no ambiguity.
        // Trailing separator → trailing empty field, which fails
        // field-count or numeric-parse below.
        std::vector<std::string_view> fields;
        {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= clean.size(); ++i) {
                if (i == clean.size() || clean[i] == ',' || clean[i] == ';') {
                    fields.push_back(std::string_view{clean}.substr(start, i - start));
                    start = i + 1;
                }
            }
        }
        if (fields.size() < 3) {
            return failure(ErrorCode::ParseError, "line " + std::to_string(line_no) +
                                                      ": expected 3 fields, got " +
                                                      std::to_string(fields.size()));
        }

        std::size_t r = 0, c = 0;
        double v = 0.0;
        if (!parse_size_field(fields[0], r) || !parse_size_field(fields[1], c)) {
            // First-line header tolerance — `row,col,value` parses as
            // non-integers and is skipped exactly once at the top.
            if (result.cells.empty())
                continue;
            return failure(ErrorCode::ParseError,
                           "line " + std::to_string(line_no) + ": row/col not integers");
        }
        if (!parse_double_field(fields[2], v)) {
            return failure(ErrorCode::ParseError, "line " + std::to_string(line_no) + ": value '" +
                                                      std::string{fields[2]} + "' is not numeric");
        }
        if (opts.table_rows > 0 && opts.table_cols > 0) {
            if (r >= opts.table_rows || c >= opts.table_cols) {
                return failure(ErrorCode::OutOfRange,
                               "line " + std::to_string(line_no) + ": (" + std::to_string(r) + "," +
                                   std::to_string(c) + ") is outside table (" +
                                   std::to_string(opts.table_rows) + " rows x " +
                                   std::to_string(opts.table_cols) + " cols)");
            }
        }
        result.cells.push_back({r, c, v});
    }
    return result;
}

} // namespace st
