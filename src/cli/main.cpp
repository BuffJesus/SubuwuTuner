// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/version.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kUsage =
    "subuwutuner-cli — headless ECU calibration tool\n"
    "\n"
    "USAGE:\n"
    "    subuwutuner-cli [OPTIONS] [COMMAND] [ARGS...]\n"
    "\n"
    "OPTIONS:\n"
    "    -h, --help              Print this help and exit\n"
    "    -V, --version           Print version and exit\n"
    "\n"
    "COMMANDS:\n"
    "    rom-info [--def <pack.toml>] <FILE>\n"
    "                            Print size, CRC32, embedded ASCII strings of a ROM.\n"
    "                            With --def, also identify the ROM against the pack\n"
    "                            and summarize its tables.\n"
    "    dump-axis --def <pack.toml> --axis <id> <FILE>\n"
    "                            Read the named axis from the ROM via the pack and\n"
    "                            print its scaled values, one per line.\n"
    "    dump-table --def <pack.toml> --table <id> <FILE>\n"
    "                            Read the named table from the ROM via the pack and\n"
    "                            print it as a labeled grid.\n"
    "    rom-diff --def <pack.toml> <A.bin> <B.bin>\n"
    "                            Compare two ROMs of the same definition table-by-\n"
    "                            table. Reports which tables changed and by how\n"
    "                            much (max delta, mean absolute delta).\n"
    "    table-edit --def <pack.toml> --table <id> [--rows A:B] [--cols A:B]\n"
    "               OP [VALUE] <FILE> --output <OUT>\n"
    "                            Edit a table in <FILE> and write the result to\n"
    "                            <OUT>. OP is one of: set, add, multiply, percent,\n"
    "                            smooth, interpolate. VALUE is required for the\n"
    "                            first four ops and ignored for smooth/interpolate.\n"
    "    project-new --source <rom> --def <pack> [--name <name>] <dir>\n"
    "                            Create a new .stune project directory containing\n"
    "                            a copy of the source ROM, an editable working\n"
    "                            ROM, and a reference to the definition pack.\n"
    "    project-info <dir>      Print metadata + current working-ROM CRC32 for\n"
    "                            a .stune project.\n"
    "    project-edit --table <id> [--rows A:B] [--cols A:B] OP [VALUE] <dir>\n"
    "                            Apply an edit to a project's working ROM and\n"
    "                            update project.toml. Same OPs as table-edit.\n";

void print_version() {
    std::printf("%.*s %.*s\n",
                static_cast<int>(st::Version::name().size()), st::Version::name().data(),
                static_cast<int>(st::Version::string().size()), st::Version::string().data());
}

void print_usage() { std::fputs(kUsage.data(), stdout); }

bool arg_matches(char const *arg, std::string_view short_form, std::string_view long_form) {
    std::string_view const sv{arg};
    return sv == short_form || sv == long_form;
}

// Forward-declared so commands defined ahead of parse_range's body can use it.
bool parse_range(std::string_view s, std::size_t &lo, std::size_t &hi);

void print_rom_summary(std::filesystem::path const &path, st::Rom const &rom) {
    std::printf("File:           %s\n", path.string().c_str());
    std::printf("Size:           %zu bytes (%.2f KiB)\n", rom.size(),
                static_cast<double>(rom.size()) / 1024.0);
    std::printf("CRC32:          0x%08X\n", rom.crc32());

    auto const strings = rom.scan_ascii(/*min_length=*/5);
    std::printf("Embedded ASCII: %zu strings (>=5 chars)\n", strings.size());

    constexpr std::size_t kMaxToPrint = 32;
    auto const            limit       = strings.size() < kMaxToPrint ? strings.size() : kMaxToPrint;
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &s = strings[i];
        std::printf("  0x%08zX  (%2zu chars)  %s\n", s.offset, s.text.size(), s.text.c_str());
    }
    if (strings.size() > kMaxToPrint) {
        std::printf("  ... %zu more not shown\n", strings.size() - kMaxToPrint);
    }
}

void print_def_summary(st::Definition const &def, st::Rom const &rom) {
    auto const &pack = def.pack();
    std::printf("\nDefinition pack: %s\n", pack.id.c_str());
    if (!pack.display_name.empty()) {
        std::printf("  Display name:  %s\n", pack.display_name.c_str());
    }
    if (!pack.platform.empty()) {
        std::printf("  Platform:      %s\n", pack.platform.c_str());
    }
    if (pack.rom_size_bytes != 0) {
        std::printf("  Expected ROM:  %zu bytes\n", pack.rom_size_bytes);
        if (rom.size() != pack.rom_size_bytes) {
            std::printf("  ! ROM size mismatch (got %zu)\n", rom.size());
        }
    }

    auto const match = def.matches(rom);
    if (match.has_value()) {
        std::printf("  Match:         %s\n", match->c_str());
    } else {
        std::printf("  Match:         (none — CID at declared offset does not match)\n");
    }

    std::printf("\nTables defined: %zu\n", def.tables().size());
    constexpr std::size_t kMaxTables = 16;
    auto const limit = def.tables().size() < kMaxTables ? def.tables().size() : kMaxTables;
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &t = def.tables()[i];
        std::printf("  %dD  0x%08zX  %-32s  %s\n", t.dimensions, t.address, t.id.c_str(),
                    t.name.c_str());
    }
    if (def.tables().size() > kMaxTables) {
        std::printf("  ... %zu more not shown\n", def.tables().size() - kMaxTables);
    }
    std::printf("PIDs defined:   %zu\n", def.pids().size());
}

int cmd_dump_axis(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string>           axis_id;
    std::optional<std::filesystem::path> rom_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("dump-axis: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--axis") {
            if (i + 1 >= argc) {
                std::fputs("dump-axis: --axis requires an id\n", stderr);
                return 2;
            }
            axis_id = std::string{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "dump-axis: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "dump-axis: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !axis_id.has_value() || !rom_path.has_value()) {
        std::fputs(
            "dump-axis: missing required arguments\n"
            "Usage: subuwutuner-cli dump-axis --def <pack.toml> --axis <id> <FILE>\n",
            stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(*def_path);
    if (!def.has_value()) {
        std::fprintf(stderr, "dump-axis: %s\n", def.error().to_string().c_str());
        return 1;
    }
    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "dump-axis: %s\n", rom.error().to_string().c_str());
        return 1;
    }

    auto const axis = def->find_axis(*axis_id);
    if (axis == nullptr) {
        std::fprintf(stderr, "dump-axis: axis '%s' not found in pack\n", axis_id->c_str());
        std::fputs("Available axes:\n", stderr);
        for (auto const &a : def->axes()) {
            std::fprintf(stderr, "  %s\n", a.id.c_str());
        }
        return 1;
    }

    auto const values = def->read_axis_values(*rom, *axis);
    if (!values.has_value()) {
        std::fprintf(stderr, "dump-axis: %s\n", values.error().to_string().c_str());
        return 1;
    }

    auto const *scaling = def->find_scaling(axis->scaling);
    auto const  unit    = (scaling != nullptr ? scaling->unit : axis->unit);
    auto const  precision =
        scaling != nullptr ? scaling->precision : 0;

    std::printf("# %s  (%zu values%s%s)\n", axis->id.c_str(), values->size(),
                unit.empty() ? "" : ", unit=", unit.c_str());
    for (auto const v : *values) {
        std::printf("%.*f\n", precision, v);
    }
    return 0;
}

int cmd_dump_table(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string>           table_id;
    std::optional<std::filesystem::path> rom_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("dump-table: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--table") {
            if (i + 1 >= argc) {
                std::fputs("dump-table: --table requires an id\n", stderr);
                return 2;
            }
            table_id = std::string{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "dump-table: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "dump-table: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !table_id.has_value() || !rom_path.has_value()) {
        std::fputs(
            "dump-table: missing required arguments\n"
            "Usage: subuwutuner-cli dump-table --def <pack.toml> --table <id> <FILE>\n",
            stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(*def_path);
    if (!def.has_value()) {
        std::fprintf(stderr, "dump-table: %s\n", def.error().to_string().c_str());
        return 1;
    }
    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "dump-table: %s\n", rom.error().to_string().c_str());
        return 1;
    }

    auto const *table = def->find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "dump-table: table '%s' not found in pack\n", table_id->c_str());
        std::fputs("Available tables:\n", stderr);
        for (auto const &t : def->tables()) {
            std::fprintf(stderr, "  %s\n", t.id.c_str());
        }
        return 1;
    }

    auto const td = def->read_table_values(*rom, *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "dump-table: %s\n", td.error().to_string().c_str());
        return 1;
    }

    auto const *scal      = def->find_scaling(table->scaling);
    auto const  precision = scal != nullptr ? scal->precision : 0;
    auto const  unit      = scal != nullptr ? scal->unit : std::string{};

    std::printf("# %s  (%dD", table->id.c_str(), table->dimensions);
    if (!table->name.empty()) std::printf(", %s", table->name.c_str());
    if (!unit.empty())        std::printf(", unit=%s", unit.c_str());
    std::printf(")\n");

    constexpr int kColWidth = 10;
    auto const &  xs        = td->axis_x;
    auto const &  ys        = td->axis_y;

    // Header row.
    std::printf("%*s", kColWidth, "");
    for (auto const x : xs) {
        std::printf(" %*.*f", kColWidth - 1, precision, x);
    }
    std::printf("\n");

    for (std::size_t r = 0; r < td->values.size(); ++r) {
        if (!ys.empty()) {
            std::printf("%*.*f", kColWidth, precision, ys[r]);
        } else {
            std::printf("%*s", kColWidth, "");
        }
        for (auto const v : td->values[r]) {
            std::printf(" %*.*f", kColWidth - 1, precision, v);
        }
        std::printf("\n");
    }

    return 0;
}

int cmd_project_edit(int argc, char *argv[]) {
    std::optional<std::string>           table_id;
    std::optional<std::string>           rows_arg;
    std::optional<std::string>           cols_arg;
    std::optional<std::string>           op;
    std::optional<double>                value;
    std::optional<std::filesystem::path> proj_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-edit: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--table") {
            if (auto const *v = require("--table"); v) table_id = std::string{v};
            else return 2;
        } else if (a == "--rows") {
            if (auto const *v = require("--rows"); v) rows_arg = std::string{v};
            else return 2;
        } else if (a == "--cols") {
            if (auto const *v = require("--cols"); v) cols_arg = std::string{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-edit: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!op.has_value()) {
            op = std::string{a};
        } else if (!value.has_value() && op != "smooth" && op != "interpolate") {
            double     d   = 0.0;
            auto const res = std::from_chars(a.data(), a.data() + a.size(), d);
            if (res.ec == std::errc{} && res.ptr == a.data() + a.size()) {
                value = d;
            } else if (!proj_path.has_value()) {
                proj_path = std::filesystem::path{a};
            } else {
                std::fprintf(stderr, "project-edit: extra argument: %s\n", argv[i]);
                return 2;
            }
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-edit: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!table_id.has_value() || !op.has_value() || !proj_path.has_value()) {
        std::fputs(
            "project-edit: missing required arguments\n"
            "Usage: subuwutuner-cli project-edit --table <id> [--rows A:B] "
            "[--cols A:B] OP [VALUE] <dir>\n",
            stderr);
        return 2;
    }

    bool const op_needs_value =
        *op == "set" || *op == "add" || *op == "multiply" || *op == "percent";
    if (op_needs_value && !value.has_value()) {
        std::fprintf(stderr, "project-edit: op '%s' requires a numeric value\n",
                     op->c_str());
        return 2;
    }

    auto proj = st::Project::open(*proj_path);
    if (!proj.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", proj.error().to_string().c_str());
        return 1;
    }

    auto const *table = proj->definition().find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "project-edit: table '%s' not found in pack\n",
                     table_id->c_str());
        return 1;
    }

    auto td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", td.error().to_string().c_str());
        return 1;
    }

    st::edit::Rect rect = st::edit::whole_table(*td);
    if (rows_arg.has_value() && !parse_range(*rows_arg, rect.r_start, rect.r_end)) {
        std::fprintf(stderr, "project-edit: bad --rows: %s\n", rows_arg->c_str());
        return 2;
    }
    if (cols_arg.has_value() && !parse_range(*cols_arg, rect.c_start, rect.c_end)) {
        std::fprintf(stderr, "project-edit: bad --cols: %s\n", cols_arg->c_str());
        return 2;
    }

    st::Status status = st::ok();
    if (*op == "set")              status = st::edit::set_cells(*td, rect, *value);
    else if (*op == "add")         status = st::edit::add_cells(*td, rect, *value);
    else if (*op == "multiply")    status = st::edit::multiply_cells(*td, rect, *value);
    else if (*op == "percent")     status = st::edit::percent_scale_cells(*td, rect, *value);
    else if (*op == "smooth")      status = st::edit::smooth_cells(*td, rect, 1);
    else if (*op == "interpolate") status = st::edit::interpolate_cells(*td, rect);
    else {
        std::fprintf(stderr, "project-edit: unknown op '%s'\n", op->c_str());
        return 2;
    }
    if (!status.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", status.error().to_string().c_str());
        return 1;
    }

    auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
    if (!wb.has_value()) {
        std::fprintf(stderr, "project-edit: writeback: %s\n", wb.error().to_string().c_str());
        return 1;
    }

    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "project-edit: save: %s\n", s.error().to_string().c_str());
        return 1;
    }

    std::printf("Table:      %s\n", table->id.c_str());
    std::printf("Op:         %s", op->c_str());
    if (op_needs_value) std::printf(" %g", *value);
    std::printf("\n");
    std::printf("Selection:  rows %zu..%zu, cols %zu..%zu (%zu cells)\n",
                rect.r_start, rect.r_end, rect.c_start, rect.c_end,
                rect.rows() * rect.cols());
    std::printf("Saved to:   %s\n", proj_path->string().c_str());
    std::printf("New CRC32:  0x%08X\n", proj->working_rom().crc32());
    return 0;
}

int cmd_project_new(int argc, char *argv[]) {
    std::optional<std::filesystem::path> source_path;
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> proj_path;
    std::string                          display_name;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "project-new: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--source") {
            if (auto const *v = require("--source"); v) source_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--def") {
            if (auto const *v = require("--def"); v) def_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--name") {
            if (auto const *v = require("--name"); v) display_name = std::string{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "project-new: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!proj_path.has_value()) {
            proj_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "project-new: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!source_path.has_value() || !def_path.has_value() || !proj_path.has_value()) {
        std::fputs(
            "project-new: missing required arguments\n"
            "Usage: subuwutuner-cli project-new --source <rom> --def <pack> "
            "[--name <name>] <dir>\n",
            stderr);
        return 2;
    }
    if (display_name.empty()) {
        display_name = proj_path->filename().string();
    }

    auto p = st::Project::create(*proj_path, *source_path, *def_path, display_name);
    if (!p.has_value()) {
        std::fprintf(stderr, "project-new: %s\n", p.error().to_string().c_str());
        return 1;
    }

    std::printf("Created project: %s\n", proj_path->string().c_str());
    std::printf("  Name:       %s\n", p->display_name().c_str());
    std::printf("  Source:     %s  (CRC32=0x%08X, %zu bytes)\n",
                source_path->string().c_str(), p->source_rom().crc32(), p->source_rom().size());
    std::printf("  Definition: %s  (pack id: %s)\n", def_path->string().c_str(),
                p->definition().pack().id.c_str());
    auto const cid = p->definition().matches(p->source_rom());
    std::printf("  CID match:  %s\n", cid.has_value() ? cid->c_str() : "(no match)");
    return 0;
}

int cmd_project_info(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("project-info: missing project directory\n", stderr);
        std::fputs("Usage: subuwutuner-cli project-info <dir>\n", stderr);
        return 2;
    }
    std::filesystem::path const dir{argv[0]};

    auto p = st::Project::open(dir);
    if (!p.has_value()) {
        std::fprintf(stderr, "project-info: %s\n", p.error().to_string().c_str());
        return 1;
    }

    std::printf("Project:    %s\n", dir.string().c_str());
    std::printf("Name:       %s\n", p->display_name().c_str());
    if (!p->notes().empty()) {
        std::printf("Notes:      %s\n", p->notes().c_str());
    }
    std::printf("Source ROM: %zu bytes, CRC32=0x%08X (recorded: 0x%08X)\n",
                p->source_rom().size(), p->source_rom().crc32(),
                p->source_crc32_at_create());
    if (p->source_rom().crc32() != p->source_crc32_at_create()) {
        std::printf("  ! source.bin has changed since project creation\n");
    }
    std::printf("Working ROM: %zu bytes, CRC32=0x%08X\n",
                p->working_rom().size(), p->working_rom().crc32());
    if (p->source_rom().crc32() == p->working_rom().crc32()) {
        std::printf("  (working matches source — no edits yet)\n");
    } else {
        std::printf("  (working differs from source — edits applied)\n");
    }
    std::printf("Definition: pack id %s\n", p->definition().pack().id.c_str());
    auto const cid = p->definition().matches(p->source_rom());
    std::printf("CID match:  %s\n", cid.has_value() ? cid->c_str() : "(no match)");
    return 0;
}

// Parse "A:B" or "A" into [lo, hi]. Returns false on malformed input.
bool parse_range(std::string_view s, std::size_t &lo, std::size_t &hi) {
    auto const colon = s.find(':');
    auto       parse = [](std::string_view sv, std::size_t &out) {
        std::size_t value = 0;
        auto const  res   = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size()) {
            return false;
        }
        out = value;
        return true;
    };
    if (colon == std::string_view::npos) {
        std::size_t v = 0;
        if (!parse(s, v)) return false;
        lo = hi = v;
        return true;
    }
    return parse(s.substr(0, colon), lo) && parse(s.substr(colon + 1), hi);
}

int cmd_table_edit(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string>           table_id;
    std::optional<std::string>           rows_arg;
    std::optional<std::string>           cols_arg;
    std::optional<std::string>           op;
    std::optional<double>                value;
    std::optional<std::filesystem::path> rom_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "table-edit: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--def") {
            if (auto const *v = require_arg("--def"); v) def_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--table") {
            if (auto const *v = require_arg("--table"); v) table_id = std::string{v};
            else return 2;
        } else if (a == "--rows") {
            if (auto const *v = require_arg("--rows"); v) rows_arg = std::string{v};
            else return 2;
        } else if (a == "--cols") {
            if (auto const *v = require_arg("--cols"); v) cols_arg = std::string{v};
            else return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) output_path = std::filesystem::path{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "table-edit: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!op.has_value()) {
            op = std::string{a};
        } else if (!value.has_value() && op != "smooth" && op != "interpolate") {
            // Try parsing as a double; if it doesn't parse, treat as the ROM path.
            double      d = 0.0;
            auto const  res = std::from_chars(a.data(), a.data() + a.size(), d);
            if (res.ec == std::errc{} && res.ptr == a.data() + a.size()) {
                value = d;
            } else if (!rom_path.has_value()) {
                rom_path = std::filesystem::path{a};
            } else {
                std::fprintf(stderr, "table-edit: extra argument: %s\n", argv[i]);
                return 2;
            }
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "table-edit: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !table_id.has_value() || !op.has_value()
        || !rom_path.has_value() || !output_path.has_value()) {
        std::fputs("table-edit: missing required arguments\n", stderr);
        std::fputs("Usage: subuwutuner-cli table-edit --def <pack.toml> --table <id>\n"
                   "       [--rows A:B] [--cols A:B] OP [VALUE] <FILE> --output <OUT>\n",
                   stderr);
        return 2;
    }

    bool const op_needs_value =
        *op == "set" || *op == "add" || *op == "multiply" || *op == "percent";
    if (op_needs_value && !value.has_value()) {
        std::fprintf(stderr, "table-edit: op '%s' requires a numeric value\n", op->c_str());
        return 2;
    }

    auto const def = st::Definition::from_file(*def_path);
    if (!def.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", def.error().to_string().c_str());
        return 1;
    }
    auto rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", rom.error().to_string().c_str());
        return 1;
    }

    auto const *table = def->find_table(*table_id);
    if (table == nullptr) {
        std::fprintf(stderr, "table-edit: table '%s' not found in pack\n", table_id->c_str());
        return 1;
    }

    auto td = def->read_table_values(*rom, *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", td.error().to_string().c_str());
        return 1;
    }

    st::edit::Rect rect = st::edit::whole_table(*td);
    if (rows_arg.has_value()) {
        if (!parse_range(*rows_arg, rect.r_start, rect.r_end)) {
            std::fprintf(stderr, "table-edit: bad --rows: %s\n", rows_arg->c_str());
            return 2;
        }
    }
    if (cols_arg.has_value()) {
        if (!parse_range(*cols_arg, rect.c_start, rect.c_end)) {
            std::fprintf(stderr, "table-edit: bad --cols: %s\n", cols_arg->c_str());
            return 2;
        }
    }

    st::Status edit_status = st::ok();
    if (*op == "set") {
        edit_status = st::edit::set_cells(*td, rect, *value);
    } else if (*op == "add") {
        edit_status = st::edit::add_cells(*td, rect, *value);
    } else if (*op == "multiply") {
        edit_status = st::edit::multiply_cells(*td, rect, *value);
    } else if (*op == "percent") {
        edit_status = st::edit::percent_scale_cells(*td, rect, *value);
    } else if (*op == "smooth") {
        edit_status = st::edit::smooth_cells(*td, rect, /*iterations=*/1);
    } else if (*op == "interpolate") {
        edit_status = st::edit::interpolate_cells(*td, rect);
    } else {
        std::fprintf(stderr, "table-edit: unknown op '%s'\n", op->c_str());
        return 2;
    }
    if (!edit_status.has_value()) {
        std::fprintf(stderr, "table-edit: %s\n", edit_status.error().to_string().c_str());
        return 1;
    }

    auto wb = def->write_table_values(*rom, *table, *td);
    if (!wb.has_value()) {
        std::fprintf(stderr, "table-edit: writeback: %s\n", wb.error().to_string().c_str());
        return 1;
    }

    std::ofstream out{*output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "table-edit: cannot open output: %s\n",
                     output_path->string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(rom->data().data()),
              static_cast<std::streamsize>(rom->size()));
    if (!out) {
        std::fprintf(stderr, "table-edit: write failed\n");
        return 1;
    }

    std::printf("Table:     %s\n", table->id.c_str());
    std::printf("Op:        %s", op->c_str());
    if (op_needs_value) std::printf(" %g", *value);
    std::printf("\n");
    std::printf("Selection: rows %zu..%zu, cols %zu..%zu (%zu cells)\n",
                rect.r_start, rect.r_end, rect.c_start, rect.c_end,
                rect.rows() * rect.cols());
    std::printf("Output:    %s\n", output_path->string().c_str());
    return 0;
}

int cmd_rom_diff(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> rom_a;
    std::optional<std::filesystem::path> rom_b;
    bool                                 verbose = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("rom-diff: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-diff: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_a.has_value()) {
            rom_a = std::filesystem::path{argv[i]};
        } else if (!rom_b.has_value()) {
            rom_b = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "rom-diff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !rom_a.has_value() || !rom_b.has_value()) {
        std::fputs(
            "rom-diff: missing required arguments\n"
            "Usage: subuwutuner-cli rom-diff --def <pack.toml> <A.bin> <B.bin>\n",
            stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(*def_path);
    if (!def.has_value()) {
        std::fprintf(stderr, "rom-diff: %s\n", def.error().to_string().c_str());
        return 1;
    }
    auto const a = st::Rom::from_file(*rom_a);
    if (!a.has_value()) {
        std::fprintf(stderr, "rom-diff: a: %s\n", a.error().to_string().c_str());
        return 1;
    }
    auto const b = st::Rom::from_file(*rom_b);
    if (!b.has_value()) {
        std::fprintf(stderr, "rom-diff: b: %s\n", b.error().to_string().c_str());
        return 1;
    }

    std::printf("ROM A: %s  (CRC32=0x%08X, %zu bytes)\n", rom_a->string().c_str(),
                a->crc32(), a->size());
    std::printf("ROM B: %s  (CRC32=0x%08X, %zu bytes)\n", rom_b->string().c_str(),
                b->crc32(), b->size());
    std::printf("Pack:  %s\n", def->pack().id.c_str());

    auto const id_a = def->matches(*a);
    auto const id_b = def->matches(*b);
    std::printf("Match A: %s\n", id_a.has_value() ? id_a->c_str() : "(no match)");
    std::printf("Match B: %s\n", id_b.has_value() ? id_b->c_str() : "(no match)");

    std::size_t                changed_count = 0;
    std::size_t                skipped       = 0;
    struct Row {
        std::string id;
        std::size_t total{};
        std::size_t changed{};
        double      max{};
        double      mean{};
        std::string unit;
    };
    std::vector<Row> rows;
    rows.reserve(def->tables().size());

    for (auto const &table : def->tables()) {
        auto const d = def->diff_table(*a, *b, table);
        if (!d.has_value()) {
            ++skipped;
            if (verbose) {
                std::fprintf(stderr, "  skip %s: %s\n", table.id.c_str(),
                             d.error().to_string().c_str());
            }
            continue;
        }
        if (!d->changed()) {
            continue;
        }
        ++changed_count;
        auto const *scal = def->find_scaling(table.scaling);
        rows.push_back({table.id, d->total_cells, d->cells_changed, d->max_abs_delta,
                        d->mean_abs_delta, scal != nullptr ? scal->unit : std::string{}});
    }

    std::printf("\nTables compared: %zu  changed: %zu  skipped: %zu\n",
                def->tables().size(), changed_count, skipped);

    if (rows.empty()) {
        std::printf("\nNo tables differ.\n");
        return 0;
    }

    std::printf("\n%-40s %10s %12s %12s\n", "table", "cells", "max |Δ|", "mean |Δ|");
    for (auto const &r : rows) {
        char cell_buf[32];
        std::snprintf(cell_buf, sizeof(cell_buf), "%zu/%zu", r.changed, r.total);
        std::printf("%-40s %10s %12.3f %12.3f", r.id.c_str(), cell_buf, r.max, r.mean);
        if (!r.unit.empty()) {
            std::printf(" %s", r.unit.c_str());
        }
        std::printf("\n");
    }
    return 0;
}

int cmd_rom_info(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::filesystem::path> rom_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--def") {
            if (i + 1 >= argc) {
                std::fputs("rom-info: --def requires a path\n", stderr);
                return 2;
            }
            def_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!rom_path.has_value()) {
            rom_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "rom-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!rom_path.has_value()) {
        std::fputs("rom-info: missing ROM path\n", stderr);
        std::fputs("Usage: subuwutuner-cli rom-info [--def <pack.toml>] <FILE>\n", stderr);
        return 2;
    }

    auto const rom = st::Rom::from_file(*rom_path);
    if (!rom.has_value()) {
        std::fprintf(stderr, "rom-info: %s\n", rom.error().to_string().c_str());
        return 1;
    }
    print_rom_summary(*rom_path, *rom);

    if (def_path.has_value()) {
        auto const def = st::Definition::from_file(*def_path);
        if (!def.has_value()) {
            std::fprintf(stderr, "rom-info: failed to load definition: %s\n",
                         def.error().to_string().c_str());
            return 1;
        }
        auto const validity = def->validate();
        if (!validity.has_value()) {
            std::fprintf(stderr, "rom-info: definition has issues:\n%s\n",
                         validity.error().message().data());
            // Continue — show what we can. The user explicitly asked for this pack.
        }
        print_def_summary(*def, *rom);
    }

    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc <= 1) {
        print_usage();
        return 0;
    }

    if (arg_matches(argv[1], "-h", "--help")) {
        print_usage();
        return 0;
    }
    if (arg_matches(argv[1], "-V", "--version")) {
        print_version();
        return 0;
    }

    std::string_view const cmd{argv[1]};
    if (cmd == "rom-info") {
        return cmd_rom_info(argc - 2, argv + 2);
    }
    if (cmd == "dump-axis") {
        return cmd_dump_axis(argc - 2, argv + 2);
    }
    if (cmd == "dump-table") {
        return cmd_dump_table(argc - 2, argv + 2);
    }
    if (cmd == "rom-diff") {
        return cmd_rom_diff(argc - 2, argv + 2);
    }
    if (cmd == "table-edit") {
        return cmd_table_edit(argc - 2, argv + 2);
    }
    if (cmd == "project-new") {
        return cmd_project_new(argc - 2, argv + 2);
    }
    if (cmd == "project-info") {
        return cmd_project_info(argc - 2, argv + 2);
    }
    if (cmd == "project-edit") {
        return cmd_project_edit(argc - 2, argv + 2);
    }

    std::fprintf(stderr, "subuwutuner-cli: unknown argument: %s\n", argv[1]);
    std::fprintf(stderr, "Try 'subuwutuner-cli --help'.\n");
    return 2;
}
