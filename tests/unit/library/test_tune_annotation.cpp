// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/tune_annotation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

constexpr char const *kMinimalAtlas = R"toml(
[atlas]
schema_version = 1
generated = "2026-06-13"
platform = "subaru.va.wrx.fa20dit"
primary_cid = "LF79103P"

[[tuner]]
id = "cobb_stage1"
display_name = "COBB Stage 1 (Redline/SF equivalent)"
patches_typical = 4244
mean_tables_touched = 90
philosophy = "Consensus AVCS exhaust-only."
signature_tables = []
skipped_tables = []

[[tuner]]
id = "fehr_wrk"
display_name = "Fehr/DMann WRK1/2/3 ETune (user's car)"
patches_typical = 5981
mean_tables_touched = 117
philosophy = "Independent AVCS intake."
signature_tables = []
skipped_tables = []

[[tuner]]
id = "ntm_fa24"
display_name = "NTM FA24 basemap"
patches_typical = 761
mean_tables_touched = 70
philosophy = "Swap-specific basemap."
signature_tables = []
skipped_tables = []
)toml";

st::library::TuneIndexEntry mk_entry(std::string vendor = "",
                                      std::string variant = "",
                                      std::string md5 = "",
                                      std::string path = "/lib/tune.ptm") {
    st::library::TuneIndexEntry e;
    e.path = std::move(path);
    e.vendor = std::move(vendor);
    e.variant = std::move(variant);
    e.md5 = std::move(md5);
    e.size = 72000;
    return e;
}

} // namespace

TEST_CASE("annotate_tune: tuner_family resolves from vendor against atlas",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());

    auto a = st::library::annotate_tune(mk_entry("COBB"), *atlas_r);
    CHECK(a.tuner_family == "cobb_stage1");
    CHECK(a.cobb_nexgen);
    CHECK_FALSE(a.fehr_family);

    auto b = st::library::annotate_tune(mk_entry("Fehr"), *atlas_r);
    CHECK(b.tuner_family == "fehr_wrk");
    CHECK(b.fehr_family);
    CHECK_FALSE(b.cobb_nexgen);

    auto c = st::library::annotate_tune(mk_entry("NTM"), *atlas_r);
    CHECK(c.tuner_family == "ntm_fa24");
    CHECK(c.ntm_swap_basemap);
}

TEST_CASE("annotate_tune: empty vendor leaves family empty + no flags",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto a = st::library::annotate_tune(mk_entry(), *atlas_r);
    CHECK(a.tuner_family.empty());
    CHECK_FALSE(a.fehr_family);
    CHECK_FALSE(a.cobb_nexgen);
    CHECK_FALSE(a.ntm_swap_basemap);
}

TEST_CASE("annotate_tune: NTM swap basemap detected from variant when vendor is silent",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto a = st::library::annotate_tune(mk_entry("", "FA24 basemap"), *atlas_r);
    CHECK(a.ntm_swap_basemap);
}

TEST_CASE("annotate_tune: Felix is a Fehr-family signal",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto a = st::library::annotate_tune(mk_entry("Felix"), *atlas_r);
    CHECK(a.fehr_family);
}

TEST_CASE("annotate_tune: currently_flashed matches /backupcksum MD5 case-insensitive",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto e = mk_entry("Fehr", "WRK3",
                       "FF11034EC965430DA3DCFEC8D0A48733"); // uppercase in index
    st::library::ApCrossRefInputs ap;
    ap.backupcksum = "ff11034ec965430da3dcfec8d0a48733"; // lowercase from AP
    auto a = st::library::annotate_tune(e, *atlas_r, ap);
    CHECK(a.currently_flashed);
}

TEST_CASE("annotate_tune: currently_flashed false on MD5 miss",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto e = mk_entry("Fehr", "WRK3",
                       "ff11034ec965430da3dcfec8d0a48733");
    st::library::ApCrossRefInputs ap;
    ap.backupcksum = "deadbeef" + std::string(24, '0');
    auto a = st::library::annotate_tune(e, *atlas_r, ap);
    CHECK_FALSE(a.currently_flashed);
}

TEST_CASE("annotate_tune: on_ap matches /maps/ basename",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto e = mk_entry("Fehr", "WRK3", "ff11034ec965430da3dcfec8d0a48733",
                       "D:/Subuwu/ap-maps/Fehr_DMann WRK3.ptm");
    st::library::ApCrossRefInputs ap;
    ap.maps_filenames = {"Stage1+SF 93 v401.ptm",
                          "Fehr_DMann WRK3.ptm",
                          "test.ptm"};
    auto a = st::library::annotate_tune(e, *atlas_r, ap);
    CHECK(a.on_ap);
}

TEST_CASE("annotate_tune: on_ap false when basename absent from /maps/",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto e = mk_entry("Fehr", "WRK3", "",
                       "D:/Subuwu/ap-maps/Fehr_DMann WRK3.ptm");
    st::library::ApCrossRefInputs ap;
    ap.maps_filenames = {"Stage1+SF 93 v401.ptm"}; // not the user's tune
    auto a = st::library::annotate_tune(e, *atlas_r, ap);
    CHECK_FALSE(a.on_ap);
}

TEST_CASE("annotate_tune: AP cross-ref degrades cleanly with empty inputs",
          "[library][annotation]") {
    auto const atlas_r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(atlas_r.has_value());
    auto a = st::library::annotate_tune(mk_entry("Fehr", "WRK3"), *atlas_r,
                                          st::library::ApCrossRefInputs{});
    CHECK_FALSE(a.currently_flashed);
    CHECK_FALSE(a.on_ap);
}

TEST_CASE("annotation_vendor_matches: symmetric containment",
          "[library][annotation]") {
    CHECK(st::library::annotation_vendor_matches("COBB",
                                                   "COBB Stage 1 (Redline/SF)"));
    CHECK(st::library::annotation_vendor_matches("Fehr",
                                                   "Fehr/DMann WRK1/2/3"));
    CHECK_FALSE(st::library::annotation_vendor_matches("Foo", "Bar"));
    CHECK_FALSE(
        st::library::annotation_vendor_matches("", "Anything"));
}
