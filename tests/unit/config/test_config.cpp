// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for st::config — the Phase 2 runtime config layer.
// Spec: docs/25-config-system.md.

#include "st/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace cfg = st::config;

namespace {

struct ScopedTempDir {
    std::filesystem::path path;

    ScopedTempDir() {
        auto base = std::filesystem::temp_directory_path() / "subuwu_config_test";
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        auto unique = base;
        for (std::size_t i = 0;; ++i) {
            auto const seed = reinterpret_cast<std::uintptr_t>(this) + i;
            unique = base / std::to_string(seed);
            if (std::filesystem::create_directories(unique, ec)) {
                break;
            }
            if (i > 100) {
                throw std::runtime_error("ScopedTempDir: too many collisions");
            }
        }
        path = unique;
    }
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ScopedTempDir(ScopedTempDir const &) = delete;
    ScopedTempDir &operator=(ScopedTempDir const &) = delete;
};

void write_file(std::filesystem::path const &p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

TEST_CASE("Config::defaults populates non-empty paths", "[config]") {
    cfg::reset_for_testing();
    auto c = cfg::Config::defaults();
    REQUIRE(!c.paths().definitions_root.empty());
    REQUIRE(!c.paths().rom_dump_root.empty());
    REQUIRE(!c.source_path().empty());
}

TEST_CASE("default_definitions_root resolves to <exe-dir>/definitions",
          "[config]") {
    cfg::reset_for_testing();
    auto const p = cfg::default_definitions_root();
    REQUIRE(p.filename().string() == "definitions");
    REQUIRE(!p.parent_path().empty());
}

TEST_CASE("load_from(nonexistent) returns defaults silently", "[config]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "missing.toml";
    auto r = cfg::Config::load_from(cfg_path);
    REQUIRE(r.has_value());
    REQUIRE(r->source_path() == cfg_path);
    // Defaults flowed through.
    auto const defs = cfg::Config::defaults();
    REQUIRE(r->paths().definitions_root == defs.paths().definitions_root);
}

TEST_CASE("load_from(corrupt) returns ParseError", "[config][error]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "corrupt.toml";
    write_file(cfg_path,
               "[paths]\n"
               "definitions_root = no_quotes_here\n");
    auto r = cfg::Config::load_from(cfg_path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("load_from + save round-trip", "[config]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "rt.toml";

    cfg::Config a = cfg::Config::defaults();
    a.set_source_path(cfg_path);
    a.paths().definitions_root = "C:/somewhere/defs";
    a.paths().rom_dump_root = "C:/elsewhere/roms";
    auto s = a.save();
    REQUIRE(s.has_value());
    REQUIRE(std::filesystem::exists(cfg_path));

    auto b_r = cfg::Config::load_from(cfg_path);
    REQUIRE(b_r.has_value());
    REQUIRE(b_r->paths().definitions_root ==
            std::filesystem::path{"C:/somewhere/defs"});
    REQUIRE(b_r->paths().rom_dump_root ==
            std::filesystem::path{"C:/elsewhere/roms"});
    REQUIRE(b_r->source_path() == cfg_path);
}

TEST_CASE("load_from honors partial config (missing keys -> defaults)",
          "[config]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "partial.toml";
    write_file(cfg_path,
               "[paths]\n"
               "definitions_root = \"D:/explicit\"\n");
    auto r = cfg::Config::load_from(cfg_path);
    REQUIRE(r.has_value());
    REQUIRE(r->paths().definitions_root == std::filesystem::path{"D:/explicit"});
    // rom_dump_root not in file -> default-shaped.
    auto const defs = cfg::Config::defaults();
    REQUIRE(r->paths().rom_dump_root == defs.paths().rom_dump_root);
}

TEST_CASE("load_from skips comments + unknown keys + unknown sections",
          "[config]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "noisy.toml";
    write_file(cfg_path,
               "# top comment\n"
               "\n"
               "[paths]\n"
               "# inline comment-only line\n"
               "definitions_root = \"D:/somewhere\"  # trailing comment\n"
               "future_key       = \"ignored\"\n"
               "\n"
               "[unknown_section]\n"
               "ignored_key = \"value\"\n");
    auto r = cfg::Config::load_from(cfg_path);
    REQUIRE(r.has_value());
    REQUIRE(r->paths().definitions_root ==
            std::filesystem::path{"D:/somewhere"});
}

TEST_CASE("save: creates parent directory if missing", "[config]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "deeply" / "nested" / "dir" / "cfg.toml";
    REQUIRE_FALSE(std::filesystem::exists(cfg_path.parent_path()));

    cfg::Config c = cfg::Config::defaults();
    c.set_source_path(cfg_path);
    c.paths().definitions_root = "X:/whatever";
    REQUIRE(c.save().has_value());
    REQUIRE(std::filesystem::exists(cfg_path));
}

TEST_CASE("override_definitions_root wins over config + env",
          "[config][override]") {
    cfg::reset_for_testing();
    cfg::override_definitions_root("Z:/explicit/override");
    REQUIRE(cfg::definitions_root() ==
            std::filesystem::path{"Z:/explicit/override"});
    cfg::reset_for_testing();
}

TEST_CASE("override_rom_dump_root wins over config + env",
          "[config][override]") {
    cfg::reset_for_testing();
    cfg::override_rom_dump_root("Z:/explicit/roms");
    REQUIRE(cfg::rom_dump_root() == std::filesystem::path{"Z:/explicit/roms"});
    cfg::reset_for_testing();
}

TEST_CASE("save() escapes backslashes / quotes correctly for Windows paths",
          "[config]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "winpaths.toml";

    cfg::Config a = cfg::Config::defaults();
    a.set_source_path(cfg_path);
    a.paths().definitions_root =
        std::filesystem::path{"C:\\Users\\Cornelio\\Defs"};
    a.paths().rom_dump_root =
        std::filesystem::path{"C:\\Users\\Cornelio\\Roms"};
    REQUIRE(a.save().has_value());

    auto b = cfg::Config::load_from(cfg_path);
    REQUIRE(b.has_value());
    // filesystem::path normalizes to native form on each platform; compare
    // via path equality rather than string equality.
    REQUIRE(b->paths().definitions_root ==
            std::filesystem::path{"C:\\Users\\Cornelio\\Defs"});
    REQUIRE(b->paths().rom_dump_root ==
            std::filesystem::path{"C:\\Users\\Cornelio\\Roms"});
}

TEST_CASE("save() rejects empty source_path", "[config][error]") {
    cfg::reset_for_testing();
    cfg::Config c;
    c.paths().definitions_root = "X:/d";
    auto s = c.save();
    REQUIRE_FALSE(s.has_value());
    REQUIRE(s.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("parse rejects malformed lines with line number",
          "[config][error]") {
    cfg::reset_for_testing();
    ScopedTempDir tmp;
    auto const cfg_path = tmp.path / "garbled.toml";
    write_file(cfg_path,
               "[paths]\n"
               "definitions_root = \"ok\"\n"
               "this line has no equals\n");
    auto r = cfg::Config::load_from(cfg_path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
    // Line 3 is the malformed one.
    REQUIRE(std::string{r.error().message()}.find(":3:") != std::string::npos);
}
