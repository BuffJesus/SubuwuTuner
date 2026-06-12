// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for st::library::TableRange + helpers — the .ptm patch ↔
// calibration-table cross-reference used by ptm inspect / list-patches /
// import / diff --by-table to render table names alongside raw addresses.
//
// These tests use the demo-pack fixture (a synthetic VA WRX MT pack
// bundled in the public repo) so they run on every CI build without
// touching analyst-side private packs.

#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/defs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path demo_pack_path() {
    // Resolve from the source tree via the same compile-time pattern
    // used for ST_FIXTURE_DEMO_STUNE — the demo pack lives next to it.
#ifdef ST_FIXTURE_DEMO_STUNE
    return std::filesystem::path{ST_FIXTURE_DEMO_STUNE}.parent_path() / "demo-pack";
#else
    return {};
#endif
}

st::library::PatchEntry make_patch(std::uint32_t rom_offset, std::uint32_t length) {
    st::library::PatchEntry p;
    p.rom_offset = rom_offset;
    p.length = length;
    p.bytes.assign(length, 0xAA);
    return p;
}

} // namespace

TEST_CASE("table_mapping: compute_table_ranges from demo pack",
          "[library][table_mapping]") {
    auto const pack = demo_pack_path();
    if (pack.empty() || !std::filesystem::exists(pack)) {
        SKIP("demo-pack fixture not staged");
    }
    auto def = st::Definition::from_file(pack);
    REQUIRE(def.has_value());

    auto const ranges = st::library::compute_table_ranges(*def);
    // Demo pack ships at least one table. The exact count is fixed in
    // the bundled fixture so this is a stable assertion.
    REQUIRE_FALSE(ranges.empty());

    // Ranges are sorted ascending by start.
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        REQUIRE(ranges[i].start >= ranges[i - 1].start);
    }
    // Every range has a non-empty footprint (zero-byte ranges are
    // dropped during compute_table_ranges).
    for (auto const &r : ranges) {
        REQUIRE(r.end_exclusive > r.start);
        REQUIRE_FALSE(r.id.empty());
    }
}

TEST_CASE("table_mapping: find_table_for handles in-range, out-of-range, boundary",
          "[library][table_mapping]") {
    std::vector<st::library::TableRange> ranges;
    ranges.push_back({"t_outer", "Outer", "cat", 0x0100, 0x0200});
    ranges.push_back({"t_inner", "Inner scalar", "cat", 0x0140, 0x0142});
    ranges.push_back({"t_far",   "Far",   "cat", 0x1000, 0x1010});

    // Offset 0x0150 sits inside both outer and inner — most-specific
    // (smallest) wins.
    auto const *hit_inner = st::library::find_table_for(ranges, 0x0140);
    REQUIRE(hit_inner != nullptr);
    REQUIRE(hit_inner->id == "t_inner");

    // Offset inside outer-only.
    auto const *hit_outer = st::library::find_table_for(ranges, 0x0180);
    REQUIRE(hit_outer != nullptr);
    REQUIRE(hit_outer->id == "t_outer");

    // Boundary: start is inclusive, end_exclusive is exclusive.
    REQUIRE(st::library::find_table_for(ranges, 0x0100)->id == "t_outer");
    REQUIRE(st::library::find_table_for(ranges, 0x0200) == nullptr);

    // Out of any range.
    REQUIRE(st::library::find_table_for(ranges, 0x0500) == nullptr);

    // Empty ranges.
    REQUIRE(st::library::find_table_for({}, 0x0100) == nullptr);
}

TEST_CASE("table_mapping: aggregate_table_hits groups + sorts",
          "[library][table_mapping]") {
    std::vector<st::library::TableRange> ranges;
    ranges.push_back({"throttle", "Throttle Target", "fuel", 0x1000, 0x1100});
    ranges.push_back({"boost",    "Boost Wastegate", "boost", 0x2000, 0x2100});
    ranges.push_back({"ignition", "Ignition Base",  "spark", 0x3000, 0x3100});

    st::library::DecodedPtm decoded;
    // 5 patches in throttle (heaviest by count)
    for (std::uint32_t i = 0; i < 5U; ++i) {
        decoded.patches.push_back(make_patch(0x1000U + i * 4U, 4));
    }
    // 2 patches in boost — but each is bigger
    decoded.patches.push_back(make_patch(0x2000U, 32));
    decoded.patches.push_back(make_patch(0x2040U, 32));
    // 1 in ignition
    decoded.patches.push_back(make_patch(0x3050U, 8));
    // 3 outside any table — should be ignored
    decoded.patches.push_back(make_patch(0x9000U, 1));
    decoded.patches.push_back(make_patch(0x9100U, 1));
    decoded.patches.push_back(make_patch(0x9200U, 1));

    auto const hits = st::library::aggregate_table_hits(decoded, ranges);
    REQUIRE(hits.size() == 3);
    // Sorted by patch_count desc (5, 2, 1).
    REQUIRE(hits[0].table_id == "throttle");
    REQUIRE(hits[0].patch_count == 5U);
    REQUIRE(hits[0].total_bytes == 20U);
    REQUIRE(hits[1].table_id == "boost");
    REQUIRE(hits[1].patch_count == 2U);
    REQUIRE(hits[1].total_bytes == 64U);
    REQUIRE(hits[2].table_id == "ignition");
    REQUIRE(hits[2].patch_count == 1U);
    REQUIRE(hits[2].total_bytes == 8U);
}

TEST_CASE("table_mapping: aggregate ties break by total_bytes",
          "[library][table_mapping]") {
    std::vector<st::library::TableRange> ranges;
    ranges.push_back({"small", "Small table", "cat", 0x1000, 0x1100});
    ranges.push_back({"big",   "Big table",   "cat", 0x2000, 0x2100});

    st::library::DecodedPtm decoded;
    // Same patch count, different byte totals.
    decoded.patches.push_back(make_patch(0x1000U, 4));
    decoded.patches.push_back(make_patch(0x1010U, 4));
    decoded.patches.push_back(make_patch(0x2000U, 32));
    decoded.patches.push_back(make_patch(0x2040U, 32));

    auto const hits = st::library::aggregate_table_hits(decoded, ranges);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].table_id == "big");      // 64 bytes wins the tie
    REQUIRE(hits[1].table_id == "small");
}

TEST_CASE("table_mapping: empty inputs return empty",
          "[library][table_mapping]") {
    st::library::DecodedPtm decoded;
    std::vector<st::library::TableRange> ranges;
    REQUIRE(st::library::aggregate_table_hits(decoded, ranges).empty());

    // With patches but no ranges.
    decoded.patches.push_back(make_patch(0x1000U, 4));
    REQUIRE(st::library::aggregate_table_hits(decoded, ranges).empty());
}
