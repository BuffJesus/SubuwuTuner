// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/diff.hpp"

#include "st/defs.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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
