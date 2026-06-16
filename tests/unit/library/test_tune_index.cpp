// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/tune_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace {

// Minimal valid index — two ptm entries + one stune. Mirrors the
// shape `tools/library_inventory/inventory.py` actually emits, so a
// regression here also catches a Python-side schema drift.
constexpr std::string_view kHappyToml = R"toml(
schema = "subuwutuner.library-index.v1"
ptm_count = 2
stune_count = 1

[[ptm]]
  path        = "D:/maps/Stage1 91 v401.ptm"
  size        = 58005
  mtime       = 1718316000
  md5         = "5ab18598343bc7d9dcd3afa5e0567d72"
  vendor      = "COBB"
  stage       = "Stage1"
  fuel_grade  = "91"
  variant     = "v401"

[[ptm]]
  path        = "D:/maps/Fehr WRK3.ptm"
  size        = 72245
  mtime       = 1718320000
  md5         = "FA67628FD8BCECE66935FF1E0ADF9E9A"
  vendor      = "Fehr"
  variant     = "WRK3"
  fuel_grade  = "93"
  unlocked_sibling = "D:/maps/Fehr WRK3_UNLOCKED.ptm"

[[stune]]
  path             = "D:/projects/test.stune"
  project_name     = "test project"
  ptm_vendor_id    = "Fehr"
  ptm_vehicle_id   = "SUBA_US_WRXM_CF_17_F"
  source_bin       = "D:/projects/test.stune/source.bin"
  source_bin_md5   = "abcdef0123456789abcdef0123456789"
)toml";

} // namespace

TEST_CASE("TuneIndex loads happy-path TOML", "[library][tune_index]") {
    auto idx = st::library::TuneIndex::load_from_string(kHappyToml);
    REQUIRE(idx.has_value());
    REQUIRE(idx->schema() == "subuwutuner.library-index.v1");
    REQUIRE(idx->ptm_entries().size() == 2);
    REQUIRE(idx->stune_entries().size() == 1);

    auto const &e0 = idx->ptm_entries()[0];
    REQUIRE(e0.path == "D:/maps/Stage1 91 v401.ptm");
    REQUIRE(e0.size == 58005);
    REQUIRE(e0.md5 == "5ab18598343bc7d9dcd3afa5e0567d72");
    REQUIRE(e0.vendor == "COBB");
    REQUIRE(e0.stage == "Stage1");

    auto const &e1 = idx->ptm_entries()[1];
    // Schema documents MD5 as lowercase hex; loader normalizes
    // mixed-case input so callers don't have to canonicalize.
    REQUIRE(e1.md5 == "fa67628fd8bcece66935ff1e0adf9e9a");
    REQUIRE(e1.vendor == "Fehr");
    REQUIRE(e1.variant == "WRK3");
    REQUIRE(e1.unlocked_sibling == "D:/maps/Fehr WRK3_UNLOCKED.ptm");

    auto const &s0 = idx->stune_entries()[0];
    REQUIRE(s0.project_name == "test project");
    REQUIRE(s0.ptm_vendor_id == "Fehr");
    REQUIRE(s0.source_bin_md5 == "abcdef0123456789abcdef0123456789");
}

TEST_CASE("TuneIndex lookup_by_md5 is case-insensitive", "[library][tune_index]") {
    auto idx = st::library::TuneIndex::load_from_string(kHappyToml);
    REQUIRE(idx.has_value());

    // The stored hash is lowercase per the loader's normalization;
    // lookup accepts mixed/upper.
    auto const *e_lower = idx->lookup_by_md5("5ab18598343bc7d9dcd3afa5e0567d72");
    auto const *e_upper = idx->lookup_by_md5("5AB18598343BC7D9DCD3AFA5E0567D72");
    auto const *e_mixed = idx->lookup_by_md5("5aB18598343Bc7d9dCd3afa5e0567D72");
    REQUIRE(e_lower != nullptr);
    REQUIRE(e_upper == e_lower);
    REQUIRE(e_mixed == e_lower);
    REQUIRE(e_lower->vendor == "COBB");
}

TEST_CASE("TuneIndex lookup_by_md5 misses cleanly", "[library][tune_index]") {
    auto idx = st::library::TuneIndex::load_from_string(kHappyToml);
    REQUIRE(idx.has_value());

    REQUIRE(idx->lookup_by_md5("00000000000000000000000000000000") == nullptr);
    REQUIRE(idx->lookup_by_md5("") == nullptr);
}

TEST_CASE("TuneIndex filter_by_name matches basename only", "[library][tune_index]") {
    auto idx = st::library::TuneIndex::load_from_string(kHappyToml);
    REQUIRE(idx.has_value());

    // "Fehr" is in the basename, not the parent path.
    auto const fehr = idx->filter_by_name("fehr");
    REQUIRE(fehr.size() == 1);
    REQUIRE(idx->ptm_entries()[fehr[0]].vendor == "Fehr");

    // Empty needle returns everything.
    auto const all = idx->filter_by_name("");
    REQUIRE(all.size() == 2);

    // Substring across files.
    auto const ptm = idx->filter_by_name("ptm");
    REQUIRE(ptm.size() == 2);
}

TEST_CASE("TuneIndex filter_by_vendor matches case-insensitive substring", "[library][tune_index]") {
    auto idx = st::library::TuneIndex::load_from_string(kHappyToml);
    REQUIRE(idx.has_value());

    auto const cobb = idx->filter_by_vendor("COBB");
    REQUIRE(cobb.size() == 1);
    auto const cobb_lower = idx->filter_by_vendor("cobb");
    REQUIRE(cobb_lower == cobb);
    auto const fehr = idx->filter_by_vendor("feh");
    REQUIRE(fehr.size() == 1);
    auto const empty = idx->filter_by_vendor("");
    REQUIRE(empty.size() == 2);
}

TEST_CASE("TuneIndex rejects unknown schema", "[library][tune_index]") {
    constexpr std::string_view bad_schema = R"toml(
schema = "totally-different-format.v9"
[[ptm]]
  path = "ignored.ptm"
  md5  = "00000000000000000000000000000000"
)toml";
    auto idx = st::library::TuneIndex::load_from_string(bad_schema);
    REQUIRE(!idx.has_value());
    REQUIRE(idx.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("TuneIndex rejects missing schema field", "[library][tune_index]") {
    constexpr std::string_view no_schema = R"toml(
[[ptm]]
  path = "ignored.ptm"
  md5  = "00000000000000000000000000000000"
)toml";
    auto idx = st::library::TuneIndex::load_from_string(no_schema);
    REQUIRE(!idx.has_value());
    REQUIRE(idx.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("TuneIndex drops malformed entries (no path)", "[library][tune_index]") {
    // The TOML has 3 [[ptm]] entries but one lacks `path` — the
    // loader skips it rather than failing the whole load.
    constexpr std::string_view partial = R"toml(
schema = "subuwutuner.library-index.v1"
[[ptm]]
  path = "ok-1.ptm"
  md5  = "11111111111111111111111111111111"
[[ptm]]
  md5  = "22222222222222222222222222222222"
[[ptm]]
  path = "ok-2.ptm"
  md5  = "33333333333333333333333333333333"
)toml";
    auto idx = st::library::TuneIndex::load_from_string(partial);
    REQUIRE(idx.has_value());
    REQUIRE(idx->ptm_entries().size() == 2);
    REQUIRE(idx->ptm_entries()[0].path == "ok-1.ptm");
    REQUIRE(idx->ptm_entries()[1].path == "ok-2.ptm");
}

TEST_CASE("TuneIndex handles empty index", "[library][tune_index]") {
    constexpr std::string_view empty_idx = R"toml(
schema = "subuwutuner.library-index.v1"
ptm_count = 0
stune_count = 0
)toml";
    auto idx = st::library::TuneIndex::load_from_string(empty_idx);
    REQUIRE(idx.has_value());
    REQUIRE(idx->ptm_entries().empty());
    REQUIRE(idx->stune_entries().empty());
    REQUIRE(idx->lookup_by_md5("anything") == nullptr);
}

TEST_CASE("TuneIndex rejects parse errors", "[library][tune_index]") {
    constexpr std::string_view bad_toml = "this is = not valid = toml = at all";
    auto idx = st::library::TuneIndex::load_from_string(bad_toml);
    REQUIRE(!idx.has_value());
    REQUIRE(idx.error().code() == st::ErrorCode::ParseError);
}
