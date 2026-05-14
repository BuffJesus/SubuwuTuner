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

TEST_CASE("Project history round-trips through edits.toml",
          "[project][history]") {
    TempDir td;
    auto const  pack_dir = make_pack(td.path / "pack");
    auto const  rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "h.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "hist");
        REQUIRE(p.has_value());

        // Synthesize two edits by hand. (The CLI normally drives this; for
        // a unit test we forge them so we know exactly what to expect on
        // reload.)
        st::edit::Snapshot before;
        before.rect   = {0, 0, 0, 1};
        before.values = {{1.0, 2.0}};
        st::edit::Snapshot after;
        after.rect   = {0, 0, 0, 1};
        after.values = {{10.0, 20.0}};

        p->history().record({"fuel_map", before, after, "set 10/20"});

        st::edit::Snapshot before2;
        before2.rect   = {1, 1, 0, 1};
        before2.values = {{5.0, 6.0}};
        st::edit::Snapshot after2;
        after2.rect   = {1, 1, 0, 1};
        after2.values = {{50.0, 60.0}};
        p->history().record({"boost_map", before2, after2, "set 50/60"});

        REQUIRE(p->save_working_rom().has_value());
        REQUIRE(std::filesystem::exists(proj_dir / "edits.toml"));
    }

    // Reopen and confirm history was restored.
    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->history().size() == 2);
    REQUIRE(reopened->history().cursor() == 2);

    auto const &records = reopened->history().records();
    REQUIRE(records[0].table_id == "fuel_map");
    REQUIRE(records[0].description == "set 10/20");
    REQUIRE(records[0].before.values == std::vector<std::vector<double>>{{1.0, 2.0}});
    REQUIRE(records[0].after.values == std::vector<std::vector<double>>{{10.0, 20.0}});

    REQUIRE(records[1].table_id == "boost_map");
    REQUIRE(records[1].after.values == std::vector<std::vector<double>>{{50.0, 60.0}});
    REQUIRE(records[1].after.rect.r_start == 1);
    REQUIRE(records[1].after.rect.c_end == 1);

    // can_undo / can_redo reflect the restored cursor.
    REQUIRE(reopened->history().can_undo());
    REQUIRE_FALSE(reopened->history().can_redo());
}

TEST_CASE("Project history empty on a fresh project until edits land",
          "[project][history]") {
    TempDir td;
    auto const  pack_dir = make_pack(td.path / "pack");
    auto const  rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto const proj_dir = td.path / "fresh.stune";
    auto       p        = st::Project::create(proj_dir, rom_path, pack_dir, "fresh");
    REQUIRE(p.has_value());
    REQUIRE(p->history().size() == 0);

    REQUIRE(p->save_working_rom().has_value());
    // No edits ever recorded -> no edits.toml on disk.
    REQUIRE_FALSE(std::filesystem::exists(proj_dir / "edits.toml"));
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


// ---- policy_profile ------------------------------------------------------

TEST_CASE("Project defaults policy_profile to MotorsportOnly", "[project][policy]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto const proj_dir = td.path / "fresh.stune";
    auto const r = st::Project::create(proj_dir, rom_path, pack_dir, "Fresh");
    REQUIRE(r.has_value());
    REQUIRE(r->policy_profile() == st::policy::Profile::MotorsportOnly);
}

TEST_CASE("Project::open reads policy_profile from project.toml", "[project][policy]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    write_bytes(td.path / "stock.bin", make_rom_bytes());

    auto const proj_dir = td.path / "with_profile.stune";
    std::filesystem::create_directories(proj_dir);
    write_bytes(proj_dir / "source.bin",  make_rom_bytes());
    write_bytes(proj_dir / "working.bin", make_rom_bytes());
    write_text(proj_dir / "project.toml", R"toml(
[project]
schema_version  = 1
display_name    = "Roadworthy build"
created         = ""
notes           = ""
policy_profile  = "eu-roadworthy"

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
    REQUIRE(p.has_value());
    REQUIRE(p->policy_profile() == st::policy::Profile::EuRoadworthy);
}

TEST_CASE("Project::open rejects unknown policy_profile string",
          "[project][policy]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const proj_dir = td.path / "bogus.stune";
    std::filesystem::create_directories(proj_dir);
    write_bytes(proj_dir / "source.bin",  make_rom_bytes());
    write_bytes(proj_dir / "working.bin", make_rom_bytes());
    write_text(proj_dir / "project.toml", R"toml(
[project]
schema_version  = 1
display_name    = ""
created         = ""
notes           = ""
policy_profile  = "freeworld"

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
    REQUIRE(p.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Project::save_metadata round-trips policy_profile",
          "[project][policy]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    write_bytes(td.path / "stock.bin", make_rom_bytes());

    auto const proj_dir = td.path / "set_profile.stune";
    auto       r = st::Project::create(proj_dir, td.path / "stock.bin",
                                        pack_dir, "Set");
    REQUIRE(r.has_value());
    r->set_policy_profile(st::policy::Profile::CaliforniaUs);
    REQUIRE(r->save_metadata().has_value());

    auto const reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->policy_profile() == st::policy::Profile::CaliforniaUs);
}
