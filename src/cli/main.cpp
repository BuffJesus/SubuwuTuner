// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/version.hpp"
#include "st/defs.hpp"
#include "st/rom.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
    "                            and summarize its tables.\n";

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

    std::fprintf(stderr, "subuwutuner-cli: unknown argument: %s\n", argv[1]);
    std::fprintf(stderr, "Try 'subuwutuner-cli --help'.\n");
    return 2;
}
