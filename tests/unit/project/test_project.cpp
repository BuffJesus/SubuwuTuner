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
        path = std::filesystem::temp_directory_path() /
               ("st_project_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(TempDir const &) = delete;
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
    bytes[0] = 'A';
    bytes[1] = 'S';
    bytes[2] = '8';
    bytes[3] = '0';
    bytes[4] = 'U';
    return bytes;
}

} // namespace

TEST_CASE("Project::create writes source.bin + working.bin + project.toml", "[project][create]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto const proj_dir = td.path / "mytune.stune";
    auto p = st::Project::create(proj_dir, rom_path, pack_dir, "My Tune");
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

TEST_CASE("Project::create refuses to populate a non-empty existing dir", "[project][create]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    // Pre-populate target dir.
    auto const proj_dir = td.path / "occupied.stune";
    std::filesystem::create_directories(proj_dir);
    write_text(proj_dir / "stale.txt", "junk");

    auto const p = st::Project::create(proj_dir, rom_path, pack_dir, "x");
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Project::open round-trips through Project::create", "[project][open]") {
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

TEST_CASE("Project::save_working_rom persists in-memory edits to disk", "[project][save]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
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

TEST_CASE("Project::open refuses a non-project directory", "[project][open]") {
    TempDir td;
    auto const p = st::Project::open(td.path);
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error().code() == st::ErrorCode::FileNotFound);
}

TEST_CASE("Project history round-trips through edits.toml", "[project][history]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "h.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "hist");
        REQUIRE(p.has_value());

        // Synthesize two edits by hand. (The CLI normally drives this; for
        // a unit test we forge them so we know exactly what to expect on
        // reload.)
        st::edit::Snapshot before;
        before.rect = {0, 0, 0, 1};
        before.values = {{1.0, 2.0}};
        st::edit::Snapshot after;
        after.rect = {0, 0, 0, 1};
        after.values = {{10.0, 20.0}};

        p->history().record(st::edit::Edit::table("fuel_map", before, after, "set 10/20"));

        st::edit::Snapshot before2;
        before2.rect = {1, 1, 0, 1};
        before2.values = {{5.0, 6.0}};
        st::edit::Snapshot after2;
        after2.rect = {1, 1, 0, 1};
        after2.values = {{50.0, 60.0}};
        p->history().record(st::edit::Edit::table("boost_map", before2, after2, "set 50/60"));

        REQUIRE(p->save_working_rom().has_value());
        REQUIRE(std::filesystem::exists(proj_dir / "edits.toml"));
    }

    // Reopen and confirm history was restored.
    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->history().size() == 2);
    REQUIRE(reopened->history().cursor() == 2);

    auto const &records = reopened->history().records();
    REQUIRE(records.size() == 2);
    {
        auto const *te0 = records[0].as_table();
        REQUIRE(te0 != nullptr);
        REQUIRE(te0->table_id == "fuel_map");
        REQUIRE(records[0].description == "set 10/20");
        REQUIRE(te0->before.values == std::vector<std::vector<double>>{{1.0, 2.0}});
        REQUIRE(te0->after.values == std::vector<std::vector<double>>{{10.0, 20.0}});
    }
    {
        auto const *te1 = records[1].as_table();
        REQUIRE(te1 != nullptr);
        REQUIRE(te1->table_id == "boost_map");
        REQUIRE(te1->after.values == std::vector<std::vector<double>>{{50.0, 60.0}});
        REQUIRE(te1->after.rect.r_start == 1);
        REQUIRE(te1->after.rect.c_end == 1);
    }

    // can_undo / can_redo reflect the restored cursor.
    REQUIRE(reopened->history().can_undo());
    REQUIRE_FALSE(reopened->history().can_redo());
}

TEST_CASE("Project history round-trips a ByteEdit through edits.toml",
          "[project][history][byte_edit]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "byteedit.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "byteedit");
        REQUIRE(p.has_value());

        // Forge a ByteEdit that captures a hypothetical DTC-bit toggle on
        // two adjacent ROM bytes. The Project layer should serialize it
        // through edits.toml without caring about the semantics -- that's
        // the contract.
        p->history().record(st::edit::Edit::bytes({{0x10, 0xFF, 0xFE}, {0x11, 0x00, 0x01}},
                                                  "disable DTC P0420, P0430"));

        REQUIRE(p->save_working_rom().has_value());
        REQUIRE(std::filesystem::exists(proj_dir / "edits.toml"));
    }

    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->history().size() == 1);

    auto const &records = reopened->history().records();
    auto const *be = records[0].as_byte();
    REQUIRE(be != nullptr);
    REQUIRE_FALSE(records[0].is_table());
    REQUIRE(records[0].description == "disable DTC P0420, P0430");
    REQUIRE(be->changes.size() == 2);
    REQUIRE(be->changes[0].address == 0x10);
    REQUIRE(be->changes[0].before == 0xFF);
    REQUIRE(be->changes[0].after == 0xFE);
    REQUIRE(be->changes[1].address == 0x11);
    REQUIRE(be->changes[1].before == 0x00);
    REQUIRE(be->changes[1].after == 0x01);
}

TEST_CASE("Project history round-trips mixed TableEdit + ByteEdit records",
          "[project][history][byte_edit]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "mixed.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "mixed");
        REQUIRE(p.has_value());

        st::edit::Snapshot before;
        before.rect = {0, 0, 0, 1};
        before.values = {{1.0, 2.0}};
        st::edit::Snapshot after;
        after.rect = {0, 0, 0, 1};
        after.values = {{10.0, 20.0}};
        p->history().record(st::edit::Edit::table("fuel_map", before, after, "set 10/20"));

        p->history().record(st::edit::Edit::bytes({{0x10, 0xFF, 0xFE}}, "disable DTC P0420"));

        REQUIRE(p->save_working_rom().has_value());
    }

    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->history().size() == 2);

    auto const &records = reopened->history().records();
    {
        auto const *te = records[0].as_table();
        REQUIRE(te != nullptr);
        REQUIRE(te->table_id == "fuel_map");
        REQUIRE(te->after.values == std::vector<std::vector<double>>{{10.0, 20.0}});
    }
    {
        auto const *be = records[1].as_byte();
        REQUIRE(be != nullptr);
        REQUIRE(be->changes.size() == 1);
        REQUIRE(be->changes[0].address == 0x10);
    }
}

TEST_CASE("Project history loads v1 schema (TableEdit-only) edits.toml unchanged",
          "[project][history][byte_edit][backward_compat]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "legacy.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "legacy");
        REQUIRE(p.has_value());
        REQUIRE(p->save_working_rom().has_value());
    }
    // Hand-write a v1 edits.toml -- pure TableEdit, no schema bump.
    write_text(proj_dir / "edits.toml", R"toml(
schema_version = 1
cursor = 1

[[edit]]
  description = "set 9"
  table_id    = "fuel_map"
  [edit.before]
  r_start = 0
  r_end   = 0
  c_start = 0
  c_end   = 0
  values = [ [ 1.0 ], ]
  [edit.after]
  r_start = 0
  r_end   = 0
  c_start = 0
  c_end   = 0
  values = [ [ 9.0 ], ]
)toml");

    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->history().size() == 1);
    auto const *te = reopened->history().records()[0].as_table();
    REQUIRE(te != nullptr);
    REQUIRE(te->table_id == "fuel_map");
}

TEST_CASE("Project::open refuses edits.toml schema_version above 2", "[project][history][schema]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "futureedits.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "futureedits");
        REQUIRE(p.has_value());
        REQUIRE(p->save_working_rom().has_value());
    }
    write_text(proj_dir / "edits.toml", R"toml(
schema_version = 3
cursor = 0
)toml");

    auto reopened = st::Project::open(proj_dir);
    REQUIRE_FALSE(reopened.has_value());
    REQUIRE(reopened.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("Project history empty on a fresh project until edits land", "[project][history]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto const proj_dir = td.path / "fresh.stune";
    auto p = st::Project::create(proj_dir, rom_path, pack_dir, "fresh");
    REQUIRE(p.has_value());
    REQUIRE(p->history().size() == 0);

    REQUIRE(p->save_working_rom().has_value());
    // No edits ever recorded -> no edits.toml on disk.
    REQUIRE_FALSE(std::filesystem::exists(proj_dir / "edits.toml"));
}

TEST_CASE("Project::open refuses a future schema_version", "[project][open][schema]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
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
    write_bytes(proj_dir / "source.bin", make_rom_bytes());
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

TEST_CASE("Project::open rejects unknown policy_profile string", "[project][policy]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const proj_dir = td.path / "bogus.stune";
    std::filesystem::create_directories(proj_dir);
    write_bytes(proj_dir / "source.bin", make_rom_bytes());
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

TEST_CASE("Project::save_metadata round-trips policy_profile", "[project][policy]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    write_bytes(td.path / "stock.bin", make_rom_bytes());

    auto const proj_dir = td.path / "set_profile.stune";
    auto r = st::Project::create(proj_dir, td.path / "stock.bin", pack_dir, "Set");
    REQUIRE(r.has_value());
    r->set_policy_profile(st::policy::Profile::CaliforniaUs);
    REQUIRE(r->save_metadata().has_value());

    auto const reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->policy_profile() == st::policy::Profile::CaliforniaUs);
}

// --------------------------------------------------------------------------
// parse_edit_csv -- CSV bulk-edit parser. Same code drives the CLI's
// project-edit-csv command and the GUI's Import CSV path; these tests
// cover the format invariants both consumers rely on.
// --------------------------------------------------------------------------

TEST_CASE("parse_edit_csv parses a minimal row,col,value block", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.table_rows = 4;
    opts.table_cols = 4;
    auto r = st::parse_edit_csv("0,0,12.5\n1,2,7.0\n3,3,-1.5\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->warnings.empty());
    REQUIRE(r->cells.size() == 3);
    REQUIRE(r->cells[0].row == 0);
    REQUIRE(r->cells[0].col == 0);
    REQUIRE(r->cells[0].value == 12.5);
    REQUIRE(r->cells[2].value == -1.5);
}

TEST_CASE("parse_edit_csv tolerates a non-numeric header row", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.table_rows = 2;
    opts.table_cols = 2;
    auto r = st::parse_edit_csv("row,col,value\n0,1,3.14\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells.size() == 1);
    REQUIRE(r->cells[0].value == 3.14);
}

TEST_CASE("parse_edit_csv skips blank lines and # comments", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.table_rows = 2;
    opts.table_cols = 2;
    auto r = st::parse_edit_csv("\n# a comment\n  \n0,0,1.0\n# trailing\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells.size() == 1);
    REQUIRE(r->cells[0].value == 1.0);
}

TEST_CASE("parse_edit_csv tolerates CRLF line endings", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.table_rows = 1;
    opts.table_cols = 2;
    auto r = st::parse_edit_csv("0,0,1.0\r\n0,1,2.0\r\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells.size() == 2);
}

TEST_CASE("parse_edit_csv strips inline `#` comment from a data line", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.table_rows = 1;
    opts.table_cols = 1;
    auto r = st::parse_edit_csv("0,0,5.0 # trailing comment\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells.size() == 1);
    REQUIRE(r->cells[0].value == 5.0);
}

TEST_CASE("parse_edit_csv warns on pack_id mismatch but still parses", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.expected_pack_id = "current_pack";
    opts.table_rows = 1;
    opts.table_cols = 1;
    auto r = st::parse_edit_csv("# pack_id = \"some_other_pack\"\n0,0,1.0\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->warnings.size() == 1);
    REQUIRE(r->warnings[0].message.find("some_other_pack") != std::string::npos);
    REQUIRE(r->cells.size() == 1);
}

TEST_CASE("parse_edit_csv does NOT warn when pack_id matches", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.expected_pack_id = "pack_xyz";
    opts.table_rows = 1;
    opts.table_cols = 1;
    auto r = st::parse_edit_csv("# pack_id = \"pack_xyz\"\n0,0,1.0\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->warnings.empty());
}

TEST_CASE("parse_edit_csv refuses on table id mismatch", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.expected_table_id = "fuel_map";
    auto r = st::parse_edit_csv("# table = \"ignition_main\"\n0,0,1.0\n", opts);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(r.error().to_string().find("ignition_main") != std::string::npos);
}

TEST_CASE("parse_edit_csv accepts matching table id without warning", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.expected_table_id = "fuel_map";
    opts.table_rows = 1;
    opts.table_cols = 1;
    auto r = st::parse_edit_csv("# table = \"fuel_map\"\n0,0,1.0\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->warnings.empty());
    REQUIRE(r->cells.size() == 1);
}

TEST_CASE("parse_edit_csv errors on cells outside table bounds", "[project][edit_csv]") {
    st::EditCsvParseOptions opts;
    opts.table_rows = 2;
    opts.table_cols = 2;
    auto r = st::parse_edit_csv("0,0,1.0\n5,5,2.0\n", opts);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("parse_edit_csv skips bounds check when dims are zero", "[project][edit_csv]") {
    // Both dims zero -> bounds disabled. Caller validates separately.
    st::EditCsvParseOptions opts;
    auto r = st::parse_edit_csv("0,0,1.0\n100,200,2.0\n", opts);
    REQUIRE(r.has_value());
    REQUIRE(r->cells.size() == 2);
}

TEST_CASE("parse_edit_csv rejects rows with fewer than 3 fields", "[project][edit_csv]") {
    auto r = st::parse_edit_csv("0,0\n", {});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_edit_csv rejects non-numeric value", "[project][edit_csv]") {
    auto r = st::parse_edit_csv("0,0,abc\n", {});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_edit_csv rejects mid-stream non-numeric row/col", "[project][edit_csv]") {
    // First-line header tolerance only applies before any edit is parsed.
    auto r = st::parse_edit_csv("0,0,1.0\nbad,col,2.0\n", {});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("parse_edit_csv returns empty result for an empty input", "[project][edit_csv]") {
    auto r = st::parse_edit_csv("", {});
    REQUIRE(r.has_value());
    REQUIRE(r->cells.empty());
    REQUIRE(r->warnings.empty());
}

TEST_CASE("parse_edit_csv handles file without trailing newline", "[project][edit_csv]") {
    auto r = st::parse_edit_csv("0,0,1.5", {});
    REQUIRE(r.has_value());
    REQUIRE(r->cells.size() == 1);
    REQUIRE(r->cells[0].value == 1.5);
}

TEST_CASE("Project::handheld_serial defaults to empty on a fresh project",
          "[project][handheld_serial]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());

    auto p = st::Project::create(td.path / "fresh.stune", rom_path, pack_dir, "fresh");
    REQUIRE(p.has_value());
    REQUIRE(p->handheld_serial().empty());
}

TEST_CASE("Project::handheld_serial round-trips through save / reopen",
          "[project][handheld_serial]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "hh.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "hh");
        REQUIRE(p.has_value());
        p->set_handheld_serial("AP3-EXAMPLE-12345");
        REQUIRE(p->save_metadata().has_value());
    }

    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->handheld_serial() == "AP3-EXAMPLE-12345");
}

TEST_CASE("Project::open accepts a legacy project lacking [security_access]",
          "[project][handheld_serial]") {
    // A project written by an older build won't have the [security_access]
    // table at all. The loader must accept that case and leave the serial
    // default-empty, not error out — this is the forward-compat contract
    // documented in docs/21-stune-format.md.
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "legacy.stune";
    std::filesystem::create_directories(proj_dir);

    // Hand-build a v1 project.toml that pre-dates the [security_access]
    // table. Copy the ROM into place to satisfy source/working path checks.
    write_bytes(proj_dir / "source.bin", make_rom_bytes());
    write_bytes(proj_dir / "working.bin", make_rom_bytes());
    auto const legacy_toml = std::string{R"toml(
[project]
schema_version = 1
display_name   = "legacy"
created        = "2026-01-01T00:00:00Z"
notes          = ""
policy_profile = "motorsport-only"

[project.source_rom]
path  = "source.bin"
crc32 = 0

[project.working_rom]
path  = "working.bin"
crc32 = 0

[project.definition]
path = ")toml"} + pack_dir.generic_string() + "\"\n";
    write_text(proj_dir / "project.toml", legacy_toml);

    auto p = st::Project::open(proj_dir);
    REQUIRE(p.has_value());
    REQUIRE(p->handheld_serial().empty());
    REQUIRE(p->display_name() == "legacy");
}

TEST_CASE("Project::additional_roms parses [[rom]] entries from project.toml",
          "[project][open][additional_roms]") {
    // Multi-ROM read slice (analyst Issue #10). A project may declare
    // extra ROMs alongside source/working; Project::open loads them into
    // additional_roms() for the Compare panel to consume. Schema stays
    // at v1 because the array is additive — older loaders ignore.
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const proj_dir = td.path / "multi.stune";
    std::filesystem::create_directories(proj_dir);
    write_bytes(proj_dir / "source.bin", make_rom_bytes());
    write_bytes(proj_dir / "working.bin", make_rom_bytes());
    auto alt_bytes = make_rom_bytes();
    alt_bytes[10] = 0xAA; // make it distinguishable from source
    write_bytes(proj_dir / "alt-tune.bin", alt_bytes);
    auto missing_bytes = make_rom_bytes();
    (void)missing_bytes;
    // Note: "missing-tune.bin" is intentionally NOT written — the
    // parser must drop that entry silently.

    std::string const toml_text = std::string{R"toml(
[project]
schema_version = 1
display_name   = "multi-rom test"

[project.source_rom]
path  = "source.bin"
crc32 = 0

[project.working_rom]
path  = "working.bin"
crc32 = 0

[project.definition]
path = ")toml"} + pack_dir.generic_string() + R"toml("

[[rom]]
id           = "alt-tune"
display_name = "Alternate tune"
path         = "alt-tune.bin"
notes        = "Snapshot pre-autotune"

[[rom]]
id    = "missing-tune"
path  = "missing-tune.bin"

[[rom]]
display_name = "no id, should be skipped"
path         = "alt-tune.bin"
)toml";
    write_text(proj_dir / "project.toml", toml_text);

    auto p = st::Project::open(proj_dir);
    REQUIRE(p.has_value());
    auto const &roms = p->additional_roms();
    // alt-tune loaded; missing-tune dropped (file absent); third entry dropped (no id).
    REQUIRE(roms.size() == 1);
    REQUIRE(roms[0].id == "alt-tune");
    REQUIRE(roms[0].display_name == "Alternate tune");
    REQUIRE(roms[0].notes == "Snapshot pre-autotune");
    REQUIRE(roms[0].rom.size() == 64);
    REQUIRE(roms[0].rom.data()[10] == 0xAA);
}

TEST_CASE("Project::additional_roms round-trips through save_metadata",
          "[project][open][additional_roms]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "rt.stune";

    auto pc = st::Project::create(proj_dir, rom_path, pack_dir, "rt");
    REQUIRE(pc.has_value());

    // Hand-write a [[rom]] entry to project.toml + place the referenced
    // file, then reopen and save_metadata — the round-trip should
    // preserve the entry.
    write_bytes(proj_dir / "extra.bin", make_rom_bytes());
    std::ifstream in{proj_dir / "project.toml"};
    std::ostringstream existing;
    existing << in.rdbuf();
    in.close();
    std::ofstream out{proj_dir / "project.toml"};
    out << existing.str();
    out << R"toml(
[[rom]]
id           = "extra"
display_name = "Extra ROM"
path         = "extra.bin"
)toml";
    out.close();

    auto p = st::Project::open(proj_dir);
    REQUIRE(p.has_value());
    REQUIRE(p->additional_roms().size() == 1);
    REQUIRE(p->save_metadata().has_value());

    auto p2 = st::Project::open(proj_dir);
    REQUIRE(p2.has_value());
    REQUIRE(p2->additional_roms().size() == 1);
    REQUIRE(p2->additional_roms()[0].id == "extra");
    REQUIRE(p2->additional_roms()[0].display_name == "Extra ROM");
}

TEST_CASE("Project::add_additional_rom rejects empty id and duplicates",
          "[project][additional_roms][add]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "add.stune";
    auto pc = st::Project::create(proj_dir, rom_path, pack_dir, "add");
    REQUIRE(pc.has_value());

    st::Project::AdditionalRom entry;
    entry.path_rel = "ignored";
    entry.rom = st::Rom::from_bytes(make_rom_bytes());

    // Empty id is rejected.
    auto const empty_id = pc->add_additional_rom(entry);
    REQUIRE_FALSE(empty_id.has_value());
    REQUIRE(empty_id.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(pc->additional_roms().empty());

    // First add with a real id succeeds.
    entry.id = "tune-a";
    REQUIRE(pc->add_additional_rom(entry).has_value());
    REQUIRE(pc->additional_roms().size() == 1);
    REQUIRE(pc->additional_roms()[0].id == "tune-a");
    // display_name defaulted from id.
    REQUIRE(pc->additional_roms()[0].display_name == "tune-a");
    // crc32 backfilled from the rom bytes.
    REQUIRE(pc->additional_roms()[0].crc32 != 0);

    // Same id rejected.
    st::Project::AdditionalRom dup;
    dup.id = "tune-a";
    dup.path_rel = "other";
    dup.rom = st::Rom::from_bytes(make_rom_bytes());
    auto const dup_r = pc->add_additional_rom(dup);
    REQUIRE_FALSE(dup_r.has_value());
    REQUIRE(dup_r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(pc->additional_roms().size() == 1); // unchanged

    // Reserved ids — "source" and "working" would shadow the built-in
    // slots that find_rom_by_id privileges. Both must be rejected.
    for (auto const *reserved : {"source", "working"}) {
        st::Project::AdditionalRom r;
        r.id = reserved;
        r.path_rel = "ignored";
        r.rom = st::Rom::from_bytes(make_rom_bytes());
        auto const got = pc->add_additional_rom(r);
        REQUIRE_FALSE(got.has_value());
        REQUIRE(got.error().code() == st::ErrorCode::InvalidArgument);
    }
    REQUIRE(pc->additional_roms().size() == 1); // still unchanged
}

TEST_CASE("Project::find_rom_by_id resolves built-in and additional slots",
          "[project][active_rom][find]") {
    // The read-side primitive that every CLI --rom and (eventually) GUI
    // active-ROM switch funnels through. "" and "working" both alias
    // to the working slot so existing code paths keep working. "source"
    // hits the immutable copy. An additional id hits its loaded Rom.
    // An unknown id returns nullptr so callers can produce a clean
    // error message instead of crashing.
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "find.stune";
    auto p = st::Project::create(proj_dir, rom_path, pack_dir, "find");
    REQUIRE(p.has_value());

    REQUIRE(p->find_rom_by_id("") == &p->working_rom());
    REQUIRE(p->find_rom_by_id("working") == &p->working_rom());
    REQUIRE(p->find_rom_by_id("source") == &p->source_rom());
    REQUIRE(p->find_rom_by_id("nope") == nullptr);

    st::Project::AdditionalRom entry;
    entry.id = "tune-a";
    entry.path_rel = "ignored";
    entry.rom = st::Rom::from_bytes(make_rom_bytes());
    REQUIRE(p->add_additional_rom(entry).has_value());

    auto const *r = p->find_rom_by_id("tune-a");
    REQUIRE(r != nullptr);
    REQUIRE(r->size() == 64);
}

TEST_CASE("Project::set_active_rom_id validates against built-ins and additional ids",
          "[project][active_rom][set]") {
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "set.stune";
    auto p = st::Project::create(proj_dir, rom_path, pack_dir, "set");
    REQUIRE(p.has_value());

    // Default is the empty string = working slot.
    REQUIRE(p->active_rom_id().empty());

    REQUIRE(p->set_active_rom_id("working").has_value());
    REQUIRE(p->active_rom_id() == "working");
    REQUIRE(p->set_active_rom_id("source").has_value());
    REQUIRE(p->active_rom_id() == "source");
    REQUIRE(p->set_active_rom_id("").has_value());
    REQUIRE(p->active_rom_id().empty());

    auto const bad = p->set_active_rom_id("never-added");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().code() == st::ErrorCode::InvalidArgument);
    // Failed set must NOT mutate the persisted id.
    REQUIRE(p->active_rom_id().empty());

    st::Project::AdditionalRom entry;
    entry.id = "tune-b";
    entry.path_rel = "ignored";
    entry.rom = st::Rom::from_bytes(make_rom_bytes());
    REQUIRE(p->add_additional_rom(entry).has_value());
    REQUIRE(p->set_active_rom_id("tune-b").has_value());
    REQUIRE(p->active_rom_id() == "tune-b");
}

TEST_CASE("Project::active_rom_id round-trips through save_metadata + open",
          "[project][active_rom][persist]") {
    // The setter only mutates in-memory state; save_metadata persists
    // it to project.toml, and Project::open restores it. A project
    // that never set the field reopens with an empty id (the working
    // default), matching the v1 behavior.
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "persist.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "persist");
        REQUIRE(p.has_value());
        REQUIRE(p->set_active_rom_id("source").has_value());
        REQUIRE(p->save_metadata().has_value());
    }
    {
        auto p = st::Project::open(proj_dir);
        REQUIRE(p.has_value());
        REQUIRE(p->active_rom_id() == "source");
    }
    {
        // Reset back to default; round-trip should bring back an
        // empty id, not "working".
        auto p = st::Project::open(proj_dir);
        REQUIRE(p.has_value());
        REQUIRE(p->set_active_rom_id("").has_value());
        REQUIRE(p->save_metadata().has_value());
    }
    {
        auto p = st::Project::open(proj_dir);
        REQUIRE(p.has_value());
        REQUIRE(p->active_rom_id().empty());
    }
}

TEST_CASE("Project::active_rom_id with stale additional id is tolerated on open",
          "[project][active_rom][persist]") {
    // If a user removes a [[rom]] entry from project.toml while the
    // active_rom_id still names it, open() must succeed (the
    // working-slot fallback in find_rom_by_id handles the read-side
    // case). The id stays in the field so the user can see it was
    // there — they can run project-set-active-rom to reset.
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const proj_dir = td.path / "stale.stune";
    std::filesystem::create_directories(proj_dir);
    write_bytes(proj_dir / "source.bin", make_rom_bytes());
    write_bytes(proj_dir / "working.bin", make_rom_bytes());

    std::string const toml_text = std::string{R"toml(
[project]
schema_version = 1
display_name   = "stale"
active_rom_id  = "ghost"

[project.source_rom]
path  = "source.bin"
crc32 = 0

[project.working_rom]
path  = "working.bin"
crc32 = 0

[project.definition]
path = ")toml"} + pack_dir.generic_string() + R"toml("
)toml";
    write_text(proj_dir / "project.toml", toml_text);

    auto p = st::Project::open(proj_dir);
    REQUIRE(p.has_value());
    REQUIRE(p->active_rom_id() == "ghost");
    REQUIRE(p->find_rom_by_id(p->active_rom_id()) == nullptr);
}

TEST_CASE("Project::handheld_serial preserves empty on default save / reopen",
          "[project][handheld_serial]") {
    // A project that never sets a handheld serial must round-trip with the
    // field still empty (i.e. the [security_access] table being present
    // with an empty value is equivalent to the table being absent).
    TempDir td;
    auto const pack_dir = make_pack(td.path / "pack");
    auto const rom_path = td.path / "stock.bin";
    write_bytes(rom_path, make_rom_bytes());
    auto const proj_dir = td.path / "empty.stune";

    {
        auto p = st::Project::create(proj_dir, rom_path, pack_dir, "empty");
        REQUIRE(p.has_value());
    }

    auto reopened = st::Project::open(proj_dir);
    REQUIRE(reopened.has_value());
    REQUIRE(reopened->handheld_serial().empty());
}
