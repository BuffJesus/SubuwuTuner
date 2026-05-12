// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubaruTuner Authors

#include "st/core/version.hpp"
#include "st/rom.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "subarutuner-cli — headless ECU calibration tool\n"
    "\n"
    "USAGE:\n"
    "    subarutuner-cli [OPTIONS] [COMMAND] [ARGS...]\n"
    "\n"
    "OPTIONS:\n"
    "    -h, --help        Print this help and exit\n"
    "    -V, --version     Print version and exit\n"
    "\n"
    "COMMANDS:\n"
    "    rom-info <FILE>   Print size, CRC32, and embedded ASCII strings of a ROM\n";

void print_version() {
    std::printf("%.*s %.*s\n",
                static_cast<int>(st::Version::name().size()), st::Version::name().data(),
                static_cast<int>(st::Version::string().size()), st::Version::string().data());
}

void print_usage() { std::fputs(kUsage.data(), stdout); }

bool arg_matches(char const *arg, std::string_view short_form, std::string_view long_form) {
    auto const len = std::strlen(arg);
    std::string_view const sv{arg, len};
    return sv == short_form || sv == long_form;
}

int cmd_rom_info(int argc, char *argv[]) {
    if (argc < 1) {
        std::fputs("rom-info: missing ROM path\n", stderr);
        std::fputs("Usage: subarutuner-cli rom-info <FILE>\n", stderr);
        return 2;
    }

    std::filesystem::path const path{argv[0]};
    auto const                  loaded = st::Rom::from_file(path);
    if (!loaded.has_value()) {
        std::fprintf(stderr, "rom-info: %s\n", loaded.error().to_string().c_str());
        return 1;
    }
    auto const &rom = *loaded;

    std::printf("File:           %s\n", path.string().c_str());
    std::printf("Size:           %zu bytes (%.2f KiB)\n", rom.size(),
                static_cast<double>(rom.size()) / 1024.0);
    std::printf("CRC32:          0x%08X\n", rom.crc32());

    // Look for ASCII strings — runs of 5+ printable chars are likely calibration
    // IDs, version stamps, copyright strings, or part numbers. We cap output to
    // keep the listing readable on a real 1.5 MB ROM where short coincidences
    // are abundant. min_length=5 already filters most noise.
    auto const strings = rom.scan_ascii(/*min_length=*/5);
    std::printf("Embedded ASCII: %zu strings (>=5 chars)\n", strings.size());

    constexpr std::size_t kMaxToPrint = 64;
    auto const            limit       = strings.size() < kMaxToPrint ? strings.size() : kMaxToPrint;
    for (std::size_t i = 0; i < limit; ++i) {
        auto const &s = strings[i];
        std::printf("  0x%08zX  (%2zu chars)  %s\n", s.offset, s.text.size(), s.text.c_str());
    }
    if (strings.size() > kMaxToPrint) {
        std::printf("  ... %zu more not shown\n", strings.size() - kMaxToPrint);
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

    std::fprintf(stderr, "subarutuner-cli: unknown argument: %s\n", argv[1]);
    std::fprintf(stderr, "Try 'subarutuner-cli --help'.\n");
    return 2;
}
