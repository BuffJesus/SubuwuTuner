// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/atlas.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace {

constexpr char const *kMinimalAtlas = R"toml(
[atlas]
schema_version = 1
generated = "2026-06-13"
platform = "subaru.va.wrx.fa20dit"
primary_cid = "LF79103P"

[[table]]
id = "boost_limit_base"
display_name = "Airflow - Turbo - Boost Limit Base"
address = 203730
size_bytes = 19
dimension = "2d"
storage = "uint8"
units = "%"
purpose = "Hard ceiling on commanded boost."
fa24_portability = "unclear"
needs_def_promotion = true
common_core = true
high_variance = true
factory_immutable = true
clusters = ["cobb_stage1", "fehr_wrk"]
co_edits = ["wastegate_duty_max"]

[[tuner]]
id = "fehr_wrk"
display_name = "Fehr / DMann WRK"
patches_typical = 5981
mean_tables_touched = 117
philosophy = "Owns high-load; delegates part-throttle."
signature_tables = ["AVCS Intake Cam Target"]
skipped_tables = ["AVCS Exhaust Cam Target (TGV Closed)"]

[[safety_pair]]
id = "boost_target_wastegate"
title = "Boost Target / Wastegate authority"
severity = "high"
lhs_patterns = ["Boost Target Main"]
rhs_patterns = ["Wastegate Duty Maximum"]
rationale = "PI controller saturates without duty headroom."

[[anchor]]
id = "hpfp_cluster"
display_name = "HPFP cluster"
rom_offset_start = 301976  # 0x49B98
rom_offset_end = 301992    # 0x49BA8
length = 16
prose = "FA24 cam-lobe phase compensation."
)toml";

} // namespace

TEST_CASE("Atlas::load_from_string parses minimal atlas",
          "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    auto const &a = *r;
    CHECK(a.schema_version() == 1);
    CHECK(a.generated() == "2026-06-13");
    CHECK(a.platform() == "subaru.va.wrx.fa20dit");
    CHECK(a.primary_cid() == "LF79103P");
    CHECK(a.tables().size() == 1);
    CHECK(a.tuners().size() == 1);
    CHECK(a.safety_pairs().size() == 1);
    CHECK(a.anchors().size() == 1);
}

TEST_CASE("Atlas table fields parse round-trip", "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    auto const *t = r->find_table("boost_limit_base");
    REQUIRE(t != nullptr);
    CHECK(t->display_name == "Airflow - Turbo - Boost Limit Base");
    CHECK(t->address == 203730);
    CHECK(t->size_bytes == 19);
    CHECK(t->dimension == "2d");
    CHECK(t->storage == "uint8");
    CHECK(t->units == "%");
    CHECK(t->fa24_portability == "unclear");
    CHECK(t->needs_def_promotion);
    CHECK(t->common_core);
    CHECK(t->factory_immutable);
    CHECK(t->clusters.size() == 2);
    CHECK(t->co_edits.size() == 1);
}

TEST_CASE("Atlas tuner round-trip", "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    auto const *t = r->find_tuner("fehr_wrk");
    REQUIRE(t != nullptr);
    CHECK(t->patches_typical == 5981);
    CHECK(t->mean_tables_touched == 117);
    CHECK(t->signature_tables.size() == 1);
    CHECK(t->skipped_tables.size() == 1);
}

TEST_CASE("Atlas safety_pair round-trip", "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    auto const *p = r->find_safety_pair("boost_target_wastegate");
    REQUIRE(p != nullptr);
    CHECK(p->severity == "high");
    CHECK(p->lhs_patterns.size() == 1);
    CHECK(p->rhs_patterns.size() == 1);
}

TEST_CASE("Atlas resolves safety pairs by table display name", "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    auto const boost = r->safety_pairs_for_table("Boost Target Main - High Wastegate");
    REQUIRE(boost.size() == 1);
    CHECK(boost[0]->id == "boost_target_wastegate");
    auto const duty = r->safety_pairs_for_table("Wastegate Duty Maximum");
    REQUIRE(duty.size() == 1);
    CHECK(duty[0]->severity == "high");
    CHECK(r->safety_pairs_for_table("Coolant Temperature Compensation").empty());
}

TEST_CASE("Atlas anchor_for_offset finds covering anchor",
          "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    // Inside the HPFP cluster window (0x49B98..0x49BA8).
    auto const *a = r->anchor_for_offset(0x49B98);
    REQUIRE(a != nullptr);
    CHECK(a->id == "hpfp_cluster");
    // 0x49BA8 is the half-open upper bound — not covered.
    CHECK(r->anchor_for_offset(0x49BA8) == nullptr);
    CHECK(r->anchor_for_offset(0x10000) == nullptr);
}

TEST_CASE("Atlas rejects schema_version > supported",
          "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string(R"toml(
[atlas]
schema_version = 99
generated = "1999-12-31"
platform = "x"
primary_cid = "X"
)toml");
    REQUIRE(!r.has_value());
    CHECK(r.error().code() == st::ErrorCode::UnsupportedVersion);
}

TEST_CASE("Atlas rejects missing [atlas] header",
          "[library][atlas]") {
    auto const r = st::library::Atlas::load_from_string("# empty\n");
    REQUIRE(!r.has_value());
    CHECK(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Atlas loads the shipped fixtures/tuner_atlas/tuner_atlas.toml",
          "[library][atlas][integration]") {
    // Locate the fixture relative to the test binary. The build copies
    // (or references) it under fixtures/tuner_atlas/. We accept either
    // location to keep this robust to layout changes.
    namespace fs = std::filesystem;
    auto const candidates = std::vector<fs::path>{
        fs::path{ST_REPO_ROOT} / "fixtures" / "tuner_atlas" / "tuner_atlas.toml",
    };
    fs::path found;
    for (auto const &p : candidates) {
        if (fs::exists(p)) {
            found = p;
            break;
        }
    }
    REQUIRE(!found.empty());

    auto const r = st::library::Atlas::load_from_file(found);
    REQUIRE(r.has_value());
    auto const &a = *r;
    // Built atlas should have 170 LF79103P tables, 9 tuner clusters,
    // 8 safety pairs, 3 anchors (per the generator). v2 also carries
    // cid_coverage rows.
    CHECK(a.schema_version() == 2);
    CHECK(a.platform() == "subaru.va.wrx.fa20dit");
    CHECK(a.primary_cid() == "LF79103P");
    CHECK(a.tables().size() == 170);
    CHECK(a.tuners().size() == 9);
    CHECK(a.safety_pairs().size() == 8);
    CHECK(a.anchors().size() == 3);
    // v2: cid_coverage non-empty; LF79103P is the primary row.
    REQUIRE(!a.cid_coverage().empty());
    auto const *primary = a.find_cid_coverage("LF79103P");
    REQUIRE(primary != nullptr);
    CHECK(primary->table_count == 170);
    // HPFP cluster anchor covers 0x49B98.
    auto const *hpfp = a.anchor_for_offset(0x49B98);
    REQUIRE(hpfp != nullptr);
    CHECK(hpfp->id == "hpfp_cluster");
    // MAF Sensor Scale anchor covers 0x30F64.
    auto const *maf = a.anchor_for_offset(0x30F64);
    REQUIRE(maf != nullptr);
    CHECK(maf->id == "maf_sensor_scale");
}

// -----------------------------------------------------------------------
// v2 schema tests
// -----------------------------------------------------------------------

namespace {

constexpr char const *kMultiCidAtlas = R"toml(
[atlas]
schema_version = 2
generated = "2026-06-13"
platform = "subaru.va.wrx.fa20dit"
primary_cid = "LF79103P"

[[table]]
id = "wastegate_duty_max"
display_name = "Airflow - Turbo - Wastegate - Wastegate Duty Maximum"
address = 202290        # 0x31632
size_bytes = 476
dimension = "3d"
storage = "uint16"
units = "%"
purpose = "Upper-bound wastegate duty"
fa24_portability = "needs_retune"
needs_def_promotion = false
common_core = true
high_variance = false
clusters = ["cobb_stage1"]
co_edits = []
primary_cid = "LF79103P"

[[table.cid_address]]
cid = "LF75404S"
address = 168106         # 0x292AA
size_bytes = 8670

[[table.cid_address]]
cid = "LF9C102P"
address = 200000
size_bytes = 476

[[table]]
id = "exclusive_to_primary"
display_name = "Some primary-only table"
address = 100000
size_bytes = 32
dimension = "2d"
storage = "uint8"
units = ""
purpose = ""
fa24_portability = "unclear"
needs_def_promotion = false
common_core = false
high_variance = false
clusters = []
co_edits = []
primary_cid = "LF79103P"

[[cid_coverage]]
cid = "LF79103P"
table_count = 2
note = "primary"

[[cid_coverage]]
cid = "LF75404S"
table_count = 1
note = ""

[[cid_coverage]]
cid = "LHBH800B00G"
table_count = 0
note = "not covered: RH850 Gen-B (RAM-mapped pointers)"
)toml";

} // namespace

TEST_CASE("Atlas v2: parses cid_address sub-tables", "[library][atlas][v2]") {
    auto const r = st::library::Atlas::load_from_string(kMultiCidAtlas);
    REQUIRE(r.has_value());
    auto const &a = *r;
    CHECK(a.schema_version() == 2);
    REQUIRE(a.tables().size() == 2);
    auto const *t = a.find_table("wastegate_duty_max");
    REQUIRE(t != nullptr);
    CHECK(t->primary_cid == "LF79103P");
    REQUIRE(t->cid_addresses.size() == 2);
    CHECK(t->cid_addresses[0].cid == "LF75404S");
    CHECK(t->cid_addresses[0].address == 168106);
    CHECK(t->cid_addresses[0].size_bytes == 8670);
    CHECK(t->cid_addresses[1].cid == "LF9C102P");
    CHECK(t->cid_addresses[1].address == 200000);
}

TEST_CASE("Atlas v2: find_table_for_cid resolves primary and per-CID",
          "[library][atlas][v2]") {
    auto const r = st::library::Atlas::load_from_string(kMultiCidAtlas);
    REQUIRE(r.has_value());
    auto const &a = *r;
    // Primary CID lookup synthesises a record from the table's own
    // address / size_bytes.
    auto const primary = a.find_table_for_cid("wastegate_duty_max", "LF79103P");
    REQUIRE(primary.has_value());
    CHECK(primary->cid == "LF79103P");
    CHECK(primary->address == 202290);
    CHECK(primary->size_bytes == 476);
    // Non-primary CID lookup hits the cid_addresses array.
    auto const secondary = a.find_table_for_cid("wastegate_duty_max", "LF75404S");
    REQUIRE(secondary.has_value());
    CHECK(secondary->cid == "LF75404S");
    CHECK(secondary->address == 168106);
    CHECK(secondary->size_bytes == 8670);
    // CID with no binding for this table -> nullopt.
    auto const missing = a.find_table_for_cid("wastegate_duty_max", "LHBH800B00G");
    CHECK(!missing.has_value());
    // Unknown table -> nullopt.
    auto const unknown_table = a.find_table_for_cid("does_not_exist", "LF79103P");
    CHECK(!unknown_table.has_value());
    // Table with no cid_addresses still resolves for its primary CID.
    auto const excl = a.find_table_for_cid("exclusive_to_primary", "LF79103P");
    REQUIRE(excl.has_value());
    CHECK(excl->address == 100000);
    CHECK(excl->size_bytes == 32);
    // ...but not for any other CID.
    auto const excl_other = a.find_table_for_cid("exclusive_to_primary", "LF75404S");
    CHECK(!excl_other.has_value());
}

TEST_CASE("Atlas v2: cid_coverage round-trip", "[library][atlas][v2]") {
    auto const r = st::library::Atlas::load_from_string(kMultiCidAtlas);
    REQUIRE(r.has_value());
    auto const &a = *r;
    REQUIRE(a.cid_coverage().size() == 3);
    auto const *p = a.find_cid_coverage("LF79103P");
    REQUIRE(p != nullptr);
    CHECK(p->table_count == 2);
    auto const *rh = a.find_cid_coverage("LHBH800B00G");
    REQUIRE(rh != nullptr);
    CHECK(rh->table_count == 0);
    CHECK(rh->note.find("not covered") != std::string::npos);
    CHECK(a.find_cid_coverage("nope") == nullptr);
}

TEST_CASE("Atlas v2 loader still accepts v1 atlases (forward-compat)",
          "[library][atlas][v2]") {
    // The kMinimalAtlas fixture above is schema_version = 1. The v2
    // loader must read it unchanged: no cid_coverage, no per-table
    // primary_cid, and find_table_for_cid falls back to the atlas-level
    // primary_cid.
    auto const r = st::library::Atlas::load_from_string(kMinimalAtlas);
    REQUIRE(r.has_value());
    auto const &a = *r;
    CHECK(a.schema_version() == 1);
    CHECK(a.cid_coverage().empty());
    auto const *t = a.find_table("boost_limit_base");
    REQUIRE(t != nullptr);
    CHECK(t->primary_cid.empty());          // not set on v1
    CHECK(t->cid_addresses.empty());        // not set on v1
    // v1 atlases should still resolve for their atlas-level primary CID.
    auto const pc = a.find_table_for_cid("boost_limit_base", "LF79103P");
    REQUIRE(pc.has_value());
    CHECK(pc->cid == "LF79103P");
    CHECK(pc->address == 203730);
    // And reject any other CID since there's no per-table mapping.
    CHECK(!a.find_table_for_cid("boost_limit_base", "LF75404S").has_value());
}

TEST_CASE("Atlas v2: archived v1 fixture round-trips through v2 loader",
          "[library][atlas][v2][integration]") {
    // We ship an archived v1 atlas at fixtures/tuner_atlas/v1.archived.toml
    // to keep forward-compat coverage on real-shaped data, not just the
    // synthetic minimal fixture.
    namespace fs = std::filesystem;
    auto const path = fs::path{ST_REPO_ROOT} / "fixtures" / "tuner_atlas" /
                      "v1.archived.toml";
    if (!fs::exists(path)) {
        WARN("archived v1 atlas not present at " << path.string());
        return;
    }
    auto const r = st::library::Atlas::load_from_file(path);
    REQUIRE(r.has_value());
    auto const &a = *r;
    CHECK(a.schema_version() == 1);
    CHECK(a.primary_cid() == "LF79103P");
    CHECK(a.tables().size() == 170);
    CHECK(a.cid_coverage().empty());
    // Spot-check: the v2 lookup helper should still resolve known v1
    // tables for the atlas-level primary CID.
    auto const &first = a.tables().front();
    auto const pc = a.find_table_for_cid(first.id, "LF79103P");
    REQUIRE(pc.has_value());
    CHECK(pc->address == first.address);
    CHECK(pc->size_bytes == first.size_bytes);
}

TEST_CASE("Atlas: v3 atlases are rejected as unsupported",
          "[library][atlas][v2]") {
    // schema_version_supported() is now 2; a hypothetical v3 must still
    // be rejected with UnsupportedVersion.
    auto const r = st::library::Atlas::load_from_string(R"toml(
[atlas]
schema_version = 3
generated = "2099-01-01"
platform = "x"
primary_cid = "X"
)toml");
    REQUIRE(!r.has_value());
    CHECK(r.error().code() == st::ErrorCode::UnsupportedVersion);
}
