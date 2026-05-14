// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/autotune.hpp"
#include "st/can.hpp"
#include "st/core/version.hpp"
#include "st/dbc.hpp"
#include "st/defs.hpp"
#include "st/discover.hpp"
#include "st/ecu/ssm.hpp"
#include "st/edit.hpp"
#include "st/flash.hpp"
#include "st/log.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"
#include "st/transport/mock.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <numeric>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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
    "    dump-axis --def <pack.toml> --axis <id> [--csv] <FILE>\n"
    "                            Read the named axis from the ROM via the pack and\n"
    "                            print its scaled values, one per line.\n"
    "    dump-table --def <pack.toml> --table <id> [--csv] <FILE>\n"
    "                            Read the named table from the ROM via the pack and\n"
    "                            print it as a labeled grid (or CSV with --csv).\n"
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
    "                            update project.toml. Same OPs as table-edit.\n"
    "    pack-info <DEF>         Print metadata + counts for a definition pack.\n"
    "    table-list <DEF> [--category C] [--emissions] [--safety-critical]\n"
    "                            List tables in a pack with optional filters.\n"
    "    project-undo <dir>      Walk back one edit in the project's history.\n"
    "    project-redo <dir>      Walk forward one edit in the project's history.\n"
    "    log --def <pack> --pid <id[,id...]> --trace <file> [--output <csv>]\n"
    "                            Replay an SSM-response trace file through a mock\n"
    "                            transport and write a CSV datalog. Trace format:\n"
    "                            one response frame per line as whitespace-separated\n"
    "                            hex bytes; '#' starts a comment. Without --output,\n"
    "                            the CSV is written to stdout.\n"
    "    can-replay <FILE.asc>   Load a Vector .asc CAN trace and print per-id\n"
    "                            statistics (count, rate, dlc, first byte mode).\n"
    "    can-diff <A.asc> <B.asc>\n"
    "                            Compare two .asc captures. Reports ids present only\n"
    "                            in A, only in B, and per-id frame-count deltas.\n"
    "    can-discover --from <FILE.asc> [--baseline <secs>] [--bus <0..3>]\n"
    "                 [--output <session.cdb>]\n"
    "                            Run offline CAN reverse-engineering discovery over\n"
    "                            a captured trace. Frames before `baseline` seconds\n"
    "                            (default 10) build the BaselineModel; later frames\n"
    "                            drive ChangeDetector. Writes a .cdb bundle to stdout\n"
    "                            unless --output is given. Events are unlabeled —\n"
    "                            edit the .cdb to add descriptions before exporting.\n"
    "    can-export-dbc <session.cdb> [--output <draft.dbc>]\n"
    "                            Emit a draft DBC from a discovery bundle. Each id\n"
    "                            becomes a BO_; each labeled Change event becomes a\n"
    "                            SG_ at identity scaling. Refine the DBC by hand.\n"
    "    can-decode --dbc <FILE.dbc> <FILE.asc> [--output <csv>]\n"
    "                            Decode a .asc capture against a DBC. Writes long-\n"
    "                            format CSV (timestamp_ns,bus,can_id,signal,value,\n"
    "                            unit) — one row per (frame, signal) pair. Frames\n"
    "                            whose id is not in the DBC are skipped.\n"
    "    flash-plan-info <FILE.toml>\n"
    "                            Load a flash plan TOML and print its summary —\n"
    "                            session, options, and the address/length of each\n"
    "                            sector write. Hardware-free; touches no transport.\n"
    "    flash-delta <SOURCE.bin> <TARGET.bin> [--sector-size <N>]\n"
    "                [--base-address <addr>] [--output <plan.toml>]\n"
    "                            Diff two ROMs of equal size; for every sector-aligned\n"
    "                            region that differs, emit a flash plan whose [[write]]\n"
    "                            entries cover those sectors with TARGET's bytes. The\n"
    "                            plan is hand-editable before execution.\n"
    "    flash-resume <ORIGINAL.plan.toml> <JOURNAL.manifest.toml>\n"
    "                [--output <resumed.plan.toml>]\n"
    "                            Given the plan from a partial-flash attempt and the\n"
    "                            manifest journal it left behind, emit a plan covering\n"
    "                            only the sectors that didn't complete. Refuses if the\n"
    "                            plan was modified between attempts (data CRC32 of any\n"
    "                            done sector differs).\n"
    "    flash-apply --plan <FILE.toml> --trace <FILE.uds>\n"
    "                [--journal <FILE.toml>] [--manifest <FILE.toml>]\n"
    "                            Run a flash plan against a MockTransport-replayed UDS\n"
    "                            trace. The trace is text with one '> req hex' /\n"
    "                            '< resp hex' pair per exchange; '#' starts a comment.\n"
    "                            Hardware-free smoke for the Flasher orchestrator;\n"
    "                            prints the FlashReport summary; if --manifest is set,\n"
    "                            writes a Manifest of the run; if --journal is set,\n"
    "                            sets FlashPlan.journal_path for incremental writes.\n"
    "    rom-pull --addr <hex> --size <hex> --trace <FILE.uds> --output <FILE.bin>\n"
    "             [--max-chunk <hex>]\n"
    "                            Read N bytes of ECU memory via Flasher::read_full_rom\n"
    "                            against a MockTransport-replayed UDS trace, written\n"
    "                            as a raw binary file. Trace format matches flash-apply\n"
    "                            ('> req' / '< resp' pairs). Default --max-chunk=0x100.\n"
    "    flash-trace --plan <FILE.toml> --output <FILE.uds>\n"
    "                            Emit a guaranteed-success UDS trace for the given plan,\n"
    "                            walking the same sequence Flasher::execute would and\n"
    "                            pairing every request with a canned positive response.\n"
    "                            Useful as a regression fixture or as input to\n"
    "                            'flash-apply --trace ...' for hardware-free validation.\n"
    "    autotune maf --log <CSV> (--axis <v,v,…> | --axis-file <path>)\n"
    "                 (--current <gs,gs,…> | --current-file <path>)\n"
    "                 [--gain 0.5] [--max-delta 8%] [--min-samples-per-cell 50]\n"
    "                 [--require-open-loop] [--no-smooth] [--strict-lint]\n"
    "                            Propose a MAF-scaling correction per docs/12. Reads a\n"
    "                            column-headered CSV log (required cols: maf_voltage,\n"
    "                            actual_afr, commanded_afr, rpm, throttle_pct, coolant_c,\n"
    "                            iat_c; optional: time_ms, closed_loop, knock, limp_mode),\n"
    "                            applies the configured data-quality gates, buckets each\n"
    "                            sample to the nearest axis cell, runs the docs/12\n"
    "                            engine-safety lint (non-monotonic + step discontinuity),\n"
    "                            and prints the per-cell diff. --strict-lint exits 3 on\n"
    "                            any violation. Hardware-free; no flashing.\n"
    "    autotune knock-pull --log <CSV>\n"
    "                        (--rpm-axis <r,r,…>  | --rpm-axis-file <path>)\n"
    "                        (--load-axis <l,l,…> | --load-axis-file <path>)\n"
    "                        (--current-timing <d,d,…> | --current-timing-file <path>)\n"
    "                        [--trigger-degrees 1.5] [--pull-step-degrees 0.75]\n"
    "                        [--min-samples-per-cell 30]\n"
    "                        [--max-neighbor-step-degrees 3.0] [--strict-lint]\n"
    "                        [--enable-add-back] [--add-back-step-degrees 0.5]\n"
    "                        [--add-back-min-clean-samples 50]\n"
    "                        [--add-back-clean-threshold-degrees 0.05]\n"
    "                            Propose ignition-timing pull on cells with sustained\n"
    "                            knock per docs/12 §\"Knock-based ignition pull\". CSV\n"
    "                            cols: rpm, load, feedback_knock, coolant_c, iat_c;\n"
    "                            optional: limp_mode. --current-timing is flat row-major\n"
    "                            (rows × cols = load × rpm). Monotonic-subtract: cells\n"
    "                            never gain timing in the pull pass. --enable-add-back\n"
    "                            opts into the docs/12 follow-up pass that adds a small\n"
    "                            amount of timing BACK in cells whose mean feedback\n"
    "                            knock was clean over enough samples; default off per\n"
    "                            spec. Runs the docs/12 neighbor-smoothness lint on the\n"
    "                            final proposed surface; --strict-lint exits 3 on any\n"
    "                            violation. Hardware-free.\n";

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
    bool                                 csv = false;

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
        } else if (a == "--csv") {
            csv = true;
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
            "Usage: subuwutuner-cli dump-axis --def <pack.toml> --axis <id> [--csv] <FILE>\n",
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

    if (csv) {
        for (std::size_t i = 0; i < values->size(); ++i) {
            if (i > 0) std::printf(",");
            std::printf("%.*f", precision, (*values)[i]);
        }
        std::printf("\n");
    } else {
        std::printf("# %s  (%zu values%s%s)\n", axis->id.c_str(), values->size(),
                    unit.empty() ? "" : ", unit=", unit.c_str());
        for (auto const v : *values) {
            std::printf("%.*f\n", precision, v);
        }
    }
    return 0;
}

int cmd_dump_table(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string>           table_id;
    std::optional<std::filesystem::path> rom_path;
    bool                                 csv = false;

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
        } else if (a == "--csv") {
            csv = true;
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

    auto const &xs = td->axis_x;
    auto const &ys = td->axis_y;
    auto const &zs = td->axis_z;

    auto const print_slice_csv = [&](std::vector<std::vector<double>> const &grid) {
        // Scalars print as the single value on its own line — no axis
        // header row, no leading comma.
        if (xs.empty() && ys.empty() && grid.size() == 1 && grid[0].size() == 1) {
            std::printf("%.*f\n", precision, grid[0][0]);
            return;
        }
        for (auto const x : xs) std::printf(",%.*f", precision, x);
        std::printf("\n");
        for (std::size_t r = 0; r < grid.size(); ++r) {
            if (!ys.empty()) std::printf("%.*f", precision, ys[r]);
            for (auto const v : grid[r]) std::printf(",%.*f", precision, v);
            std::printf("\n");
        }
    };

    constexpr int kColWidth = 10;
    auto const    print_slice_pretty =
        [&](std::vector<std::vector<double>> const &grid) {
        if (xs.empty() && ys.empty() && grid.size() == 1 && grid[0].size() == 1) {
            std::printf("%.*f\n", precision, grid[0][0]);
            return;
        }
        std::printf("%*s", kColWidth, "");
        for (auto const x : xs) std::printf(" %*.*f", kColWidth - 1, precision, x);
        std::printf("\n");
        for (std::size_t r = 0; r < grid.size(); ++r) {
            if (!ys.empty()) std::printf("%*.*f", kColWidth, precision, ys[r]);
            else             std::printf("%*s", kColWidth, "");
            for (auto const v : grid[r])
                std::printf(" %*.*f", kColWidth - 1, precision, v);
            std::printf("\n");
        }
    };

    if (csv) {
        if (table->dimensions == 3) {
            for (std::size_t z = 0; z < td->slices.size(); ++z) {
                std::printf("# z=%.*f\n", precision, zs.empty() ? 0.0 : zs[z]);
                print_slice_csv(td->slices[z]);
            }
        } else {
            print_slice_csv(td->values);
        }
        return 0;
    }

    std::printf("# %s  (%dD", table->id.c_str(), table->dimensions);
    if (!table->name.empty()) std::printf(", %s", table->name.c_str());
    if (!unit.empty())        std::printf(", unit=%s", unit.c_str());
    std::printf(")\n");

    if (table->dimensions == 3) {
        for (std::size_t z = 0; z < td->slices.size(); ++z) {
            std::printf("\n--- z = %.*f ---\n",
                        precision, zs.empty() ? 0.0 : zs[z]);
            print_slice_pretty(td->slices[z]);
        }
    } else {
        print_slice_pretty(td->values);
    }
    return 0;
}

int cmd_project_step(int argc, char *argv[], bool forward) {
    char const *cmd_name = forward ? "project-redo" : "project-undo";
    if (argc < 1) {
        std::fprintf(stderr, "%s: missing project directory\n", cmd_name);
        std::fprintf(stderr, "Usage: subuwutuner-cli %s <dir>\n", cmd_name);
        return 2;
    }
    std::filesystem::path const dir{argv[0]};

    auto proj = st::Project::open(dir);
    if (!proj.has_value()) {
        std::fprintf(stderr, "%s: %s\n", cmd_name, proj.error().to_string().c_str());
        return 1;
    }

    st::edit::Edit const *edit_record =
        forward ? proj->history().redo() : proj->history().undo();
    if (edit_record == nullptr) {
        std::fprintf(stderr, "%s: no edit to %s\n", cmd_name,
                     forward ? "redo" : "undo");
        return 1;
    }

    auto const *table = proj->definition().find_table(edit_record->table_id);
    if (table == nullptr) {
        std::fprintf(stderr,
                     "%s: edit references table '%s' which is not in the current pack\n",
                     cmd_name, edit_record->table_id.c_str());
        return 1;
    }

    auto td = proj->definition().read_table_values(proj->working_rom(), *table);
    if (!td.has_value()) {
        std::fprintf(stderr, "%s: %s\n", cmd_name, td.error().to_string().c_str());
        return 1;
    }

    // Undo restores `before`; redo restores `after`.
    auto const &snap = forward ? edit_record->after : edit_record->before;
    if (auto s = st::edit::restore(*td, snap); !s.has_value()) {
        std::fprintf(stderr, "%s: %s\n", cmd_name, s.error().to_string().c_str());
        return 1;
    }

    auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
    if (!wb.has_value()) {
        std::fprintf(stderr, "%s: writeback: %s\n", cmd_name,
                     wb.error().to_string().c_str());
        return 1;
    }

    if (auto s = proj->save_working_rom(); !s.has_value()) {
        std::fprintf(stderr, "%s: save: %s\n", cmd_name, s.error().to_string().c_str());
        return 1;
    }

    std::printf("%s applied edit: %s on %s\n",
                forward ? "Redo" : "Undo",
                edit_record->description.c_str(),
                edit_record->table_id.c_str());
    std::printf("Working CRC32: 0x%08X\n", proj->working_rom().crc32());
    std::printf("History cursor: %zu / %zu\n", proj->history().cursor(),
                proj->history().size());
    return 0;
}

int cmd_pack_info(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("pack-info: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli pack-info <DEF>\n", stderr);
        return 2;
    }
    std::filesystem::path const path{argv[0]};
    auto const                  def = st::Definition::from_file(path);
    if (!def.has_value()) {
        std::fprintf(stderr, "pack-info: %s\n", def.error().to_string().c_str());
        return 1;
    }
    auto const &pack = def->pack();
    std::printf("Path:           %s\n", path.string().c_str());
    std::printf("Schema version: %d\n", pack.schema_version);
    std::printf("Id:             %s\n", pack.id.c_str());
    if (!pack.display_name.empty()) {
        std::printf("Display name:   %s\n", pack.display_name.c_str());
    }
    if (!pack.platform.empty()) {
        std::printf("Platform:       %s\n", pack.platform.c_str());
    }
    if (!pack.transmission.empty()) {
        std::printf("Transmission:   %s\n", pack.transmission.c_str());
    }
    if (!pack.years.empty()) {
        std::printf("Years:         ");
        for (int y : pack.years) std::printf(" %d", y);
        std::printf("\n");
    }
    std::printf("Endianness:     %s\n", pack.endianness.c_str());
    if (pack.rom_size_bytes != 0) {
        std::printf("Expected ROM:   %zu bytes (%.2f KiB)\n", pack.rom_size_bytes,
                    static_cast<double>(pack.rom_size_bytes) / 1024.0);
    }
    if (!pack.license.empty()) {
        std::printf("License:        %s\n", pack.license.c_str());
    }
    if (pack.extends.has_value()) {
        std::printf("Extends:        %s\n", pack.extends->c_str());
    }
    std::printf("\n");
    std::printf("Identifications: %zu\n", def->identifications().size());
    for (auto const &id : def->identifications()) {
        std::printf("  - %s  (CID '%s' @ 0x%08zX)\n", id.name.c_str(),
                    id.cid_match.c_str(), id.cid_address);
    }
    std::printf("Axes:            %zu\n", def->axes().size());
    std::printf("Scalings:        %zu\n", def->scalings().size());
    std::printf("Tables:          %zu\n", def->tables().size());
    std::printf("PIDs:            %zu\n", def->pids().size());
    std::printf("Switches:        %zu\n", def->switches().size());

    // Quick validate so users see issues without running rom-info.
    auto const validity = def->validate();
    if (!validity.has_value()) {
        std::printf("\nValidation: ! %s\n", validity.error().message().data());
        return 1;
    }
    std::printf("\nValidation: OK\n");
    return 0;
}

int cmd_table_list(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string>           category_filter;
    bool                                 emissions_only       = false;
    bool                                 safety_critical_only = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "table-list: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--category") {
            if (auto const *v = require("--category"); v) category_filter = std::string{v};
            else return 2;
        } else if (a == "--emissions") {
            emissions_only = true;
        } else if (a == "--safety-critical") {
            safety_critical_only = true;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "table-list: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!def_path.has_value()) {
            def_path = std::filesystem::path{a};
        } else {
            std::fprintf(stderr, "table-list: extra argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value()) {
        std::fputs("table-list: missing path\n", stderr);
        std::fputs("Usage: subuwutuner-cli table-list <DEF> [--category C] "
                   "[--emissions] [--safety-critical]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(*def_path);
    if (!def.has_value()) {
        std::fprintf(stderr, "table-list: %s\n", def.error().to_string().c_str());
        return 1;
    }

    std::size_t matched = 0;
    std::printf("%-3s %-10s %-32s %-18s %s\n", "D", "address", "id", "category", "name");
    for (auto const &t : def->tables()) {
        if (category_filter.has_value() && t.category != *category_filter) continue;
        if (emissions_only && !t.emissions_relevant) continue;
        if (safety_critical_only && !t.engine_safety_critical) continue;
        char flags[8];
        std::snprintf(flags, sizeof(flags), "%s%s",
                      t.emissions_relevant ? "E" : "-",
                      t.engine_safety_critical ? "S" : "-");
        (void) flags; // currently not printed; reserved for a future --flags column
        std::printf("%dD  0x%08zX %-32s %-18s %s\n", t.dimensions, t.address,
                    t.id.c_str(), t.category.c_str(), t.name.c_str());
        ++matched;
    }
    std::printf("\n%zu tables shown (of %zu in pack).\n", matched, def->tables().size());
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

    // Capture before-snapshot (for the history record).
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", before.error().to_string().c_str());
        return 1;
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

    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        std::fprintf(stderr, "project-edit: %s\n", after.error().to_string().c_str());
        return 1;
    }

    auto wb = proj->definition().write_table_values(proj->working_rom(), *table, *td);
    if (!wb.has_value()) {
        std::fprintf(stderr, "project-edit: writeback: %s\n", wb.error().to_string().c_str());
        return 1;
    }

    // Build a short human-readable description for the history record.
    std::string desc{*op};
    if (op_needs_value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " %g", *value);
        desc.append(buf);
    }
    proj->history().record({table->id, std::move(*before), std::move(*after),
                            std::move(desc)});

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

// Parse a trace file of SSM response frames. Format: one frame per line,
// hex bytes separated by whitespace; '#' starts a comment that runs to
// end-of-line; blank lines are skipped. Returns the parsed frames in file
// order, or an error string in `err` and false on parse failure.
bool load_trace_file(std::filesystem::path const &path,
                     std::vector<std::vector<std::uint8_t>> &out_frames,
                     std::string &err) {
    std::ifstream in{path};
    if (!in) {
        err = "log: cannot open trace file: " + path.string();
        return false;
    }
    std::string line;
    int         line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (auto const hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream iss{line};
        std::vector<std::uint8_t> frame;
        std::string               tok;
        while (iss >> tok) {
            unsigned             value = 0;
            auto const           first = tok.data();
            auto const           last  = tok.data() + tok.size();
            auto const           res   = std::from_chars(first, last, value, 16);
            if (res.ec != std::errc{} || res.ptr != last || value > 0xFFU) {
                err = "log: bad hex byte '" + tok + "' on line "
                      + std::to_string(line_no);
                return false;
            }
            frame.push_back(static_cast<std::uint8_t>(value));
        }
        if (!frame.empty()) {
            out_frames.push_back(std::move(frame));
        }
    }
    if (out_frames.empty()) {
        err = "log: trace file is empty: " + path.string();
        return false;
    }
    return true;
}

// Split a comma-separated list into trimmed, non-empty tokens.
std::vector<std::string> split_csv_list(std::string_view s) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (start <= s.size()) {
        auto const end = s.find(',', start);
        auto const piece =
            s.substr(start,
                     end == std::string_view::npos ? std::string_view::npos
                                                   : end - start);
        // Trim whitespace.
        std::size_t a = 0;
        while (a < piece.size() && std::isspace(static_cast<unsigned char>(piece[a]))) ++a;
        std::size_t b = piece.size();
        while (b > a && std::isspace(static_cast<unsigned char>(piece[b - 1]))) --b;
        if (b > a) {
            out.emplace_back(piece.substr(a, b - a));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

int cmd_log(int argc, char *argv[]) {
    std::optional<std::filesystem::path> def_path;
    std::optional<std::string>           pid_list_arg;
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "log: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--def") {
            if (auto const *v = require_arg("--def"); v) def_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--pid") {
            if (auto const *v = require_arg("--pid"); v) pid_list_arg = std::string{v};
            else return 2;
        } else if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v) trace_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) output_path = std::filesystem::path{v};
            else return 2;
        } else {
            std::fprintf(stderr, "log: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!def_path.has_value() || !pid_list_arg.has_value() || !trace_path.has_value()) {
        std::fputs("log: missing required arguments\n", stderr);
        std::fputs("Usage: subuwutuner-cli log --def <pack> --pid <id[,id...]>\n"
                   "       --trace <file> [--output <csv>]\n",
                   stderr);
        return 2;
    }

    auto const def = st::Definition::from_file(*def_path);
    if (!def.has_value()) {
        std::fprintf(stderr, "log: %s\n", def.error().to_string().c_str());
        return 1;
    }

    // Resolve each named PID to a LogChannel.
    auto const pid_ids = split_csv_list(*pid_list_arg);
    if (pid_ids.empty()) {
        std::fputs("log: --pid list is empty\n", stderr);
        return 2;
    }
    std::vector<st::log::LogChannel> channels;
    channels.reserve(pid_ids.size());
    for (auto const &id : pid_ids) {
        auto const *pid = def->find_pid(id);
        if (pid == nullptr) {
            std::fprintf(stderr, "log: pid '%s' not found in pack\n", id.c_str());
            return 1;
        }
        auto const *scaling = def->find_scaling(pid->scaling);
        channels.push_back(st::log::LogChannel{
            pid->id,
            static_cast<std::uint32_t>(pid->ssm_address),
            pid->data_type,
            scaling != nullptr ? std::optional<st::Scaling>{*scaling}
                               : std::nullopt,
        });
    }

    // Load the trace and seed a MockTransport with one response per cycle.
    std::vector<std::vector<std::uint8_t>> frames;
    std::string                            err;
    if (!load_trace_file(*trace_path, frames, err)) {
        std::fputs(err.c_str(), stderr); std::fputc('\n', stderr);
        return 1;
    }

    // Build the exact SSM A8 request the session will emit, since
    // MockTransport matches request bytes exactly.
    std::vector<std::uint32_t> read_addrs;
    for (auto const &ch : channels) {
        auto const bytes = st::byte_size(ch.data_type);
        for (std::size_t i = 0; i < bytes; ++i) {
            read_addrs.push_back(ch.address + static_cast<std::uint32_t>(i));
        }
    }
    auto const req = st::ecu::ssm::build_a8_request(read_addrs);
    if (!req.has_value()) {
        std::fprintf(stderr, "log: cannot build SSM request: %s\n",
                     req.error().to_string().c_str());
        return 1;
    }

    st::transport::MockTransport mock;
    if (auto s = mock.open({}); !s.has_value()) {
        std::fprintf(stderr, "log: mock transport open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }
    for (auto const &resp : frames) {
        mock.expect_send_recv(*req, resp);
    }

    // Resolve where the CSV goes.
    std::ofstream  file_out;
    std::ostream  *out_stream = &std::cout;
    if (output_path.has_value()) {
        file_out.open(*output_path);
        if (!file_out) {
            std::fprintf(stderr, "log: cannot open output: %s\n",
                         output_path->string().c_str());
            return 1;
        }
        out_stream = &file_out;
    }

    st::log::CsvSink sink{*out_stream, channels};
    sink.write_header();

    // Drive the session and drain. Once cycles_completed reaches the
    // trace size, every queued response has been consumed.
    st::log::LogSession session{mock, channels, /*ring_capacity=*/1024};
    if (auto s = session.start(); !s.has_value()) {
        std::fprintf(stderr, "log: %s\n", s.error().to_string().c_str());
        return 1;
    }

    std::int64_t        ts = 0;
    std::vector<double> values(channels.size(), 0.0);
    std::uint64_t const target = frames.size();
    auto const          deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (session.cycles_completed() < target) {
        while (session.stream().try_pop(ts, values)) {
            sink.write_row(ts, values);
        }
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    // Final drain after the producer is done.
    while (session.stream().try_pop(ts, values)) {
        sink.write_row(ts, values);
    }
    session.stop();
    out_stream->flush();

    std::fprintf(stderr,
                 "log: cycles=%llu  drops=%llu  io_errors=%llu  channels=%zu\n",
                 session.cycles_completed(),
                 session.stream().dropped_count(),
                 session.io_errors(),
                 channels.size());
    return 0;
}

// Parse a fraction-or-percent string: "8%" → 0.08, "0.08" → 0.08. The
// trailing '%' is optional; whitespace around the value is tolerated.
// Returns nullopt on malformed input or non-finite results.
std::optional<double> parse_fraction_or_percent(std::string_view raw) {
    std::string s{raw};
    auto trim = [](std::string &t) {
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) {
            t.erase(t.begin());
        }
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) {
            t.pop_back();
        }
    };
    trim(s);
    if (s.empty()) {
        return std::nullopt;
    }
    bool const percent = (s.back() == '%');
    if (percent) {
        s.pop_back();
        trim(s);
    }
    char        *end = nullptr;
    double const v   = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0'
        || !std::isfinite(v)) {
        return std::nullopt;
    }
    return percent ? v / 100.0 : v;
}

// Print the canonical "Lint:" section used by every `autotune *`
// subcommand. Returns 3 when `strict` is true AND any violation is
// present so the caller can use it as its exit code; returns 0
// otherwise. Centralising this keeps the cosmetic formatting and the
// strict-lint exit code in one place as more autotune commands land.
int print_lint_section(std::span<st::autotune::LintViolation const> violations,
                       bool strict) {
    std::printf("\nLint:\n");
    if (violations.empty()) {
        std::printf("  No violations.\n");
        return 0;
    }
    std::printf("  %zu violation%s:\n",
                violations.size(),
                violations.size() == 1 ? "" : "s");
    for (auto const &v : violations) {
        std::printf("  - [%s] %s\n",
                    st::autotune::lint_kind_name(v.kind),
                    v.message.c_str());
    }
    return strict ? 3 : 0;
}

// Parse a plain decimal: strict strtod with no '%' acceptance. Used
// for arguments that name a physical unit (e.g. degrees) where a
// trailing '%' would be a user error to silently divide-by-100.
std::optional<double> parse_decimal(std::string_view raw) {
    std::string s{raw};
    auto trim = [](std::string &t) {
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) {
            t.erase(t.begin());
        }
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) {
            t.pop_back();
        }
    };
    trim(s);
    if (s.empty()) {
        return std::nullopt;
    }
    char        *end = nullptr;
    double const v   = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0'
        || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

// Read a flat list of decimals from a text file. Tokens may be
// separated by commas, whitespace (including newlines), or any mix —
// the reader walks character-by-character and parses any contiguous
// numeric token. Lines starting with '#' are skipped (after leading
// whitespace). Empty file → empty vector. Used for `--axis-file`,
// `--current-file`, etc. so real-world tables (a 32-breakpoint MAF
// axis isn't fun to inline on the command line) can live in a file.
std::optional<std::vector<double>> read_decimal_list_file(
    std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string const  text = buf.str();
    std::vector<double> out;

    std::size_t i = 0;
    while (i < text.size()) {
        // Skip whitespace and commas.
        while (i < text.size()
               && (std::isspace(static_cast<unsigned char>(text[i]))
                   || text[i] == ',')) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        if (text[i] == '#') {
            // Comment to end of line.
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        // Parse a numeric token: read up to the next separator.
        std::size_t const start = i;
        while (i < text.size()
               && !std::isspace(static_cast<unsigned char>(text[i]))
               && text[i] != ',' && text[i] != '#') {
            ++i;
        }
        std::string const token{text.data() + start, i - start};
        char             *end = nullptr;
        double const      v   = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || end == nullptr || *end != '\0'
            || !std::isfinite(v)) {
            return std::nullopt;
        }
        out.push_back(v);
    }
    return out;
}

// Parse a comma-separated list of doubles into a flat vector. Returns
// nullopt if any field fails to parse; an empty input gives an empty
// vector (the caller decides if that's an error).
std::optional<std::vector<double>> parse_double_list(std::string_view s) {
    std::vector<double> out;
    auto const          fields = split_csv_list(s);
    out.reserve(fields.size());
    for (auto const &f : fields) {
        char        *end = nullptr;
        double const v   = std::strtod(f.c_str(), &end);
        if (end == f.c_str() || end == nullptr || *end != '\0'
            || !std::isfinite(v)) {
            return std::nullopt;
        }
        out.push_back(v);
    }
    return out;
}

int cmd_autotune_maf(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string>           axis_arg;
    std::optional<std::filesystem::path> axis_file;
    std::optional<std::string>           current_arg;
    std::optional<std::filesystem::path> current_file;
    std::optional<double>                gain;
    std::optional<double>                max_delta_pct;
    std::optional<std::size_t>           min_samples;
    bool                                 require_open_loop = false;
    bool                                 skip_smooth       = false;
    bool                                 strict_lint       = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "autotune maf: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--log") {
            if (auto const *v = require_arg("--log"); v) {
                log_path = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--axis") {
            if (auto const *v = require_arg("--axis"); v) {
                axis_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--axis-file") {
            if (auto const *v = require_arg("--axis-file"); v) {
                axis_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--current") {
            if (auto const *v = require_arg("--current"); v) {
                current_arg = std::string{v};
            } else {
                return 2;
            }
        } else if (a == "--current-file") {
            if (auto const *v = require_arg("--current-file"); v) {
                current_file = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a == "--gain") {
            if (auto const *v = require_arg("--gain"); v) {
                auto const parsed = parse_fraction_or_percent(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune maf: --gain must be a number "
                                 "(got '%s')\n", v);
                    return 2;
                }
                gain = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--max-delta" || a == "--max-delta-pct") {
            if (auto const *v = require_arg("--max-delta"); v) {
                auto const parsed = parse_fraction_or_percent(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune maf: --max-delta must be a "
                                 "number (got '%s')\n", v);
                    return 2;
                }
                max_delta_pct = *parsed;
            } else {
                return 2;
            }
        } else if (a == "--min-samples-per-cell" || a == "--min-samples") {
            if (auto const *v = require_arg("--min-samples-per-cell"); v) {
                char       *end  = nullptr;
                long long const n = std::strtoll(v, &end, 10);
                if (end == v || end == nullptr || *end != '\0' || n < 0) {
                    std::fprintf(stderr,
                                 "autotune maf: --min-samples-per-cell "
                                 "must be a non-negative integer (got '%s')\n",
                                 v);
                    return 2;
                }
                min_samples = static_cast<std::size_t>(n);
            } else {
                return 2;
            }
        } else if (a == "--require-open-loop") {
            require_open_loop = true;
        } else if (a == "--no-smooth") {
            skip_smooth = true;
        } else if (a == "--strict-lint") {
            strict_lint = true;
        } else {
            std::fprintf(stderr, "autotune maf: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    bool const axis_given    = axis_arg.has_value()    || axis_file.has_value();
    bool const current_given = current_arg.has_value() || current_file.has_value();
    if (!log_path.has_value() || !axis_given || !current_given) {
        std::fputs("autotune maf: missing required arguments\n", stderr);
        std::fputs("Usage: subuwutuner-cli autotune maf "
                   "--log <csv> (--axis <v,v,…> | --axis-file <path>)\n"
                   "       (--current <gs,gs,…> | --current-file <path>)\n"
                   "       [--gain 0.5] [--max-delta 8%] "
                   "[--min-samples-per-cell 50]\n"
                   "       [--require-open-loop] [--no-smooth] "
                   "[--strict-lint]\n",
                   stderr);
        return 2;
    }
    if (axis_arg.has_value() && axis_file.has_value()) {
        std::fputs("autotune maf: --axis and --axis-file are mutually exclusive\n",
                   stderr);
        return 2;
    }
    if (current_arg.has_value() && current_file.has_value()) {
        std::fputs("autotune maf: --current and --current-file are mutually "
                   "exclusive\n",
                   stderr);
        return 2;
    }

    std::optional<std::vector<double>> axis;
    if (axis_file.has_value()) {
        axis = read_decimal_list_file(*axis_file);
        if (!axis.has_value()) {
            std::fprintf(stderr,
                         "autotune maf: cannot read --axis-file '%s' "
                         "(missing, unreadable, or contains a non-numeric "
                         "token)\n",
                         axis_file->string().c_str());
            return 1;
        }
    } else {
        axis = parse_double_list(*axis_arg);
    }
    if (!axis.has_value() || axis->empty()) {
        std::fputs("autotune maf: --axis / --axis-file must yield a non-empty "
                   "list of numbers\n", stderr);
        return 2;
    }
    std::optional<std::vector<double>> current;
    if (current_file.has_value()) {
        current = read_decimal_list_file(*current_file);
        if (!current.has_value()) {
            std::fprintf(stderr,
                         "autotune maf: cannot read --current-file '%s' "
                         "(missing, unreadable, or contains a non-numeric "
                         "token)\n",
                         current_file->string().c_str());
            return 1;
        }
    } else {
        current = parse_double_list(*current_arg);
    }
    if (!current.has_value() || current->empty()) {
        std::fputs("autotune maf: --current / --current-file must yield a "
                   "non-empty list of numbers\n", stderr);
        return 2;
    }
    if (axis->size() != current->size()) {
        std::fprintf(stderr,
                     "autotune maf: axis (%zu) and current (%zu) must have "
                     "the same length\n",
                     axis->size(), current->size());
        return 2;
    }

    std::ifstream in{*log_path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "autotune maf: cannot open --log '%s'\n",
                     log_path->string().c_str());
        return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto const samples = st::autotune::read_maf_samples_csv(buf.str());
    if (!samples.has_value()) {
        std::fprintf(stderr, "autotune maf: %s\n",
                     samples.error().to_string().c_str());
        return 1;
    }

    st::autotune::MafTuneOptions opts;
    if (gain.has_value())          { opts.gain                 = *gain; }
    if (max_delta_pct.has_value()) { opts.max_delta_pct        = *max_delta_pct; }
    if (min_samples.has_value())   { opts.min_samples_per_cell = *min_samples; }
    opts.require_open_loop = require_open_loop;

    auto const result =
        st::autotune::tune_maf(*axis, *current, *samples, opts);
    if (!result.has_value()) {
        std::fprintf(stderr, "autotune maf: %s\n",
                     result.error().to_string().c_str());
        return 1;
    }

    auto const &final_result =
        skip_smooth
            ? *result
            : st::autotune::smooth_proposals(*result, opts.max_delta_pct);

    // Summary header — mirrors docs/12 §"Output".
    std::printf("Loaded %zu samples from %s\n",
                final_result.total_samples,
                log_path->string().c_str());
    double const retained_pct =
        final_result.total_samples == 0
            ? 0.0
            : 100.0
                  * static_cast<double>(final_result.samples_after_gates)
                  / static_cast<double>(final_result.total_samples);
    std::printf("After quality gates: %zu samples (%.1f%% retained)\n\n",
                final_result.samples_after_gates,
                retained_pct);

    // Aggregate stats. After smoothing, a cell can have a non-zero
    // post-smooth delta from neighbor pull even when its own
    // samples_used < min_samples_per_cell — so `modified` counts every
    // cell that moved (regardless of why), and `underpowered` reports
    // separately how many cells lacked direct data of their own.
    constexpr double kModifiedEpsilon = 1e-9;
    std::size_t modified         = 0;
    std::size_t underpowered     = 0;
    double      sum_delta_pct    = 0.0;
    double      max_delta_signed = 0.0;
    double      min_delta_signed = 0.0;
    std::size_t max_cell_idx     = 0;
    std::size_t min_cell_idx     = 0;
    for (auto const &c : final_result.cells) {
        if (c.samples_used < opts.min_samples_per_cell) {
            ++underpowered;
        }
        double const delta_pct = c.current_value == 0.0
                                     ? 0.0
                                     : (c.proposed_value / c.current_value)
                                         - 1.0;
        if (std::abs(delta_pct) > kModifiedEpsilon) {
            ++modified;
            sum_delta_pct += delta_pct;
        }
        if (delta_pct > max_delta_signed) {
            max_delta_signed = delta_pct;
            max_cell_idx     = c.cell_index;
        }
        if (delta_pct < min_delta_signed) {
            min_delta_signed = delta_pct;
            min_cell_idx     = c.cell_index;
        }
    }

    std::printf("MAF scaling proposal:\n");
    std::printf("  Cells modified:        %zu / %zu\n",
                modified, final_result.cells.size());
    std::printf("  Underpowered cells:    %zu (<%zu direct samples; any "
                "delta is from neighbor smoothing)\n",
                underpowered, opts.min_samples_per_cell);
    if (modified > 0) {
        std::printf("  Mean delta:            %+.2f%%\n",
                    100.0 * sum_delta_pct / static_cast<double>(modified));
    }
    if (max_delta_signed > 0.0) {
        auto const &c = final_result.cells[max_cell_idx];
        std::printf("  Max delta:             %+.2f%% at v=%.2f (n=%zu)\n",
                    100.0 * max_delta_signed,
                    (*axis)[max_cell_idx],
                    c.samples_used);
    }
    if (min_delta_signed < 0.0) {
        auto const &c = final_result.cells[min_cell_idx];
        std::printf("  Min delta:             %+.2f%% at v=%.2f (n=%zu)\n",
                    100.0 * min_delta_signed,
                    (*axis)[min_cell_idx],
                    c.samples_used);
    }
    std::printf("\n");

    // Per-cell ledger — small N (the MAF axis has a couple of dozen
    // breakpoints) so we can afford one row per cell.
    std::printf("  v (V) |  current  | proposed  |   delta   |   n   | confidence\n");
    std::printf("  ------+-----------+-----------+-----------+-------+-----------\n");
    for (std::size_t i = 0; i < final_result.cells.size(); ++i) {
        auto const &c = final_result.cells[i];
        double const delta_pct = c.current_value == 0.0
                                     ? 0.0
                                     : (c.proposed_value / c.current_value)
                                         - 1.0;
        std::printf("  %5.2f | %9.4f | %9.4f | %+8.2f%% | %5zu | %6.2f\n",
                    (*axis)[i],
                    c.current_value,
                    c.proposed_value,
                    100.0 * delta_pct,
                    c.samples_used,
                    c.confidence);
    }

    // Engine-safety lint per docs/12 §"Engine-safety linting". Always
    // runs — the proposal isn't auto-applied, so the user reviews the
    // findings and decides. With --strict-lint, exit non-zero when any
    // violation is present so this command can gate a downstream
    // --apply (when that lands in the project-integration slice).
    auto const violations =
        st::autotune::lint_maf_proposal(*axis, *current, final_result);
    return print_lint_section(violations, strict_lint);
}

int cmd_autotune_knock_pull(int argc, char *argv[]) {
    std::optional<std::filesystem::path> log_path;
    std::optional<std::string>           rpm_axis_arg;
    std::optional<std::filesystem::path> rpm_axis_file;
    std::optional<std::string>           load_axis_arg;
    std::optional<std::filesystem::path> load_axis_file;
    std::optional<std::string>           current_arg;
    std::optional<std::filesystem::path> current_file;
    std::optional<double>                trigger_deg;
    std::optional<double>                pull_step_deg;
    std::optional<std::size_t>           min_samples;
    std::optional<double>                max_neighbor_step;
    bool                                 strict_lint        = false;
    bool                                 enable_add_back    = false;
    std::optional<double>                add_back_step;
    std::optional<std::size_t>           add_back_min_clean;
    std::optional<double>                add_back_threshold;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                             "autotune knock-pull: %s requires a value\n",
                             name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--log") {
            if (auto const *v = require_arg("--log"); v) {
                log_path = std::filesystem::path{v};
            } else { return 2; }
        } else if (a == "--rpm-axis") {
            if (auto const *v = require_arg("--rpm-axis"); v) {
                rpm_axis_arg = std::string{v};
            } else { return 2; }
        } else if (a == "--rpm-axis-file") {
            if (auto const *v = require_arg("--rpm-axis-file"); v) {
                rpm_axis_file = std::filesystem::path{v};
            } else { return 2; }
        } else if (a == "--load-axis") {
            if (auto const *v = require_arg("--load-axis"); v) {
                load_axis_arg = std::string{v};
            } else { return 2; }
        } else if (a == "--load-axis-file") {
            if (auto const *v = require_arg("--load-axis-file"); v) {
                load_axis_file = std::filesystem::path{v};
            } else { return 2; }
        } else if (a == "--current-timing" || a == "--current") {
            if (auto const *v = require_arg("--current-timing"); v) {
                current_arg = std::string{v};
            } else { return 2; }
        } else if (a == "--current-timing-file"
                   || a == "--current-file") {
            if (auto const *v = require_arg("--current-timing-file"); v) {
                current_file = std::filesystem::path{v};
            } else { return 2; }
        } else if (a == "--trigger-degrees" || a == "--trigger") {
            if (auto const *v = require_arg("--trigger-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: --trigger-degrees must "
                                 "be a decimal number in degrees (got '%s')\n",
                                 v);
                    return 2;
                }
                trigger_deg = *parsed;
            } else { return 2; }
        } else if (a == "--pull-step-degrees" || a == "--pull-step") {
            if (auto const *v = require_arg("--pull-step-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value()) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: --pull-step-degrees "
                                 "must be a decimal number in degrees "
                                 "(got '%s')\n", v);
                    return 2;
                }
                pull_step_deg = *parsed;
            } else { return 2; }
        } else if (a == "--min-samples-per-cell" || a == "--min-samples") {
            if (auto const *v = require_arg("--min-samples-per-cell"); v) {
                char       *end  = nullptr;
                long long const n = std::strtoll(v, &end, 10);
                if (end == v || end == nullptr || *end != '\0' || n < 0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: --min-samples-per-cell "
                                 "must be a non-negative integer (got '%s')\n",
                                 v);
                    return 2;
                }
                min_samples = static_cast<std::size_t>(n);
            } else { return 2; }
        } else if (a == "--max-neighbor-step-degrees"
                   || a == "--max-neighbor-step") {
            if (auto const *v = require_arg("--max-neighbor-step-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value() || *parsed < 0.0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--max-neighbor-step-degrees must be a "
                                 "non-negative decimal (got '%s')\n", v);
                    return 2;
                }
                max_neighbor_step = *parsed;
            } else { return 2; }
        } else if (a == "--strict-lint") {
            strict_lint = true;
        } else if (a == "--enable-add-back") {
            enable_add_back = true;
        } else if (a == "--add-back-step-degrees"
                   || a == "--add-back-step") {
            if (auto const *v = require_arg("--add-back-step-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value() || *parsed < 0.0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--add-back-step-degrees must be a "
                                 "non-negative decimal (got '%s')\n", v);
                    return 2;
                }
                add_back_step = *parsed;
            } else { return 2; }
        } else if (a == "--add-back-min-clean-samples"
                   || a == "--add-back-min-samples") {
            if (auto const *v = require_arg("--add-back-min-clean-samples"); v) {
                char       *end  = nullptr;
                long long const n = std::strtoll(v, &end, 10);
                if (end == v || end == nullptr || *end != '\0' || n < 0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--add-back-min-clean-samples must be a "
                                 "non-negative integer (got '%s')\n", v);
                    return 2;
                }
                add_back_min_clean = static_cast<std::size_t>(n);
            } else { return 2; }
        } else if (a == "--add-back-clean-threshold-degrees"
                   || a == "--add-back-threshold") {
            if (auto const *v =
                    require_arg("--add-back-clean-threshold-degrees"); v) {
                auto const parsed = parse_decimal(v);
                if (!parsed.has_value() || *parsed < 0.0) {
                    std::fprintf(stderr,
                                 "autotune knock-pull: "
                                 "--add-back-clean-threshold-degrees must "
                                 "be a non-negative decimal (got '%s')\n", v);
                    return 2;
                }
                add_back_threshold = *parsed;
            } else { return 2; }
        } else {
            std::fprintf(stderr,
                         "autotune knock-pull: unknown argument: %s\n",
                         argv[i]);
            return 2;
        }
    }

    bool const rpm_given     = rpm_axis_arg.has_value()  || rpm_axis_file.has_value();
    bool const load_given    = load_axis_arg.has_value() || load_axis_file.has_value();
    bool const current_given = current_arg.has_value()   || current_file.has_value();
    if (!log_path.has_value() || !rpm_given || !load_given || !current_given) {
        std::fputs("autotune knock-pull: missing required arguments\n",
                   stderr);
        std::fputs("Usage: subuwutuner-cli autotune knock-pull "
                   "--log <csv>\n"
                   "       (--rpm-axis <r,r,…> | --rpm-axis-file <path>)\n"
                   "       (--load-axis <l,l,…> | --load-axis-file <path>)\n"
                   "       (--current-timing <d,d,…> | --current-timing-file <path>)"
                   "   (flat row-major, rows × cols = load × rpm)\n"
                   "       [--trigger-degrees 1.5] [--pull-step-degrees 0.75]\n"
                   "       [--min-samples-per-cell 30] "
                   "[--max-neighbor-step-degrees 3.0]\n"
                   "       [--strict-lint]\n",
                   stderr);
        return 2;
    }
    auto inline_and_file_exclusive = [](char const *flag_inline,
                                         char const *flag_file,
                                         bool        has_inline,
                                         bool        has_file) {
        if (has_inline && has_file) {
            std::fprintf(stderr,
                         "autotune knock-pull: %s and %s are mutually exclusive\n",
                         flag_inline, flag_file);
            return true;
        }
        return false;
    };
    if (inline_and_file_exclusive("--rpm-axis", "--rpm-axis-file",
                                   rpm_axis_arg.has_value(),
                                   rpm_axis_file.has_value())) {
        return 2;
    }
    if (inline_and_file_exclusive("--load-axis", "--load-axis-file",
                                   load_axis_arg.has_value(),
                                   load_axis_file.has_value())) {
        return 2;
    }
    if (inline_and_file_exclusive("--current-timing", "--current-timing-file",
                                   current_arg.has_value(),
                                   current_file.has_value())) {
        return 2;
    }

    // Returns 0 on success and fills `out`; returns 1 if the file
    // path was given but couldn't be read (already printed by us — rc=1
    // mirrors MAF's "I/O failed" exit code); returns 2 if the list was
    // empty or the inline parser rejected the input (rc=2 mirrors MAF's
    // "argument was malformed" exit code).
    auto load_list = [](char const                                 *flag_file,
                         char const                                 *list_label,
                         std::optional<std::string> const           &inline_arg,
                         std::optional<std::filesystem::path> const &file_arg,
                         std::vector<double>                        &out) -> int {
        std::optional<std::vector<double>> v;
        if (file_arg.has_value()) {
            v = read_decimal_list_file(*file_arg);
            if (!v.has_value()) {
                std::fprintf(stderr,
                             "autotune knock-pull: cannot read %s '%s' "
                             "(missing, unreadable, or contains a non-numeric "
                             "token)\n",
                             flag_file, file_arg->string().c_str());
                return 1;
            }
        } else {
            v = parse_double_list(*inline_arg);
        }
        if (!v.has_value() || v->empty()) {
            std::fprintf(stderr,
                         "autotune knock-pull: %s must yield a non-empty "
                         "list of numbers\n", list_label);
            return 2;
        }
        out = std::move(*v);
        return 0;
    };

    std::vector<double> rpm_axis;
    if (int const rc = load_list("--rpm-axis-file", "rpm-axis",
                                 rpm_axis_arg, rpm_axis_file, rpm_axis);
        rc != 0) {
        return rc;
    }
    std::vector<double> load_axis;
    if (int const rc = load_list("--load-axis-file", "load-axis",
                                 load_axis_arg, load_axis_file, load_axis);
        rc != 0) {
        return rc;
    }
    std::vector<double> current;
    if (int const rc = load_list("--current-timing-file", "current-timing",
                                 current_arg, current_file, current);
        rc != 0) {
        return rc;
    }
    if (current.size() != rpm_axis.size() * load_axis.size()) {
        std::fprintf(stderr,
                     "autotune knock-pull: --current-timing length (%zu) must "
                     "equal --rpm-axis (%zu) × --load-axis (%zu) = %zu\n",
                     current.size(), rpm_axis.size(), load_axis.size(),
                     rpm_axis.size() * load_axis.size());
        return 2;
    }

    std::ifstream in{*log_path, std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "autotune knock-pull: cannot open --log '%s'\n",
                     log_path->string().c_str());
        return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto const samples = st::autotune::read_knock_samples_csv(buf.str());
    if (!samples.has_value()) {
        std::fprintf(stderr, "autotune knock-pull: %s\n",
                     samples.error().to_string().c_str());
        return 1;
    }

    st::autotune::KnockPullOptions opts;
    if (trigger_deg.has_value())   { opts.trigger_degrees      = *trigger_deg; }
    if (pull_step_deg.has_value()) { opts.pull_step_degrees    = *pull_step_deg; }
    if (min_samples.has_value())   { opts.min_samples_per_cell = *min_samples; }

    auto const pull_result = st::autotune::tune_knock_pull(
        rpm_axis, load_axis, current, *samples, opts);
    if (!pull_result.has_value()) {
        std::fprintf(stderr, "autotune knock-pull: %s\n",
                     pull_result.error().to_string().c_str());
        return 1;
    }

    // Opt-in add-back pass per docs/12. The pull pass is always run;
    // the add-back pass only runs when --enable-add-back is set,
    // matching the docs default-off posture for the dangerous direction
    // (adding timing). `apply_knock_add_back` is a no-op when
    // `opts.enabled` is false, so always calling it keeps the gate in
    // one place (the kernel) rather than splitting it across CLI + lib.
    st::autotune::KnockAddBackOptions add_opts;
    add_opts.enabled = enable_add_back;
    if (add_back_step.has_value()) {
        add_opts.add_step_degrees = *add_back_step;
    }
    if (add_back_min_clean.has_value()) {
        add_opts.min_clean_samples_per_cell = *add_back_min_clean;
    }
    if (add_back_threshold.has_value()) {
        add_opts.clean_threshold_degrees = *add_back_threshold;
    }
    auto const result =
        st::autotune::apply_knock_add_back(*pull_result, add_opts);

    std::printf("Loaded %zu samples from %s\n",
                result.total_samples,
                log_path->string().c_str());
    double const retained_pct =
        result.total_samples == 0
            ? 0.0
            : 100.0
                  * static_cast<double>(result.samples_after_gates)
                  / static_cast<double>(result.total_samples);
    std::printf("After quality gates: %zu samples (%.1f%% retained)\n\n",
                result.samples_after_gates, retained_pct);

    // Count pulled vs added-back cells. A cell is "added back" when it
    // wasn't pulled but its proposed_value differs from current_value
    // — that's the post-pass marker apply_knock_add_back leaves.
    std::size_t pulled       = 0;
    std::size_t added_back   = 0;
    double      max_pull_deg = 0.0;
    std::size_t max_pull_cell = 0;
    double      max_add_deg  = 0.0;
    std::size_t max_add_cell = 0;
    for (auto const &c : result.cells) {
        if (c.pulled) {
            ++pulled;
            double const drop = c.current_value - c.proposed_value;
            if (drop > max_pull_deg) {
                max_pull_deg  = drop;
                max_pull_cell = c.cell_index;
            }
        } else if (c.proposed_value > c.current_value) {
            ++added_back;
            double const gain = c.proposed_value - c.current_value;
            if (gain > max_add_deg) {
                max_add_deg  = gain;
                max_add_cell = c.cell_index;
            }
        }
    }
    std::printf("Knock pull proposal:\n");
    std::printf("  Cells pulled:          %zu / %zu\n",
                pulled, result.cells.size());
    std::printf("  Pull step:             %.2f°\n", opts.pull_step_degrees);
    std::printf("  Trigger threshold:     mean feedback knock < -%.2f° "
                "(over ≥ %zu samples)\n",
                opts.trigger_degrees, opts.min_samples_per_cell);
    if (pulled > 0) {
        std::size_t const row = max_pull_cell / rpm_axis.size();
        std::size_t const col = max_pull_cell % rpm_axis.size();
        std::printf("  Worst cell:            load=%.2f rpm=%.0f "
                    "pulled %.2f°\n",
                    load_axis[row], rpm_axis[col], max_pull_deg);
    }
    if (enable_add_back) {
        std::printf("  Cells added back:      %zu (clean cells, +%.2f°)\n",
                    added_back, add_opts.add_step_degrees);
        if (added_back > 0) {
            std::size_t const row = max_add_cell / rpm_axis.size();
            std::size_t const col = max_add_cell % rpm_axis.size();
            std::printf("  Largest add-back:      load=%.2f rpm=%.0f "
                        "+%.2f°\n",
                        load_axis[row], rpm_axis[col], max_add_deg);
        }
    }
    std::printf("\n");

    // 2D ledger: one row per load × one cell-pair per RPM. Compact when
    // the table is small (the docs/12 timing maps are typically 8×8 to
    // 16×16, which fits on a terminal line). Added-back cells use the
    // same signed-degree format as pulled cells — a leading '+' makes
    // it obvious which direction the cell moved.
    std::printf("  load \\ rpm");
    for (std::size_t c = 0; c < rpm_axis.size(); ++c) {
        std::printf(" | %7.0f", rpm_axis[c]);
    }
    std::printf("\n  -----------");
    for (std::size_t c = 0; c < rpm_axis.size(); ++c) {
        std::printf("-+--------");
    }
    std::printf("\n");
    for (std::size_t r = 0; r < load_axis.size(); ++r) {
        std::printf("  %9.2f ", load_axis[r]);
        for (std::size_t c = 0; c < rpm_axis.size(); ++c) {
            auto const &cell = result.cells[r * rpm_axis.size() + c];
            double const delta = cell.proposed_value - cell.current_value;
            if (cell.pulled || delta != 0.0) {
                std::printf("|  %+5.2f ", delta);
            } else {
                std::printf("|    .   ");
            }
        }
        std::printf("\n");
    }

    // Engine-safety lint per docs/12 §"Engine-safety linting" for the
    // 2D timing surface. Always runs; --strict-lint exits 3 on any
    // violation. Same exit-code convention as `autotune maf`.
    st::autotune::KnockLintOptions lint_opts;
    if (max_neighbor_step.has_value()) {
        lint_opts.max_neighbor_step_degrees = *max_neighbor_step;
    }
    auto const violations = st::autotune::lint_knock_proposal(
        rpm_axis, load_axis, result, lint_opts);
    return print_lint_section(violations, strict_lint);
}

// Helper for `can-*`: emit one column-aligned line for a (bus, id) summary.
void print_id_row(st::can::BusId bus, std::uint32_t can_id, bool extended,
                  std::size_t count, double rate_hz, double avg_dlc) {
    std::printf("  bus=%u  id=0x%0*X%s  frames=%6zu  rate=%8.2f Hz  dlc~%.1f\n",
                static_cast<unsigned>(bus),
                extended ? 8 : 3,
                can_id,
                extended ? "" : "  ",
                count, rate_hz, avg_dlc);
}

int cmd_can_replay(int argc, char *argv[]) {
    std::optional<std::filesystem::path> trace_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "can-replay: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!trace_path.has_value()) {
            trace_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-replay: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!trace_path.has_value()) {
        std::fputs("can-replay: missing required argument\n"
                   "Usage: subuwutuner-cli can-replay <FILE.asc>\n", stderr);
        return 2;
    }

    auto const frames = st::can::read_asc(*trace_path);
    if (!frames.has_value()) {
        std::fprintf(stderr, "can-replay: %s\n", frames.error().to_string().c_str());
        return 1;
    }
    if (frames->empty()) {
        std::printf("File: %s\nNo frames.\n", trace_path->string().c_str());
        return 0;
    }

    // Per-id aggregates. Key = (bus << 32) | id; preserve insertion order
    // for stable output via a parallel vector of keys.
    struct Agg {
        st::can::BusId bus{st::can::BusId::Hs};
        std::uint32_t  id{0};
        bool           extended{false};
        std::size_t    count{0};
        std::uint64_t  dlc_sum{0};
        std::array<std::size_t, 256> byte0_hist{};
        std::int64_t   first_ts_ns{0};
        std::int64_t   last_ts_ns{0};
    };
    std::unordered_map<std::uint64_t, Agg> aggs;
    std::vector<std::uint64_t>             order;

    std::int64_t global_first = frames->front().timestamp_ns;
    std::int64_t global_last  = frames->front().timestamp_ns;
    for (auto const &f : *frames) {
        std::uint64_t const key =
            (static_cast<std::uint64_t>(f.bus) << 32) | f.id;
        auto [it, inserted] = aggs.try_emplace(key);
        if (inserted) {
            it->second.bus = f.bus;
            it->second.id  = f.id;
            it->second.extended = f.extended;
            it->second.first_ts_ns = f.timestamp_ns;
            order.push_back(key);
        }
        auto &agg = it->second;
        ++agg.count;
        agg.dlc_sum += f.dlc;
        agg.last_ts_ns = f.timestamp_ns;
        if (f.dlc > 0) {
            ++agg.byte0_hist[f.data[0]];
        }
        if (f.timestamp_ns < global_first) global_first = f.timestamp_ns;
        if (f.timestamp_ns > global_last)  global_last  = f.timestamp_ns;
    }

    double const duration_s =
        static_cast<double>(global_last - global_first) / 1e9;

    std::printf("File: %s\n", trace_path->string().c_str());
    std::printf("Frames: %zu   unique ids: %zu   duration: %.3f s\n",
                frames->size(), aggs.size(), duration_s);

    // Sort keys by descending frame count for readability.
    std::sort(order.begin(), order.end(),
              [&](std::uint64_t a, std::uint64_t b) {
                  return aggs[a].count > aggs[b].count;
              });

    std::printf("\nPer-id summary (sorted by count):\n");
    for (auto const key : order) {
        auto const &a = aggs[key];
        double const rate =
            duration_s > 0.0 ? static_cast<double>(a.count) / duration_s : 0.0;
        double const avg_dlc =
            a.count > 0
                ? static_cast<double>(a.dlc_sum) / static_cast<double>(a.count)
                : 0.0;
        print_id_row(a.bus, a.id, a.extended, a.count, rate, avg_dlc);

        // Modal first-byte value, if any payload was seen.
        std::size_t  mode_count = 0;
        unsigned     mode_byte  = 0;
        for (std::size_t v = 0; v < 256; ++v) {
            if (a.byte0_hist[v] > mode_count) {
                mode_count = a.byte0_hist[v];
                mode_byte  = static_cast<unsigned>(v);
            }
        }
        if (mode_count > 0) {
            double const share =
                static_cast<double>(mode_count) / static_cast<double>(a.count);
            std::printf("        byte0 mode=0x%02X (%.0f%% of frames)\n",
                        mode_byte, share * 100.0);
        }
    }
    return 0;
}

int cmd_can_diff(int argc, char *argv[]) {
    std::optional<std::filesystem::path> a_path;
    std::optional<std::filesystem::path> b_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "can-diff: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!a_path.has_value()) {
            a_path = std::filesystem::path{argv[i]};
        } else if (!b_path.has_value()) {
            b_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-diff: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!a_path.has_value() || !b_path.has_value()) {
        std::fputs("can-diff: missing required arguments\n"
                   "Usage: subuwutuner-cli can-diff <A.asc> <B.asc>\n", stderr);
        return 2;
    }

    auto const fa = st::can::read_asc(*a_path);
    if (!fa.has_value()) {
        std::fprintf(stderr, "can-diff: %s: %s\n",
                     a_path->string().c_str(), fa.error().to_string().c_str());
        return 1;
    }
    auto const fb = st::can::read_asc(*b_path);
    if (!fb.has_value()) {
        std::fprintf(stderr, "can-diff: %s: %s\n",
                     b_path->string().c_str(), fb.error().to_string().c_str());
        return 1;
    }

    struct Entry {
        st::can::BusId bus{st::can::BusId::Hs};
        std::uint32_t  id{0};
        bool           extended{false};
        std::size_t    count{0};
    };
    auto tally = [](std::vector<st::can::Frame> const &frames) {
        std::unordered_map<std::uint64_t, Entry> m;
        for (auto const &f : frames) {
            std::uint64_t const key =
                (static_cast<std::uint64_t>(f.bus) << 32) | f.id;
            auto [it, inserted] = m.try_emplace(key);
            if (inserted) {
                it->second.bus = f.bus;
                it->second.id  = f.id;
                it->second.extended = f.extended;
            }
            ++it->second.count;
        }
        return m;
    };

    auto const ma = tally(*fa);
    auto const mb = tally(*fb);

    std::printf("A: %s   (%zu frames, %zu ids)\n",
                a_path->string().c_str(), fa->size(), ma.size());
    std::printf("B: %s   (%zu frames, %zu ids)\n",
                b_path->string().c_str(), fb->size(), mb.size());

    auto sorted_keys = [](auto const &m) {
        std::vector<std::uint64_t> keys;
        keys.reserve(m.size());
        for (auto const &kv : m) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        return keys;
    };

    std::vector<std::uint64_t> only_a;
    std::vector<std::uint64_t> only_b;
    std::vector<std::uint64_t> shared;
    for (auto const key : sorted_keys(ma)) {
        if (mb.find(key) == mb.end()) only_a.push_back(key);
        else                          shared.push_back(key);
    }
    for (auto const key : sorted_keys(mb)) {
        if (ma.find(key) == ma.end()) only_b.push_back(key);
    }

    std::printf("\nIds only in A (%zu):\n", only_a.size());
    for (auto const key : only_a) {
        auto const &e = ma.at(key);
        std::printf("  bus=%u  id=0x%0*X  count=%zu\n",
                    static_cast<unsigned>(e.bus),
                    e.extended ? 8 : 3, e.id, e.count);
    }
    std::printf("\nIds only in B (%zu):\n", only_b.size());
    for (auto const key : only_b) {
        auto const &e = mb.at(key);
        std::printf("  bus=%u  id=0x%0*X  count=%zu\n",
                    static_cast<unsigned>(e.bus),
                    e.extended ? 8 : 3, e.id, e.count);
    }

    std::size_t shared_changed = 0;
    for (auto const key : shared) {
        if (ma.at(key).count != mb.at(key).count) ++shared_changed;
    }
    std::printf("\nShared ids with count delta (%zu of %zu shared):\n",
                shared_changed, shared.size());
    for (auto const key : shared) {
        auto const &ea = ma.at(key);
        auto const &eb = mb.at(key);
        if (ea.count == eb.count) continue;
        long long const delta = static_cast<long long>(eb.count) - static_cast<long long>(ea.count);
        std::printf("  bus=%u  id=0x%0*X  A=%zu  B=%zu  delta=%+lld\n",
                    static_cast<unsigned>(ea.bus),
                    ea.extended ? 8 : 3, ea.id,
                    ea.count, eb.count, delta);
    }
    return 0;
}

int cmd_can_discover(int argc, char *argv[]) {
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> output_path;
    double                               baseline_secs = 10.0;
    std::optional<unsigned>              bus_filter;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "can-discover: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--from") {
            if (auto const *v = require_arg("--from"); v) trace_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--baseline") {
            auto const *v = require_arg("--baseline");
            if (v == nullptr) return 2;
            std::string_view sv{v};
            // Accept "10", "10s" — strip trailing 's' for ergonomics.
            if (!sv.empty() && (sv.back() == 's' || sv.back() == 'S')) {
                sv.remove_suffix(1);
            }
            double           secs = 0.0;
            char            *end  = nullptr;
            std::string      tmp{sv};
            secs = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str() || secs <= 0.0) {
                std::fprintf(stderr, "can-discover: --baseline must be a positive number of seconds\n");
                return 2;
            }
            baseline_secs = secs;
        } else if (a == "--bus") {
            auto const *v = require_arg("--bus");
            if (v == nullptr) return 2;
            unsigned    value = 0;
            auto const  res   = std::from_chars(v, v + std::strlen(v), value);
            if (res.ec != std::errc{} || value > 3) {
                std::fprintf(stderr, "can-discover: --bus must be 0..3\n");
                return 2;
            }
            bus_filter = value;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) output_path = std::filesystem::path{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "can-discover: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "can-discover: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!trace_path.has_value()) {
        std::fputs("can-discover: missing required argument\n"
                   "Usage: subuwutuner-cli can-discover --from <FILE.asc> [--baseline <secs>]\n"
                   "       [--bus <0..3>] [--output <session.cdb>]\n", stderr);
        return 2;
    }

    auto const all_frames = st::can::read_asc(*trace_path);
    if (!all_frames.has_value()) {
        std::fprintf(stderr, "can-discover: %s\n",
                     all_frames.error().to_string().c_str());
        return 1;
    }
    if (all_frames->empty()) {
        std::fputs("can-discover: trace contains no frames\n", stderr);
        return 1;
    }

    // Optional bus filter: keep only frames on the requested bus, in order.
    std::vector<st::can::Frame> frames;
    frames.reserve(all_frames->size());
    if (bus_filter.has_value()) {
        auto const want = static_cast<st::can::BusId>(*bus_filter);
        for (auto const &f : *all_frames) {
            if (f.bus == want) frames.push_back(f);
        }
        if (frames.empty()) {
            std::fprintf(stderr, "can-discover: no frames on bus %u\n", *bus_filter);
            return 1;
        }
    } else {
        frames = *all_frames;
    }

    std::int64_t const t0          = frames.front().timestamp_ns;
    std::int64_t const baseline_ns = static_cast<std::int64_t>(baseline_secs * 1e9);
    std::int64_t const split_ns    = t0 + baseline_ns;

    // Partition into baseline (timestamp < split) and watch (>= split).
    std::vector<st::can::Frame> baseline_frames;
    std::vector<st::can::Frame> watch_frames;
    baseline_frames.reserve(frames.size());
    for (auto const &f : frames) {
        if (f.timestamp_ns < split_ns) baseline_frames.push_back(f);
        else                           watch_frames.push_back(f);
    }
    if (baseline_frames.empty()) {
        std::fputs("can-discover: baseline window is empty (trace shorter than --baseline?)\n",
                   stderr);
        return 1;
    }

    st::discover::BaselineModel model =
        st::discover::build_baseline(baseline_frames);

    auto const events =
        st::discover::detect_changes(model, watch_frames);

    st::discover::Bundle bundle;
    bundle.schema_version   = 1;
    bundle.captured_at      = "";
    bundle.bus_label        = bus_filter.has_value()
                                  ? std::string{"bus"} + std::to_string(*bus_filter)
                                  : std::string{};
    bundle.baseline_ns      = baseline_ns;
    bundle.baseline_entries = model.entries();
    bundle.events           = events;

    if (output_path.has_value()) {
        if (auto s = st::discover::write_cdb(*output_path, bundle); !s.has_value()) {
            std::fprintf(stderr, "can-discover: %s\n", s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "can-discover: baseline ids=%zu  events=%zu  wrote %s\n",
                     bundle.baseline_entries.size(),
                     bundle.events.size(),
                     output_path->string().c_str());
    } else {
        auto const text = st::discover::write_cdb_string(bundle);
        if (!text.has_value()) {
            std::fprintf(stderr, "can-discover: %s\n", text.error().to_string().c_str());
            return 1;
        }
        std::fputs(text->c_str(), stdout);
        std::fprintf(stderr,
                     "can-discover: baseline ids=%zu  events=%zu\n",
                     bundle.baseline_entries.size(),
                     bundle.events.size());
    }
    return 0;
}

// Convert free-form description text into a DBC-safe identifier:
// lowercase, [a-z0-9_] only, leading digit prefixed with underscore,
// empty input mapped to "signal".
std::string slugify(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (auto const c : text) {
        unsigned char const u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_') {
            out.push_back(static_cast<char>(u));
        } else if (u >= 'A' && u <= 'Z') {
            out.push_back(static_cast<char>(u - 'A' + 'a'));
        } else {
            if (!out.empty() && out.back() != '_') out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "signal";
    if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
    return out;
}

int cmd_can_export_dbc(int argc, char *argv[]) {
    std::optional<std::filesystem::path> cdb_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("can-export-dbc: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "can-export-dbc: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!cdb_path.has_value()) {
            cdb_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-export-dbc: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!cdb_path.has_value()) {
        std::fputs("can-export-dbc: missing required argument\n"
                   "Usage: subuwutuner-cli can-export-dbc <session.cdb> [--output <draft.dbc>]\n",
                   stderr);
        return 2;
    }

    auto const bundle = st::discover::read_cdb(*cdb_path);
    if (!bundle.has_value()) {
        std::fprintf(stderr, "can-export-dbc: %s\n",
                     bundle.error().to_string().c_str());
        return 1;
    }

    st::dbc::Database db;
    db.version = "";
    db.nodes.push_back("Vector__XXX");

    // Build one Message per (bus,id) appearing in the baseline. Index by
    // 64-bit key so signals can be attached to the right Message later.
    std::unordered_map<std::uint64_t, std::size_t> idx_by_key;
    for (auto const &b : bundle->baseline_entries) {
        std::uint64_t const key =
            (static_cast<std::uint64_t>(b.bus) << 32) | b.can_id;
        st::dbc::Message m;
        m.id       = b.can_id;
        m.extended = b.extended;
        char buf[32];
        std::snprintf(buf, sizeof buf, "MSG_%X", b.can_id);
        m.name   = buf;
        m.length = b.dlc;
        m.sender = "Vector__XXX";
        idx_by_key[key] = db.messages.size();
        db.messages.push_back(std::move(m));
    }

    // Each labeled Change event becomes a draft SG_ on its message.
    // Conventions per docs/14: identity scaling, Motorola/big-endian,
    // unsigned. Bit position = first changed byte * 8; length covers
    // the contiguous run min..max byte index.
    std::size_t signals_added       = 0;
    std::size_t orphan_signals      = 0;   // event on an id missing from baseline
    std::size_t skipped_unlabeled   = 0;
    std::size_t skipped_new_id      = 0;
    std::unordered_map<std::size_t, std::unordered_map<std::string, int>> name_dedup;

    for (auto const &ev : bundle->events) {
        if (ev.kind == st::discover::DiscoveryEvent::Kind::NewId) {
            ++skipped_new_id;
            continue;
        }
        if (ev.description.empty() || ev.changed_byte_indices.empty()) {
            ++skipped_unlabeled;
            continue;
        }
        std::uint64_t const key =
            (static_cast<std::uint64_t>(ev.bus) << 32) | ev.can_id;
        auto const it = idx_by_key.find(key);
        if (it == idx_by_key.end()) {
            ++orphan_signals;
            continue;
        }
        auto &msg = db.messages[it->second];

        std::uint8_t lo = ev.changed_byte_indices.front();
        std::uint8_t hi = lo;
        for (auto const b : ev.changed_byte_indices) {
            if (b < lo) lo = b;
            if (b > hi) hi = b;
        }

        st::dbc::Signal sig;
        std::string     base = slugify(ev.description);
        // Disambiguate signals that slugify to the same identifier
        // within the same message.
        auto &seen = name_dedup[it->second];
        int  &n    = seen[base];
        sig.name   = (n == 0) ? base : base + "_" + std::to_string(n + 1);
        ++n;
        sig.start_bit   = static_cast<std::size_t>(lo) * 8U;
        sig.length_bits = static_cast<std::size_t>(hi - lo + 1) * 8U;
        sig.byte_order  = st::dbc::ByteOrder::Motorola;
        sig.sign        = st::dbc::SignKind::Unsigned;
        sig.factor      = 1.0;
        sig.offset      = 0.0;
        sig.min_value   = 0.0;
        sig.max_value   = sig.length_bits >= 64
                              ? static_cast<double>(std::numeric_limits<std::uint64_t>::max())
                              : static_cast<double>((1ULL << sig.length_bits) - 1);
        sig.unit        = "";
        sig.receivers.push_back("Vector__XXX");
        msg.signals.push_back(std::move(sig));
        ++signals_added;
    }

    auto const text = st::dbc::format_dbc(db);
    if (output_path.has_value()) {
        if (auto s = st::dbc::write_dbc(*output_path, db); !s.has_value()) {
            std::fprintf(stderr, "can-export-dbc: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "can-export-dbc: messages=%zu  signals=%zu  wrote %s\n",
                     db.messages.size(), signals_added,
                     output_path->string().c_str());
    } else {
        std::fputs(text.c_str(), stdout);
        std::fprintf(stderr,
                     "can-export-dbc: messages=%zu  signals=%zu\n",
                     db.messages.size(), signals_added);
    }
    if (skipped_unlabeled > 0 || orphan_signals > 0 || skipped_new_id > 0) {
        std::fprintf(stderr,
                     "can-export-dbc: skipped unlabeled=%zu  new-id=%zu  orphan=%zu\n",
                     skipped_unlabeled, skipped_new_id, orphan_signals);
    }
    return 0;
}

// Escape a single CSV cell per RFC 4180: wrap in quotes iff it contains
// comma, quote, CR, or LF; double any embedded quotes.
std::string csv_cell(std::string_view s) {
    bool needs_quotes = false;
    for (auto const c : s) {
        if (c == ',' || c == '"' || c == '\r' || c == '\n') {
            needs_quotes = true; break;
        }
    }
    if (!needs_quotes) return std::string{s};
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (auto const c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

int cmd_can_decode(int argc, char *argv[]) {
    std::optional<std::filesystem::path> dbc_path;
    std::optional<std::filesystem::path> asc_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "can-decode: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--dbc") {
            if (auto const *v = require_arg("--dbc"); v) dbc_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) output_path = std::filesystem::path{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "can-decode: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!asc_path.has_value()) {
            asc_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "can-decode: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!dbc_path.has_value() || !asc_path.has_value()) {
        std::fputs("can-decode: missing required arguments\n"
                   "Usage: subuwutuner-cli can-decode --dbc <FILE.dbc> <FILE.asc> [--output <csv>]\n",
                   stderr);
        return 2;
    }

    auto const db = st::dbc::read_dbc(*dbc_path);
    if (!db.has_value()) {
        std::fprintf(stderr, "can-decode: %s\n", db.error().to_string().c_str());
        return 1;
    }
    auto const frames = st::can::read_asc(*asc_path);
    if (!frames.has_value()) {
        std::fprintf(stderr, "can-decode: %s\n", frames.error().to_string().c_str());
        return 1;
    }

    std::ofstream  file_out;
    std::ostream  *out_stream = &std::cout;
    if (output_path.has_value()) {
        file_out.open(*output_path);
        if (!file_out) {
            std::fprintf(stderr, "can-decode: cannot open output: %s\n",
                         output_path->string().c_str());
            return 1;
        }
        out_stream = &file_out;
    }

    (*out_stream) << "timestamp_ns,bus,can_id,signal,value,unit\n";

    std::size_t decoded_rows  = 0;
    std::size_t missing_id    = 0;
    for (auto const &f : *frames) {
        auto const *msg = db->find_message(f.id);
        if (msg == nullptr || msg->extended != f.extended) {
            ++missing_id;
            continue;
        }
        for (auto const &sig : msg->signals) {
            double const value = st::dbc::decode_signal(f.payload(), sig);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", value);
            (*out_stream) << f.timestamp_ns << ','
                          << static_cast<unsigned>(f.bus) << ','
                          << f.id << ','
                          << csv_cell(sig.name) << ','
                          << buf << ','
                          << csv_cell(sig.unit) << '\n';
            ++decoded_rows;
        }
    }
    out_stream->flush();

    std::fprintf(stderr,
                 "can-decode: frames=%zu  rows=%zu  unknown-id-frames=%zu\n",
                 frames->size(), decoded_rows, missing_id);
    return 0;
}

int cmd_flash_plan_info(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-plan-info: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!plan_path.has_value()) {
            plan_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-plan-info: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value()) {
        std::fputs("flash-plan-info: missing required argument\n"
                   "Usage: subuwutuner-cli flash-plan-info <FILE.toml>\n",
                   stderr);
        return 2;
    }
    auto const r = st::flash::read_plan(*plan_path);
    if (!r.has_value()) {
        std::fprintf(stderr, "flash-plan-info: %s\n",
                     r.error().to_string().c_str());
        return 1;
    }
    auto const &p = *r;
    std::printf("Plan: %s\n", plan_path->string().c_str());
    std::printf("  session            = 0x%02X\n",
                static_cast<unsigned>(p.session));
    std::printf("  data_format        = 0x%02X\n",
                static_cast<unsigned>(p.data_format));
    std::printf("  silence_bus        = %s\n",
                p.silence_bus ? "true" : "false");
    std::printf("  verify_after_write = %s\n",
                p.verify_after_write ? "true" : "false");
    std::printf("  dry_run            = %s\n",
                p.dry_run ? "true" : "false");
    std::printf("  block_size_hint    = %u\n",
                static_cast<unsigned>(p.block_size_hint));
    std::printf("  verify_chunk_size  = 0x%X\n",
                static_cast<unsigned>(p.verify_chunk_size));
    std::size_t total_bytes = 0;
    std::printf("\nSector writes (%zu):\n", p.writes.size());
    for (std::size_t i = 0; i < p.writes.size(); ++i) {
        auto const &w = p.writes[i];
        std::printf("  [%zu] 0x%08X .. 0x%08X  (%u bytes)\n",
                    i, w.sector.address,
                    w.sector.address + w.sector.length,
                    static_cast<unsigned>(w.sector.length));
        total_bytes += w.sector.length;
    }
    std::printf("  total              = %zu bytes\n", total_bytes);
    return 0;
}

int cmd_flash_delta(int argc, char *argv[]) {
    std::optional<std::filesystem::path> source_path;
    std::optional<std::filesystem::path> target_path;
    std::optional<std::filesystem::path> output_path;
    std::uint32_t                        sector_size  = 0x1000;
    std::uint32_t                        base_address = 0;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-delta: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--sector-size") {
            auto const *v = require_arg("--sector-size");
            if (v == nullptr) return 2;
            std::uint32_t val = 0;
            std::string_view sv{v};
            int base = 10;
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
                base = 16;
            }
            auto const res = std::from_chars(sv.data(), sv.data() + sv.size(),
                                              val, base);
            if (res.ec != std::errc{} || val == 0) {
                std::fprintf(stderr, "flash-delta: --sector-size must be a positive integer\n");
                return 2;
            }
            sector_size = val;
        } else if (a == "--base-address") {
            auto const *v = require_arg("--base-address");
            if (v == nullptr) return 2;
            std::uint32_t val = 0;
            std::string_view sv{v};
            int base = 10;
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
                base = 16;
            }
            auto const res = std::from_chars(sv.data(), sv.data() + sv.size(),
                                              val, base);
            if (res.ec != std::errc{}) {
                std::fprintf(stderr, "flash-delta: --base-address must be an integer\n");
                return 2;
            }
            base_address = val;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) {
                output_path = std::filesystem::path{v};
            } else {
                return 2;
            }
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-delta: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!source_path.has_value()) {
            source_path = std::filesystem::path{argv[i]};
        } else if (!target_path.has_value()) {
            target_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-delta: extra positional argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!source_path.has_value() || !target_path.has_value()) {
        std::fputs("flash-delta: missing required arguments\n"
                   "Usage: subuwutuner-cli flash-delta <SOURCE.bin> <TARGET.bin>\n"
                   "       [--sector-size <N>] [--base-address <addr>] [--output <plan.toml>]\n",
                   stderr);
        return 2;
    }
    auto const src = st::Rom::from_file(*source_path);
    if (!src.has_value()) {
        std::fprintf(stderr, "flash-delta: %s\n", src.error().to_string().c_str());
        return 1;
    }
    auto const tgt = st::Rom::from_file(*target_path);
    if (!tgt.has_value()) {
        std::fprintf(stderr, "flash-delta: %s\n", tgt.error().to_string().c_str());
        return 1;
    }
    if (src->size() != tgt->size()) {
        std::fprintf(stderr,
                     "flash-delta: source size (%zu) != target size (%zu)\n",
                     src->size(), tgt->size());
        return 1;
    }
    auto const sectors = st::flash::Flasher::compute_delta(
        src->data(), tgt->data(), sector_size, base_address);

    st::flash::FlashPlan plan;
    plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite sw;
        sw.sector = s;
        std::size_t const off =
            static_cast<std::size_t>(s.address - base_address);
        sw.data.assign(tgt->data().begin() + static_cast<std::ptrdiff_t>(off),
                       tgt->data().begin()
                           + static_cast<std::ptrdiff_t>(off + s.length));
        plan.writes.push_back(std::move(sw));
    }

    if (output_path.has_value()) {
        if (plan.writes.empty()) {
            std::fputs("flash-delta: source and target are identical; "
                       "no plan written\n", stderr);
            return 0;
        }
        if (auto s = st::flash::write_plan(*output_path, plan); !s.has_value()) {
            std::fprintf(stderr, "flash-delta: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "flash-delta: %zu sector(s), %zu bytes, wrote %s\n",
                     plan.writes.size(),
                     std::accumulate(plan.writes.begin(), plan.writes.end(),
                                     std::size_t{0},
                                     [](std::size_t acc, auto const &w) {
                                         return acc + w.sector.length;
                                     }),
                     output_path->string().c_str());
    } else {
        std::fputs(st::flash::format_plan(plan).c_str(), stdout);
        std::fprintf(stderr,
                     "flash-delta: %zu sector(s), %zu bytes\n",
                     plan.writes.size(),
                     std::accumulate(plan.writes.begin(), plan.writes.end(),
                                     std::size_t{0},
                                     [](std::size_t acc, auto const &w) {
                                         return acc + w.sector.length;
                                     }));
    }
    return 0;
}

int cmd_flash_resume(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> journal_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) {
                std::fputs("flash-resume: --output requires a path\n", stderr);
                return 2;
            }
            output_path = std::filesystem::path{argv[++i]};
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-resume: unknown option: %s\n", argv[i]);
            return 2;
        } else if (!plan_path.has_value()) {
            plan_path = std::filesystem::path{argv[i]};
        } else if (!journal_path.has_value()) {
            journal_path = std::filesystem::path{argv[i]};
        } else {
            std::fprintf(stderr, "flash-resume: extra positional argument: %s\n",
                         argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value() || !journal_path.has_value()) {
        std::fputs("flash-resume: missing required arguments\n"
                   "Usage: subuwutuner-cli flash-resume <ORIGINAL.plan.toml> "
                   "<JOURNAL.manifest.toml> [--output <resumed.plan.toml>]\n",
                   stderr);
        return 2;
    }

    auto const original = st::flash::read_plan(*plan_path);
    if (!original.has_value()) {
        std::fprintf(stderr, "flash-resume: %s\n",
                     original.error().to_string().c_str());
        return 1;
    }
    auto const journal = st::flash::read_manifest(*journal_path);
    if (!journal.has_value()) {
        std::fprintf(stderr, "flash-resume: %s\n",
                     journal.error().to_string().c_str());
        return 1;
    }

    auto resumed = st::flash::plan_resume(*original, *journal);
    if (!resumed.has_value()) {
        std::fprintf(stderr, "flash-resume: %s\n",
                     resumed.error().to_string().c_str());
        return 1;
    }

    if (resumed->writes.empty()) {
        std::fputs("flash-resume: every sector in the original plan is already "
                   "transferred and verified; no resume needed\n", stderr);
        return 0;
    }

    // Clear journal_path so the resumed plan doesn't accidentally
    // overwrite the original journal on the next execute(). The user can
    // set a fresh path explicitly via the resumed plan TOML if they want
    // continued journaling.
    resumed->journal_path.clear();

    std::size_t const bytes =
        std::accumulate(resumed->writes.begin(), resumed->writes.end(),
                        std::size_t{0},
                        [](std::size_t acc, auto const &w) {
                            return acc + w.sector.length;
                        });

    if (output_path.has_value()) {
        if (auto s = st::flash::write_plan(*output_path, *resumed);
            !s.has_value()) {
            std::fprintf(stderr, "flash-resume: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "flash-resume: %zu sector(s), %zu bytes, wrote %s\n",
                     resumed->writes.size(), bytes,
                     output_path->string().c_str());
    } else {
        std::fputs(st::flash::format_plan(*resumed).c_str(), stdout);
        std::fprintf(stderr,
                     "flash-resume: %zu sector(s), %zu bytes\n",
                     resumed->writes.size(), bytes);
    }
    return 0;
}

// Parse a UDS trace file into {request, response} pairs. Format:
//   > hh hh ...    a tester request (hex bytes, whitespace tolerant,
//                  optional "0x" prefix per byte)
//   < hh hh ...    the matching ECU response
//   # ...          comment to end of line
//   <blank>        ignored
//
// Strict alternation: each '>' line is immediately followed by one '<'
// line. Anything else is a parse error.
struct UdsTracePair {
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
};

bool parse_uds_trace(std::filesystem::path const &path,
                     std::vector<UdsTracePair>   &out_pairs,
                     std::string                 &err) {
    std::ifstream in{path};
    if (!in) {
        err = "flash-apply: cannot open trace file: " + path.string();
        return false;
    }
    auto const parse_hex_line = [&](std::string_view body,
                                     std::vector<std::uint8_t> &dst,
                                     int line_no) -> bool {
        std::istringstream iss{std::string{body}};
        std::string        tok;
        while (iss >> tok) {
            std::string_view sv{tok};
            if (sv.starts_with("0x") || sv.starts_with("0X")) {
                sv.remove_prefix(2);
            }
            if (sv.size() != 2) {
                err = "flash-apply: bad hex byte '" + tok + "' on line "
                      + std::to_string(line_no);
                return false;
            }
            unsigned   value = 0;
            auto const res   = std::from_chars(sv.data(),
                                                sv.data() + sv.size(),
                                                value, 16);
            if (res.ec != std::errc{}
                || res.ptr != sv.data() + sv.size()
                || value > 0xFFU) {
                err = "flash-apply: bad hex byte '" + tok + "' on line "
                      + std::to_string(line_no);
                return false;
            }
            dst.push_back(static_cast<std::uint8_t>(value));
        }
        return true;
    };

    std::string line;
    int         line_no  = 0;
    bool        expect_request = true;
    UdsTracePair pending;
    while (std::getline(in, line)) {
        ++line_no;
        if (auto const hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        // Trim leading whitespace.
        std::size_t i = 0;
        while (i < line.size()
               && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size()) continue;  // blank
        char const dir = line[i];
        if (dir != '>' && dir != '<') {
            err = "flash-apply: line " + std::to_string(line_no)
                  + " must start with '>' or '<' after any whitespace";
            return false;
        }
        std::string_view const body{line.data() + i + 1, line.size() - i - 1};
        if (dir == '>') {
            if (!expect_request) {
                err = "flash-apply: line " + std::to_string(line_no)
                      + ": two requests in a row (expected '<')";
                return false;
            }
            pending = UdsTracePair{};
            if (!parse_hex_line(body, pending.request, line_no)) return false;
            if (pending.request.empty()) {
                err = "flash-apply: line " + std::to_string(line_no)
                      + ": request must have at least one byte";
                return false;
            }
            expect_request = false;
        } else {
            if (expect_request) {
                err = "flash-apply: line " + std::to_string(line_no)
                      + ": response with no preceding request";
                return false;
            }
            if (!parse_hex_line(body, pending.response, line_no)) return false;
            if (pending.response.empty()) {
                err = "flash-apply: line " + std::to_string(line_no)
                      + ": response must have at least one byte";
                return false;
            }
            out_pairs.push_back(std::move(pending));
            expect_request = true;
        }
    }
    if (!expect_request) {
        err = "flash-apply: trace ends with an unmatched request "
              "(missing '<' response)";
        return false;
    }
    if (out_pairs.empty()) {
        err = "flash-apply: trace file contains no exchanges: "
              + path.string();
        return false;
    }
    return true;
}

int cmd_flash_apply(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<std::filesystem::path> journal_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-apply: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--plan") {
            if (auto const *v = require_arg("--plan"); v) plan_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v) trace_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--manifest") {
            if (auto const *v = require_arg("--manifest"); v) manifest_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--journal") {
            if (auto const *v = require_arg("--journal"); v) journal_path = std::filesystem::path{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-apply: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "flash-apply: extra positional argument: %s\n",
                         argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value() || !trace_path.has_value()) {
        std::fputs("flash-apply: missing required arguments\n"
                   "Usage: subuwutuner-cli flash-apply --plan <FILE.toml> "
                   "--trace <FILE.uds>\n"
                   "       [--journal <FILE.toml>] [--manifest <FILE.toml>]\n",
                   stderr);
        return 2;
    }

    auto plan = st::flash::read_plan(*plan_path);
    if (!plan.has_value()) {
        std::fprintf(stderr, "flash-apply: %s\n",
                     plan.error().to_string().c_str());
        return 1;
    }
    if (journal_path.has_value()) {
        plan->journal_path = *journal_path;
    }

    std::vector<UdsTracePair> pairs;
    std::string               err;
    if (!parse_uds_trace(*trace_path, pairs, err)) {
        std::fputs(err.c_str(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }

    st::transport::MockTransport mock;
    if (auto s = mock.open({}); !s.has_value()) {
        std::fprintf(stderr, "flash-apply: mock open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }
    for (auto &p : pairs) {
        mock.expect_send_recv(std::move(p.request), std::move(p.response));
    }

    st::flash::Flasher flasher{mock};
    auto const         outcome = flasher.execute(*plan);

    // Always print a summary, regardless of success/failure, since the
    // ExecuteOutcome's report is always populated.
    auto const &report = outcome.report;
    std::printf("flash-apply: %s\n",
                outcome.ok() ? "SUCCESS" : "FAILED");
    std::printf("  entered_session    = %s\n",
                report.entered_session ? "true" : "false");
    std::printf("  silenced_bus       = %s\n",
                report.silenced_bus ? "true" : "false");
    std::printf("  restored_bus       = %s\n",
                report.restored_bus ? "true" : "false");
    std::printf("  bytes_transferred  = %zu\n", report.bytes_transferred);
    std::printf("  sectors            = %zu\n", report.sectors.size());
    for (std::size_t i = 0; i < report.sectors.size(); ++i) {
        auto const &so = report.sectors[i];
        std::printf("    [%zu] 0x%08X .. 0x%08X  erased=%d downloaded=%d "
                    "transferred=%d exited=%d check_deps=%d verified=%d\n",
                    i, so.sector.address,
                    so.sector.address + so.sector.length,
                    static_cast<int>(so.erased),
                    static_cast<int>(so.downloaded),
                    static_cast<int>(so.transferred),
                    static_cast<int>(so.exited),
                    static_cast<int>(so.check_deps_passed),
                    static_cast<int>(so.verified));
    }
    if (!outcome.ok()) {
        std::fprintf(stderr, "flash-apply: %s\n",
                     outcome.error->to_string().c_str());
    }
    if (!mock.exhausted()) {
        std::fprintf(stderr,
                     "flash-apply: warning: %zu trace entries unused\n",
                     mock.remaining());
    }

    // Build a manifest of the run if requested. plan_text is the source
    // TOML text so plan_crc32 is meaningful.
    if (manifest_path.has_value()) {
        std::ifstream pin{*plan_path, std::ios::binary};
        std::ostringstream pss;
        pss << pin.rdbuf();
        auto const manifest =
            st::flash::build_manifest(*plan, pss.str(), report);
        if (auto s = st::flash::write_manifest(*manifest_path, manifest);
            !s.has_value()) {
            std::fprintf(stderr, "flash-apply: %s\n",
                         s.error().to_string().c_str());
            return 1;
        }
        std::fprintf(stderr, "flash-apply: wrote manifest %s\n",
                     manifest_path->string().c_str());
    }

    return outcome.ok() ? 0 : 1;
}

namespace {

// Parse a TOML-style hex-or-decimal integer ("0x100" -> 256, "256" -> 256).
// Returns true on success.
bool parse_uint32_arg(std::string_view sv, std::uint32_t &out) {
    int base = 10;
    if (sv.starts_with("0x") || sv.starts_with("0X")) {
        sv.remove_prefix(2);
        base = 16;
    }
    auto const res = std::from_chars(sv.data(), sv.data() + sv.size(),
                                      out, base);
    return res.ec == std::errc{} && res.ptr == sv.data() + sv.size();
}

} // namespace

// Format a byte span as space-separated uppercase hex (no prefix).
std::string hex_bytes_line(std::span<std::uint8_t const> bytes) {
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) out.push_back(' ');
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned>(bytes[i]));
        out.append(buf, 2);
    }
    return out;
}

// flash-trace: walk the exact UDS sequence Flasher::execute would emit
// for `plan` and write `> req` / `< resp` pairs with canned positive
// responses to `out`. The reported maxNumberOfBlockLength in the
// RequestDownload response is fixed at 0x1000 so the TransferData
// chunking matches whatever choose_block_payload() picks given the
// plan's block_size_hint — same formula the orchestrator uses, so the
// generated trace lines up exactly with what the orchestrator sends.
void emit_happy_path_trace(st::flash::FlashPlan const &plan,
                            std::ostream                &out) {
    constexpr std::uint32_t kReportedMaxBlockLength = 0x1000;
    constexpr std::uint32_t kReportedPayloadCap     = kReportedMaxBlockLength - 2U;

    auto const write_pair = [&](std::vector<std::uint8_t> const &req,
                                 std::vector<std::uint8_t> const &resp,
                                 char const *label) {
        if (label != nullptr) out << "# " << label << "\n";
        out << "> " << hex_bytes_line(req) << "\n";
        out << "< " << hex_bytes_line(resp) << "\n";
    };

    out << "# Generated by 'subuwutuner-cli flash-trace' — guaranteed-success "
           "trace for the named plan.\n";

    // 1. DSC programming.
    write_pair(st::ecu::uds::build_dsc_request(plan.session),
               {static_cast<std::uint8_t>(0x50),
                static_cast<std::uint8_t>(plan.session)},
               "DSC programming");

    // 2. CC off.
    if (plan.silence_bus) {
        write_pair({0x28, 0x03, 0x03}, {0x68, 0x03},
                   "CommunicationControl: disable RX+TX, normal+nm");
    }

    // 3. Per sector. dry_run skips this whole block (matches
    //    Flasher::execute), so emit nothing for sectors when dry_run.
    if (!plan.dry_run) {
        for (auto const &w : plan.writes) {
            // 3a. eraseMemory.
            std::vector<std::uint8_t> opt;
            opt.reserve(9);
            opt.push_back(0x44);
            for (int shift : {24, 16, 8, 0}) {
                opt.push_back(static_cast<std::uint8_t>(
                    (w.sector.address >> shift) & 0xFFU));
            }
            for (int shift : {24, 16, 8, 0}) {
                opt.push_back(static_cast<std::uint8_t>(
                    (w.sector.length >> shift) & 0xFFU));
            }
            write_pair(st::ecu::uds::build_routine_control(
                           st::ecu::uds::kRcStart,
                           st::ecu::uds::kRidEraseMemory, opt),
                       {0x71, 0x01, 0xFF, 0x00},
                       "eraseMemory");

            // 3b. RequestDownload. Reported max_block_length = 0x1000.
            write_pair(st::ecu::uds::build_request_download(
                           plan.data_format, w.sector.address, w.sector.length),
                       {0x74, 0x20, 0x10, 0x00},
                       "RequestDownload (max_block_length=0x1000)");

            // 3c. Chunked TransferData. block_payload =
            //     min(reported - 2, block_size_hint), or just reported - 2
            //     when block_size_hint == 0. Matches choose_block_payload.
            std::uint32_t const block_payload =
                (plan.block_size_hint == 0
                    || kReportedPayloadCap < plan.block_size_hint)
                    ? kReportedPayloadCap
                    : plan.block_size_hint;
            std::uint8_t counter = 1;
            std::size_t  offset  = 0;
            while (offset < w.data.size()) {
                std::size_t const remaining = w.data.size() - offset;
                std::size_t const this_block =
                    remaining < block_payload ? remaining : block_payload;
                std::span<std::uint8_t const> chunk{w.data.data() + offset,
                                                     this_block};
                write_pair(st::ecu::uds::build_transfer_data(counter, chunk),
                           {0x76, counter},
                           offset == 0 ? "TransferData" : nullptr);
                offset += this_block;
                counter = static_cast<std::uint8_t>(counter + 1U);
            }

            // 3d. RequestTransferExit.
            write_pair(st::ecu::uds::build_request_transfer_exit(),
                       {0x77},
                       "RequestTransferExit");

            // 3e. checkProgrammingDependencies.
            write_pair(st::ecu::uds::build_routine_control(
                           st::ecu::uds::kRcStart,
                           st::ecu::uds::kRidCheckProgrammingDependencies),
                       {0x71, 0x01, 0xFF, 0x01},
                       "checkProgrammingDependencies");

            // 3f. Verify readback (only when verify_after_write).
            if (plan.verify_after_write) {
                std::uint32_t const chunk_size = plan.verify_chunk_size == 0
                    ? static_cast<std::uint32_t>(w.data.size())
                    : plan.verify_chunk_size;
                std::size_t off = 0;
                while (off < w.data.size()) {
                    std::size_t const remaining = w.data.size() - off;
                    std::size_t const this_chunk =
                        remaining < chunk_size ? remaining : chunk_size;
                    auto const req = st::ecu::uds::build_read_memory_by_address(
                        w.sector.address + static_cast<std::uint32_t>(off),
                        static_cast<std::uint32_t>(this_chunk));
                    std::vector<std::uint8_t> resp;
                    resp.reserve(1 + this_chunk);
                    resp.push_back(0x63);
                    resp.insert(resp.end(),
                                w.data.begin() + static_cast<std::ptrdiff_t>(off),
                                w.data.begin()
                                    + static_cast<std::ptrdiff_t>(off + this_chunk));
                    write_pair(req, resp, off == 0 ? "verify readback" : nullptr);
                    off += this_chunk;
                }
            }
        }
    }

    // 4. CC on.
    if (plan.silence_bus) {
        write_pair({0x28, 0x00, 0x03}, {0x68, 0x00},
                   "CommunicationControl: re-enable RX+TX");
    }
}

int cmd_flash_trace(int argc, char *argv[]) {
    std::optional<std::filesystem::path> plan_path;
    std::optional<std::filesystem::path> output_path;
    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "flash-trace: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--plan") {
            if (auto const *v = require_arg("--plan"); v) plan_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) output_path = std::filesystem::path{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "flash-trace: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "flash-trace: extra positional argument: %s\n",
                         argv[i]);
            return 2;
        }
    }
    if (!plan_path.has_value() || !output_path.has_value()) {
        std::fputs("flash-trace: missing required arguments\n"
                   "Usage: subuwutuner-cli flash-trace --plan <FILE.toml> "
                   "--output <FILE.uds>\n",
                   stderr);
        return 2;
    }
    auto const plan = st::flash::read_plan(*plan_path);
    if (!plan.has_value()) {
        std::fprintf(stderr, "flash-trace: %s\n",
                     plan.error().to_string().c_str());
        return 1;
    }
    std::ofstream out{*output_path};
    if (!out) {
        std::fprintf(stderr, "flash-trace: cannot open output: %s\n",
                     output_path->string().c_str());
        return 1;
    }
    emit_happy_path_trace(*plan, out);
    if (!out) {
        std::fprintf(stderr, "flash-trace: write failed: %s\n",
                     output_path->string().c_str());
        return 1;
    }
    std::fprintf(stderr, "flash-trace: wrote %s\n",
                 output_path->string().c_str());
    return 0;
}

int cmd_rom_pull(int argc, char *argv[]) {
    std::optional<std::uint32_t>         addr;
    std::optional<std::uint32_t>         size;
    std::uint32_t                        max_chunk = 0x100;
    std::optional<std::filesystem::path> trace_path;
    std::optional<std::filesystem::path> output_path;

    for (int i = 0; i < argc; ++i) {
        std::string_view const a{argv[i]};
        auto const             require_arg = [&](char const *name) -> char const * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "rom-pull: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--addr") {
            auto const *v = require_arg("--addr");
            if (v == nullptr) return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val)) {
                std::fprintf(stderr, "rom-pull: --addr must be a hex or decimal "
                             "integer\n");
                return 2;
            }
            addr = val;
        } else if (a == "--size") {
            auto const *v = require_arg("--size");
            if (v == nullptr) return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fprintf(stderr, "rom-pull: --size must be a positive "
                             "hex or decimal integer\n");
                return 2;
            }
            size = val;
        } else if (a == "--max-chunk") {
            auto const *v = require_arg("--max-chunk");
            if (v == nullptr) return 2;
            std::uint32_t val = 0;
            if (!parse_uint32_arg(v, val) || val == 0) {
                std::fprintf(stderr, "rom-pull: --max-chunk must be a positive "
                             "hex or decimal integer\n");
                return 2;
            }
            max_chunk = val;
        } else if (a == "--trace") {
            if (auto const *v = require_arg("--trace"); v) trace_path = std::filesystem::path{v};
            else return 2;
        } else if (a == "--output" || a == "-o") {
            if (auto const *v = require_arg("--output"); v) output_path = std::filesystem::path{v};
            else return 2;
        } else if (a.starts_with("--")) {
            std::fprintf(stderr, "rom-pull: unknown option: %s\n", argv[i]);
            return 2;
        } else {
            std::fprintf(stderr, "rom-pull: extra positional argument: %s\n",
                         argv[i]);
            return 2;
        }
    }
    if (!addr.has_value() || !size.has_value() || !trace_path.has_value()
        || !output_path.has_value()) {
        std::fputs("rom-pull: missing required arguments\n"
                   "Usage: subuwutuner-cli rom-pull --addr <hex> --size <hex> "
                   "--trace <FILE.uds> --output <FILE.bin>\n"
                   "       [--max-chunk <hex>]\n",
                   stderr);
        return 2;
    }

    std::vector<UdsTracePair> pairs;
    std::string               err;
    if (!parse_uds_trace(*trace_path, pairs, err)) {
        std::fputs(err.c_str(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }

    st::transport::MockTransport mock;
    if (auto s = mock.open({}); !s.has_value()) {
        std::fprintf(stderr, "rom-pull: mock open failed: %s\n",
                     s.error().to_string().c_str());
        return 1;
    }
    for (auto &p : pairs) {
        mock.expect_send_recv(std::move(p.request), std::move(p.response));
    }

    st::flash::Flasher flasher{mock};
    auto const         r = flasher.read_full_rom(*addr, *size, max_chunk);
    if (!r.has_value()) {
        std::fprintf(stderr, "rom-pull: %s\n", r.error().to_string().c_str());
        return 1;
    }

    std::ofstream out{*output_path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "rom-pull: cannot open output: %s\n",
                     output_path->string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<char const *>(r->data()),
              static_cast<std::streamsize>(r->size()));
    if (!out) {
        std::fprintf(stderr, "rom-pull: write failed: %s\n",
                     output_path->string().c_str());
        return 1;
    }

    if (!mock.exhausted()) {
        std::fprintf(stderr,
                     "rom-pull: warning: %zu trace entries unused\n",
                     mock.remaining());
    }
    std::fprintf(stderr,
                 "rom-pull: read %u bytes from 0x%08X in chunks of %u; "
                 "wrote %s\n",
                 static_cast<unsigned>(*size), *addr,
                 static_cast<unsigned>(max_chunk),
                 output_path->string().c_str());
    return 0;
}

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
    if (cmd == "pack-info") {
        return cmd_pack_info(argc - 2, argv + 2);
    }
    if (cmd == "table-list") {
        return cmd_table_list(argc - 2, argv + 2);
    }
    if (cmd == "project-undo") {
        return cmd_project_step(argc - 2, argv + 2, /*forward=*/false);
    }
    if (cmd == "project-redo") {
        return cmd_project_step(argc - 2, argv + 2, /*forward=*/true);
    }
    if (cmd == "log") {
        return cmd_log(argc - 2, argv + 2);
    }
    if (cmd == "can-replay") {
        return cmd_can_replay(argc - 2, argv + 2);
    }
    if (cmd == "can-diff") {
        return cmd_can_diff(argc - 2, argv + 2);
    }
    if (cmd == "can-discover") {
        return cmd_can_discover(argc - 2, argv + 2);
    }
    if (cmd == "can-export-dbc") {
        return cmd_can_export_dbc(argc - 2, argv + 2);
    }
    if (cmd == "can-decode") {
        return cmd_can_decode(argc - 2, argv + 2);
    }
    if (cmd == "flash-plan-info") {
        return cmd_flash_plan_info(argc - 2, argv + 2);
    }
    if (cmd == "flash-delta") {
        return cmd_flash_delta(argc - 2, argv + 2);
    }
    if (cmd == "flash-resume") {
        return cmd_flash_resume(argc - 2, argv + 2);
    }
    if (cmd == "flash-apply") {
        return cmd_flash_apply(argc - 2, argv + 2);
    }
    if (cmd == "rom-pull") {
        return cmd_rom_pull(argc - 2, argv + 2);
    }
    if (cmd == "flash-trace") {
        return cmd_flash_trace(argc - 2, argv + 2);
    }
    if (cmd == "autotune") {
        if (argc < 3) {
            std::fputs("autotune: missing subcommand. Try 'autotune maf'.\n",
                       stderr);
            return 2;
        }
        std::string_view const sub{argv[2]};
        if (sub == "maf") {
            return cmd_autotune_maf(argc - 3, argv + 3);
        }
        if (sub == "knock-pull") {
            return cmd_autotune_knock_pull(argc - 3, argv + 3);
        }
        std::fprintf(stderr, "autotune: unknown subcommand: %s\n", argv[2]);
        return 2;
    }

    std::fprintf(stderr, "subuwutuner-cli: unknown argument: %s\n", argv[1]);
    std::fprintf(stderr, "Try 'subuwutuner-cli --help'.\n");
    return 2;
}
