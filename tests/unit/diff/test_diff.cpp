// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/diff.hpp"

#include "st/defs.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Use the demo project as the test bench — its loader walks the
// multi-file pack at fixtures/demo-pack/ (pack.toml + tables/*.toml)
// so we get a Definition with tables populated, plus a real ROM that
// matches the pack's expected size. The raw Definition::from_file
// on pack.toml alone returns an empty-tables Definition because
// the table subdir isn't followed without the project loader.
struct DemoBench {
    st::Project project;
    st::Definition def; // copy snapshot — Project::definition() returns ref
    st::Rom rom;        // copy of source_rom for mutation in tests
};

DemoBench load_demo_bench() {
    auto p = st::Project::open("fixtures/demo.stune");
    REQUIRE(p.has_value());
    // Copy the definition + source ROM so we can mutate the rom
    // independently per test.
    st::Definition def_copy = p->definition();
    st::Rom rom_copy = p->source_rom();
    return {std::move(*p), std::move(def_copy), std::move(rom_copy)};
}

} // namespace

TEST_CASE("diff::compare identical ROMs reports zero changes",
          "[diff][identical]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;

    auto r = st::diff::compare(a, a, bench.def);
    REQUIRE(r.has_value());
    REQUIRE(r->identical());
    REQUIRE(r->tables_changed == 0);
    REQUIRE(r->total_cells_changed == 0);
    REQUIRE(r->tables.empty()); // include_identical default false
    REQUIRE(r->pack_id == "demo-va-wrx-mt");
    REQUIRE(r->rom_a_crc32 == a.crc32());
    REQUIRE(r->rom_b_crc32 == a.crc32());
}

TEST_CASE("diff::compare include_identical surfaces every compared table",
          "[diff][options]") {
    auto bench = load_demo_bench();

    st::diff::Options opts;
    opts.include_identical = true;
    auto r = st::diff::compare(bench.rom, bench.rom, bench.def, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->tables_compared > 0);
    REQUIRE(r->tables.size() == r->tables_compared);
    for (auto const &t : r->tables) {
        REQUIRE_FALSE(t.changed());
    }
}

TEST_CASE("diff::compare detects single-cell change",
          "[diff][cell-change]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;
    auto b = bench.rom;
    REQUIRE(bench.def.tables().size() >= 1);
    auto const first_addr = bench.def.tables().front().address;
    REQUIRE(b.size() > first_addr);
    // Flip the high byte of the first cell — uint16_be cells get a
    // big scaled delta from a high-byte flip.
    auto data = b.data_mut();
    data[first_addr] = static_cast<std::uint8_t>(data[first_addr] ^ 0xFFU);

    auto r = st::diff::compare(a, b, bench.def);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->identical());
    REQUIRE(r->tables_changed >= 1);
    REQUIRE(r->total_cells_changed >= 1);

    bool found = false;
    for (auto const &t : r->tables) {
        if (!t.changed())
            continue;
        for (auto const &c : t.changes) {
            if (c.row == 0 && c.col == 0 && c.value_a != c.value_b) {
                found = true;
                break;
            }
        }
        if (found)
            break;
    }
    REQUIRE(found);
}

TEST_CASE("diff::compare include_cell_list false suppresses per-cell entries",
          "[diff][options]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;
    auto b = bench.rom;
    auto data = b.data_mut();
    auto const first_addr = bench.def.tables().front().address;
    data[first_addr] ^= 0xFFU;

    st::diff::Options opts;
    opts.include_cell_list = false;
    auto r = st::diff::compare(a, b, bench.def, opts);
    REQUIRE(r.has_value());
    REQUIRE(r->tables_changed >= 1);
    for (auto const &t : r->tables) {
        REQUIRE(t.changes.empty());
        if (t.changed()) {
            REQUIRE(t.cells_changed > 0);
            REQUIRE(t.max_abs_delta > 0.0);
        }
    }
}

TEST_CASE("diff::compare epsilon absorbs sub-threshold changes",
          "[diff][epsilon]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;
    auto b = bench.rom;
    auto data = b.data_mut();
    auto const first_addr = bench.def.tables().front().address;
    data[first_addr + 1] ^= 0x01U;

    auto strict = st::diff::compare(a, b, bench.def);
    REQUIRE(strict.has_value());
    auto const changed_under_strict = strict->total_cells_changed;

    st::diff::Options opts;
    opts.cell_epsilon = 1.0e6;
    auto loose = st::diff::compare(a, b, bench.def, opts);
    REQUIRE(loose.has_value());
    REQUIRE(loose->total_cells_changed == 0);
    REQUIRE(loose->identical());
    if (changed_under_strict == 0) {
        WARN("LSB flip produced no scaled delta — epsilon test is "
             "vacuous on this pack/table combo");
    }
}

TEST_CASE("diff::compare rejects empty definition",
          "[diff][error]") {
    constexpr char const *empty_pack = R"toml(
[pack]
id = "empty"
display_name = "empty"
)toml";

    auto const def = st::Definition::from_toml_string(empty_pack);
    REQUIRE(def.has_value());
    auto bench = load_demo_bench();

    auto r = st::diff::compare(bench.rom, bench.rom, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("render_text on identical ROMs reports 'Identical'",
          "[diff][render][text]") {
    auto bench = load_demo_bench();
    auto r = st::diff::compare(bench.rom, bench.rom, bench.def);
    REQUIRE(r.has_value());
    auto const text = st::diff::render_text(*r);
    REQUIRE(text.find("Identical") != std::string::npos);
    REQUIRE(text.find("demo-va-wrx-mt") != std::string::npos);
}

TEST_CASE("render_text on changes lists table id + delta stats",
          "[diff][render][text]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;
    auto b = bench.rom;
    auto data = b.data_mut();
    data[bench.def.tables().front().address] ^= 0xFFU;

    auto r = st::diff::compare(a, b, bench.def);
    REQUIRE(r.has_value());
    auto const text = st::diff::render_text(*r);
    REQUIRE(text.find("Changed tables") != std::string::npos);
    REQUIRE(text.find(bench.def.tables().front().id) != std::string::npos);
    REQUIRE(text.find("cells changed") != std::string::npos);
}

TEST_CASE("render_csv emits a header + one row per changed cell",
          "[diff][render][csv]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;
    auto b = bench.rom;
    auto data = b.data_mut();
    data[bench.def.tables().front().address] ^= 0xFFU;

    auto r = st::diff::compare(a, b, bench.def);
    REQUIRE(r.has_value());
    auto const csv = st::diff::render_csv(*r);
    REQUIRE(csv.starts_with("table_id,table_name,row,col,value_a,value_b,delta"));
    auto const newline_count = std::count(csv.begin(), csv.end(), '\n');
    REQUIRE(newline_count >= 2);
}

TEST_CASE("render_json emits subuwutuner.diff.v1 with summary block",
          "[diff][render][json]") {
    auto bench = load_demo_bench();
    auto const r = st::diff::compare(bench.rom, bench.rom, bench.def);
    REQUIRE(r.has_value());
    auto const json = st::diff::render_json(*r);
    REQUIRE(json.find("\"schema\":\"subuwutuner.diff.v1\"") != std::string::npos);
    REQUIRE(json.find("\"pack_id\":\"demo-va-wrx-mt\"") != std::string::npos);
    REQUIRE(json.find("\"summary\":") != std::string::npos);
    REQUIRE(json.find("\"identical\":true") != std::string::npos);
}

TEST_CASE("render_json on cell-changed compare populates the changes array",
          "[diff][render][json]") {
    auto bench = load_demo_bench();
    auto const &a = bench.rom;
    auto b = bench.rom;
    auto data = b.data_mut();
    data[bench.def.tables().front().address] ^= 0xFFU;

    auto const r = st::diff::compare(a, b, bench.def);
    REQUIRE(r.has_value());
    auto const json = st::diff::render_json(*r);
    REQUIRE(json.find("\"identical\":false") != std::string::npos);
    REQUIRE(json.find("\"changes\":[") != std::string::npos);
    REQUIRE(json.find("\"row\":0") != std::string::npos);
    REQUIRE(json.find("\"delta\":") != std::string::npos);
}

// ---- CompareSession persistence (.stcompare) --------------------------

#include <cstdlib>

namespace {

// Build a populated CompareSession against the demo bench. The
// session points at the demo project's source.bin twice (identical
// compare) so verify_compare_inputs passes against the on-disk
// fixture without further setup.
st::diff::CompareSession make_demo_session(std::string const &abs_source_path,
                                           std::uint32_t crc,
                                           std::size_t size) {
    st::diff::CompareSession s;
    s.rom_a_path = abs_source_path;
    s.rom_a_crc32 = crc;
    s.rom_a_size = size;
    s.rom_b_path = abs_source_path;
    s.rom_b_crc32 = crc;
    s.rom_b_size = size;
    s.pack_id = "demo-va-wrx-mt";
    s.options.cell_epsilon = 0.5;
    s.options.include_identical = true;
    s.options.include_cell_list = true;
    s.created_at = "2026-06-01T20:00:00Z";
    s.annotations.push_back({"primary_open_loop_fuel", 0, 0,
                             "Stock 14.7:1; bump on Stage 1?"});
    s.annotations.push_back(
        {"primary_open_loop_fuel", 3, 5, "Boost cell — keep rich for safety."});
    return s;
}

std::filesystem::path temp_stcompare_path(char const *stem) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string{"subuwutuner_test_"} + stem + ".stcompare");
    std::filesystem::remove(p);
    return p;
}

} // namespace

TEST_CASE("CompareSession round-trip preserves all fields",
          "[diff][stcompare][round-trip]") {
    auto bench = load_demo_bench();
    auto const abs = std::filesystem::absolute("fixtures/demo.stune/source.bin");

    auto const original = make_demo_session(abs.string(), bench.rom.crc32(),
                                            bench.rom.size());
    auto const path = temp_stcompare_path("roundtrip");
    REQUIRE(st::diff::save_compare_session(original, path).has_value());

    auto const loaded = st::diff::load_compare_session(path);
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->schema_version == original.schema_version);
    REQUIRE(loaded->pack_id == original.pack_id);
    REQUIRE(loaded->created_at == original.created_at);
    REQUIRE(loaded->rom_a_path == original.rom_a_path);
    REQUIRE(loaded->rom_a_crc32 == original.rom_a_crc32);
    REQUIRE(loaded->rom_a_size == original.rom_a_size);
    REQUIRE(loaded->rom_b_path == original.rom_b_path);
    REQUIRE(loaded->rom_b_crc32 == original.rom_b_crc32);
    REQUIRE(loaded->rom_b_size == original.rom_b_size);
    REQUIRE(loaded->options.cell_epsilon == original.options.cell_epsilon);
    REQUIRE(loaded->options.include_identical == original.options.include_identical);
    REQUIRE(loaded->options.include_cell_list == original.options.include_cell_list);
    REQUIRE(loaded->annotations.size() == 2);
    REQUIRE(loaded->annotations[0].table_id == "primary_open_loop_fuel");
    REQUIRE(loaded->annotations[0].row == 0);
    REQUIRE(loaded->annotations[0].col == 0);
    REQUIRE(loaded->annotations[1].text == "Boost cell — keep rich for safety.");

    std::filesystem::remove(path);
}

TEST_CASE("verify_compare_inputs accepts unchanged inputs",
          "[diff][stcompare][verify]") {
    auto bench = load_demo_bench();
    auto const abs = std::filesystem::absolute("fixtures/demo.stune/source.bin");
    auto const session = make_demo_session(abs.string(), bench.rom.crc32(),
                                           bench.rom.size());

    auto const r = st::diff::verify_compare_inputs(session, std::filesystem::current_path());
    INFO("verify error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
}

TEST_CASE("verify_compare_inputs rejects CRC32 drift",
          "[diff][stcompare][verify][error]") {
    auto bench = load_demo_bench();
    auto const abs = std::filesystem::absolute("fixtures/demo.stune/source.bin");
    // Synthetic CRC mismatch — same size, wrong CRC.
    auto session = make_demo_session(abs.string(), bench.rom.crc32() ^ 0xFFFFFFFFU,
                                     bench.rom.size());

    auto const r = st::diff::verify_compare_inputs(session, std::filesystem::current_path());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(r.error().to_string().find("CRC32 mismatch") != std::string::npos);
    REQUIRE(r.error().to_string().find("ROM has changed") != std::string::npos);
}

TEST_CASE("verify_compare_inputs rejects size drift",
          "[diff][stcompare][verify][error]") {
    auto bench = load_demo_bench();
    auto const abs = std::filesystem::absolute("fixtures/demo.stune/source.bin");
    auto session = make_demo_session(abs.string(), bench.rom.crc32(),
                                     bench.rom.size() + 1);

    auto const r = st::diff::verify_compare_inputs(session, std::filesystem::current_path());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("size mismatch") != std::string::npos);
}

TEST_CASE("verify_compare_inputs rejects missing file",
          "[diff][stcompare][verify][error]") {
    auto bench = load_demo_bench();
    auto session = make_demo_session("does-not-exist.bin", bench.rom.crc32(),
                                     bench.rom.size());

    auto const r = st::diff::verify_compare_inputs(session, std::filesystem::current_path());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("cannot read") != std::string::npos);
}

TEST_CASE("load_compare_session rejects malformed TOML",
          "[diff][stcompare][error]") {
    auto const path = temp_stcompare_path("malformed");
    {
        std::ofstream out{path};
        out << "this is not valid toml = = = junk\n";
    }
    auto const r = st::diff::load_compare_session(path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
    std::filesystem::remove(path);
}

TEST_CASE("load_compare_session rejects missing [stcompare] table",
          "[diff][stcompare][error]") {
    auto const path = temp_stcompare_path("no_stcompare");
    {
        std::ofstream out{path};
        out << "[other]\nkey = 1\n";
    }
    auto const r = st::diff::load_compare_session(path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("[stcompare]") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("load_compare_session rejects future schema version",
          "[diff][stcompare][error]") {
    auto const path = temp_stcompare_path("future_schema");
    {
        std::ofstream out{path};
        out << "[stcompare]\n"
            << "schema_version = 99\n"
            << "pack_id = \"x\"\n"
            << "[stcompare.rom_a]\npath = \"a.bin\"\ncrc32 = 0\nsize = 0\n"
            << "[stcompare.rom_b]\npath = \"b.bin\"\ncrc32 = 0\nsize = 0\n";
    }
    auto const r = st::diff::load_compare_session(path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("newer than supported") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("CompareSession with no annotations round-trips with empty list",
          "[diff][stcompare][edge]") {
    auto bench = load_demo_bench();
    auto const abs = std::filesystem::absolute("fixtures/demo.stune/source.bin");
    auto session = make_demo_session(abs.string(), bench.rom.crc32(), bench.rom.size());
    session.annotations.clear();

    auto const path = temp_stcompare_path("no_annotations");
    REQUIRE(st::diff::save_compare_session(session, path).has_value());
    auto loaded = st::diff::load_compare_session(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->annotations.empty());
    std::filesystem::remove(path);
}
