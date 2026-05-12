// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <vector>

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path()
             / ("st_project_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(TempDir const &)            = delete;
    TempDir &operator=(TempDir const &) = delete;
};

void write_bytes(std::filesystem::path const &p, std::vector<std::uint8_t> const &bytes) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary};
    out.write(reinterpret_cast<char const *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void write_text(std::filesystem::path const &p, std::string_view text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{p};
    out << text;
}

// Build a minimal usable pack on disk and return its path. The pack matches
// the synthetic ROM created by make_rom() below.
std::filesystem::path make_pack(std::filesystem::path const &dir) {
    write_text(dir / "pack.toml", R"toml(
[pack]
schema_version = 1
id             = "test-pack"
endianness     = "big"
rom_size_bytes = 64

[[identification]]
name        = "AS80U fixture"
cid_address = 0
cid_length  = 5
cid_match   = "AS80U"
)toml");
    return dir;
}

std::vector<std::uint8_t> make_rom_bytes() {
    std::vector<std::uint8_t> bytes(64, 0xFF);
    // CID at offset 0 = "AS80U"
    bytes[0] = 'A'; bytes[1] = 'S'; bytes[2] = '8'; bytes[3] = '0'; bytes[4] = 'U';
    return bytes;
}

} // namespace

TEST_CASE("Project::create writes source.bin + working.bin + project.toml",
          "[project][create]") {
    TempDir td;
    auto const  pack_dir = make_pack(td.path / "pack");
    auto const  rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto const proj_dir = td.path / "mytune.stune";
    auto       p        = st::Project::create(proj_dir, rom_path, pack_dir, "My Tune");
    REQUIRE(p.has_value());

    REQUIRE(std::filesystem::exists(proj_dir / "project.toml"));
    REQUIRE(std::filesystem::exists(proj_dir / "source.bin"));
    REQUIRE(std::filesystem::exists(proj_dir / "working.bin"));

    // source and working start identical
    REQUIRE(p->source_rom().size() == 64);
    REQUIRE(p->working_rom().size() == 64);
    REQUIRE(p->source_rom().crc32() == p->working_rom().crc32());
    REQUIRE(p->source_crc32_at_create() == p->source_rom().crc32());
    REQUIRE(p->display_name() == "My Tune");

    // The definition pack was loaded and matched against the source ROM.
    REQUIRE(p->definition().pack().id == "test-pack");
}

TEST_CASE("Project::create refuses to populate a non-empty existing dir",
          "[project][create]") {
    TempDir td;
    auto const  pack_dir = make_pack(td.path / "pack");
    auto const  rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    // Pre-populate target dir.
    auto const proj_dir = td.path / "occupied.stune";
    std::filesystem::create_directories(proj_dir);
    write_text(proj_dir / "stale.txt", "junk");

    auto const p = st::Project::create(proj_dir, rom_path, pack_dir, "x");
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Project::open round-trips through Project::create",
          "[project][open]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "rt.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "Round-trip");
        REQUIRE(p.has_value());
    }

    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->display_name() == "Round-trip");
    REQUIRE(reopened->source_rom().size() == 64);
    REQUIRE(reopened->working_rom().size() == 64);
    REQUIRE(reopened->definition().pack().id == "test-pack");
}

TEST_CASE("Project::save_working_rom persists in-memory edits to disk",
          "[project][save]") {
    TempDir td;
    auto const  pack_dir = make_pack(td.path / "pack");
    auto const  rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "save.stune";

    auto p = st::Project::create(proj_dir, rom_path, pack_dir, "x");
    REQUIRE(p.has_value());

    // Mutate working.bin in memory.
    auto const w = p->working_rom().write_u8(10, 0xAA);
    REQUIRE(w.has_value());
    auto const expected_crc = p->working_rom().crc32();

    REQUIRE(p->save_working_rom().has_value());

    // Re-open and confirm the change persisted.
    auto p2 = st::Project::open(proj_dir);
    REQUIRE(p2.has_value());
    REQUIRE(p2->working_rom().data()[10] == 0xAA);
    REQUIRE(p2->working_rom().crc32() == expected_crc);
    // Source remains untouched.
    REQUIRE(p2->source_rom().data()[10] == 0xFF);
}

TEST_CASE("Project::open refuses a non-project directory",
          "[project][open]") {
    TempDir td;
    auto const p = st::Project::open(td.path);
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("Project::open refuses a future schema_version",
          "[project][open][schema]") {
    TempDir td;
    auto const  pack_dir = make_pack(td.path / "pack");
    auto const  rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto const proj_dir = td.path / "future.stune";
    std::filesystem::create_directories(proj_dir);
    write_bytes(proj_dir / "source.bin", make_rom_bytes());
    write_bytes(proj_dir / "working.bin", make_rom_bytes());

    write_text(proj_dir / "project.toml", R"toml(
[project]
schema_version = 999
display_name   = "From the future"
created        = ""
notes          = ""

[project.source_rom]
path  = "source.bin"
crc32 = 0

[project.working_rom]
path  = "working.bin"
crc32 = 0

[project.definition]
path = "../pack"
)toml");

    auto const p = st::Project::open(proj_dir);
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error().code() == st::ErrorCode::UnsupportedVersion);
}
