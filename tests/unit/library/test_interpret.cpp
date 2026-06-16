// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unit tests for st::library::interpret_inspect / interpret_diff —
// the heuristic prose generator used by `ptm inspect` and `ptm diff`
// (CLI + GUI modals). One test per rule lane: synthesize a minimal
// DecodedPtm + (optional) TableHit / TuneDiffResult that ought to
// trigger exactly one rule, assert that the sentence appears.

#include "st/library/interpret.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/library/tune_diff.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

st::library::PatchEntry mk(std::uint32_t rom, std::vector<std::uint8_t> bytes) {
    st::library::PatchEntry p;
    p.rom_offset = rom;
    p.length = static_cast<std::uint32_t>(bytes.size());
    p.bytes = std::move(bytes);
    return p;
}

st::library::TableHit mk_hit(std::string id, std::string name,
                              std::uint32_t patches, std::uint64_t bytes) {
    st::library::TableHit h;
    h.table_id = std::move(id);
    h.table_name = std::move(name);
    h.patch_count = patches;
    h.total_bytes = bytes;
    return h;
}

[[nodiscard]] bool any_contains(std::vector<std::string> const &lines,
                                std::string_view needle) {
    return std::any_of(lines.begin(), lines.end(), [&](std::string const &s) {
        return s.find(needle) != std::string::npos;
    });
}

} // namespace

TEST_CASE("interpret_inspect: empty tune emits nothing", "[library][interpret]") {
    st::library::DecodedPtm decoded;
    auto const lines = st::library::interpret_inspect(decoded, {});
    REQUIRE(lines.empty());
}

TEST_CASE("interpret_inspect: lightweight basemap threshold fires under 1000",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    for (int i = 0; i < 50; ++i) {
        decoded.patches.push_back(
            mk(static_cast<std::uint32_t>(0x10000 + i * 4), {0x00, 0x00, 0x00, 0x00}));
    }
    auto const lines = st::library::interpret_inspect(decoded, {});
    REQUIRE(any_contains(lines, "Lightweight basemap or single-feature mod"));
}

TEST_CASE("interpret_inspect: Fehr-class threshold fires past 5000",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    for (int i = 0; i < 5500; ++i) {
        decoded.patches.push_back(
            mk(static_cast<std::uint32_t>(0x100000 + i), {0x00}));
    }
    auto const lines = st::library::interpret_inspect(decoded, {});
    REQUIRE(any_contains(lines, "Full custom tune"));
    REQUIRE(any_contains(lines, "Fehr-class"));
}

TEST_CASE("interpret_inspect: HPFP cluster anchor fires without def-pack",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    // Patch lands at 0x49B9E — inside the HPFP cluster window
    // [0x49B98, 0x49BA8) per project_hpfp_0x49ba0_resolved.md.
    decoded.patches.push_back(mk(0x49B9E, {0x10, 0x20, 0x30, 0x40, 0x50}));
    auto const lines = st::library::interpret_inspect(decoded, {});
    REQUIRE(any_contains(lines, "HPFP cluster"));
    REQUIRE(any_contains(lines, "FA24"));
}

TEST_CASE("interpret_inspect: MAF Sensor Scale BigSF anchor fires",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x30F64, std::vector<std::uint8_t>(256, 0x42)));
    auto const lines = st::library::interpret_inspect(decoded, {});
    REQUIRE(any_contains(lines, "MAF Sensor Scale"));
    REQUIRE(any_contains(lines, "BigSF"));
}

TEST_CASE("interpret_inspect: Target Throttle variants fire Stage-tier rule",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    // Make it count as a mid-weight tune so the only top_tables-driven
    // rule under test fires.
    for (int i = 0; i < 1500; ++i) {
        decoded.patches.push_back(
            mk(static_cast<std::uint32_t>(0x10000 + i), {0x00}));
    }
    std::vector<st::library::TableHit> hits;
    hits.push_back(mk_hit("tt_a", "Target Throttle Sport", 80, 520));
    hits.push_back(mk_hit("tt_b", "Target Throttle Sport #", 80, 520));
    hits.push_back(mk_hit("tt_c", "Target Throttle Intelligent", 80, 520));
    hits.push_back(mk_hit("tt_d", "Target Throttle Intelligent #", 80, 520));
    hits.push_back(mk_hit("tt_e", "Target Throttle Sport+", 80, 520));
    auto const lines = st::library::interpret_inspect(decoded, hits);
    REQUIRE(any_contains(lines, "Target Throttle"));
    REQUIRE(any_contains(lines, "Stage-tier"));
}

TEST_CASE("interpret_inspect: AVCS Exhaust-only fires COBB consensus rule",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x20000, {0x00}));
    std::vector<st::library::TableHit> hits;
    hits.push_back(mk_hit("avcs_ex_a", "AVCS Exhaust Cam Target", 20, 256));
    auto const lines = st::library::interpret_inspect(decoded, hits);
    REQUIRE(any_contains(lines, "AVCS Exhaust"));
    REQUIRE(any_contains(lines, "COBB"));
}

TEST_CASE("interpret_inspect: AVCS Intake touched fires Fehr-family signature",
          "[library][interpret]") {
    // Cross-corpus finding: COBB / NexGen never touch Intake AVCS, so
    // any Intake retune (with or without Exhaust) is a Fehr-family
    // signal. See findings/tuning-knowledge-2026-06-13/fingerprints/.
    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x20000, {0x00}));
    std::vector<st::library::TableHit> hits;
    hits.push_back(mk_hit("avcs_in", "AVCS Intake Cam Target", 20, 256));
    hits.push_back(mk_hit("avcs_ex", "AVCS Exhaust Cam Target", 20, 256));
    auto const lines = st::library::interpret_inspect(decoded, hits);
    REQUIRE(any_contains(lines, "AVCS Intake"));
    REQUIRE(any_contains(lines, "Fehr-family"));
}

TEST_CASE("interpret_inspect: Wastegate Init + Max fires boost-up rule",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x30000, {0x00}));
    std::vector<st::library::TableHit> hits;
    hits.push_back(mk_hit("wg_i", "Wastegate Duty Initial", 96, 192));
    hits.push_back(mk_hit("wg_m", "Wastegate Duty Maximum", 96, 192));
    auto const lines = st::library::interpret_inspect(decoded, hits);
    REQUIRE(any_contains(lines, "Wastegate Duty Initial"));
    REQUIRE(any_contains(lines, "boost-up"));
}

TEST_CASE("interpret_inspect: Knock Threshold + DAA fires timing aggression",
          "[library][interpret]") {
    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x40000, {0x00}));
    std::vector<st::library::TableHit> hits;
    hits.push_back(mk_hit("kt", "Knock Threshold Base", 40, 256));
    hits.push_back(mk_hit("daa", "Ignition Dynamic Advance Adder", 45, 480));
    auto const lines = st::library::interpret_inspect(decoded, hits);
    REQUIRE(any_contains(lines, "Knock thresholds"));
    REQUIRE(any_contains(lines, "Dynamic Advance Adder"));
    REQUIRE(any_contains(lines, "timing aggression"));
}

TEST_CASE("interpret_diff: identical tunes emit 'structurally identical'",
          "[library][interpret]") {
    st::library::DecodedPtm a;
    a.patches.push_back(mk(0x1000, {1, 2, 3, 4}));
    a.patches.push_back(mk(0x2000, {5, 6, 7, 8}));
    st::library::DecodedPtm const b = a;
    auto const r = st::library::compute_tune_diff(a, b);
    auto const lines = st::library::interpret_diff(a, b, r);
    REQUIRE(any_contains(lines, "structurally identical"));
}

TEST_CASE("interpret_diff: uniform per-byte delta fires scaler bump",
          "[library][interpret]") {
    // Five shared patches, each 15 cells (odd length → uint16 view
    // doesn't apply, so the per-byte +2 reading must dominate).
    st::library::DecodedPtm a, b;
    for (int i = 0; i < 5; ++i) {
        std::vector<std::uint8_t> bytes_a(15, 0x40);
        std::vector<std::uint8_t> bytes_b(15, 0x42);
        a.patches.push_back(
            mk(static_cast<std::uint32_t>(0x10000 + i * 32), bytes_a));
        b.patches.push_back(
            mk(static_cast<std::uint32_t>(0x10000 + i * 32), bytes_b));
    }
    auto const r = st::library::compute_tune_diff(a, b);
    auto const lines = st::library::interpret_diff(a, b, r);
    REQUIRE(any_contains(lines, "+2"));
    REQUIRE(any_contains(lines, "scaler bump"));
}

TEST_CASE("interpret_diff: signed int16 BE wrap-around reports the signed delta",
          "[library][interpret][signed]") {
    // Regression for the WRK2→WRK3 sign defect: signed-cell tables
    // (ignition timing, knock retard, torque trim) crossing zero are
    // stored as int16 BE in flash but read as uint16 by the per-cell
    // detector. A cell going from +10 raw to -6 raw produces unsigned
    // bytes 0x000A → 0xFFFA — naive subtraction yields +65520, hiding
    // the real -16 delta. The wrap-aware reinterpretation must surface
    // -16, not +65520.
    st::library::DecodedPtm a, b;
    std::vector<std::uint8_t> bytes_a(64, 0);
    std::vector<std::uint8_t> bytes_b(64, 0);
    // 32 uint16 BE cells. Each cell goes from +10 raw to -6 raw (i.e.
    // -16 signed delta). Encoded BE: high byte first.
    for (std::size_t i = 0; i < 32; ++i) {
        // +10 = 0x000A
        bytes_a[i * 2 + 0] = 0x00;
        bytes_a[i * 2 + 1] = 0x0A;
        // -6 = 0xFFFA
        bytes_b[i * 2 + 0] = 0xFF;
        bytes_b[i * 2 + 1] = 0xFA;
    }
    a.patches.push_back(mk(0x70000, bytes_a));
    b.patches.push_back(mk(0x70000, bytes_b));
    auto const r = st::library::compute_tune_diff(a, b);
    auto const lines = st::library::interpret_diff(a, b, r);
    // Must report the signed delta (-16), not the unsigned wrap (+65520).
    bool saw_signed = false;
    bool saw_unsigned_wrap = false;
    for (auto const &l : lines) {
        if (l.find("-16") != std::string::npos)
            saw_signed = true;
        if (l.find("65520") != std::string::npos || l.find("+65520") != std::string::npos)
            saw_unsigned_wrap = true;
    }
    REQUIRE(saw_signed);
    REQUIRE_FALSE(saw_unsigned_wrap);
}

TEST_CASE("interpret_diff: targeted partial-cell delta surfaces correctly",
          "[library][interpret]") {
    // Reproduces the WRK2→WRK3 case: one 476-byte patch where half the
    // uint16 cells shifted by +2 LE / +512 BE and the rest are
    // unchanged. The old per-byte uniform check failed silently here;
    // the new cell-aware detector must surface "B shifts N of M cells".
    st::library::DecodedPtm a, b;
    std::vector<std::uint8_t> bytes_a(476, 0);
    std::vector<std::uint8_t> bytes_b(476, 0);
    // 112 of 238 uint16 LE cells with +2 LE delta = high byte +0, low byte +2.
    for (std::size_t i = 0; i < 112; ++i) {
        bytes_a[i * 2] = 0x00;
        bytes_a[i * 2 + 1] = 0x10;
        bytes_b[i * 2] = 0x00;
        bytes_b[i * 2 + 1] = 0x12;
    }
    a.patches.push_back(mk(0x31456, bytes_a));
    b.patches.push_back(mk(0x31456, bytes_b));
    auto const r = st::library::compute_tune_diff(a, b);
    auto const lines = st::library::interpret_diff(a, b, r);
    REQUIRE(any_contains(lines, "112"));
    REQUIRE(any_contains(lines, "+2"));
}

TEST_CASE("interpret_diff: duplicate-key patches don't surface phantom uniform deltas",
          "[library][interpret]") {
    // Regression test for the duplicate-(rom,ram) bug: a Fehr-class
    // .ptm has ~1000 patches sharing keys (sequentially-applied
    // overlays). The old find_matching_b returned B's first match for
    // every A duplicate, mis-aligning ~1000 pairs and surfacing
    // spurious "uniform -8 across 67 cells" prose. The new
    // last-occurrence semantics must skip these cleanly.
    st::library::DecodedPtm a, b;
    // Two A-patches at the same (rom, ram) with different bytes;
    // last-occurrence wins on the effective state.
    a.patches.push_back(mk(0x40000, {0x40, 0x40, 0x40, 0x40}));
    a.patches.push_back(mk(0x40000, {0x48, 0x48, 0x48, 0x48})); // overlays
    b.patches.push_back(mk(0x40000, {0x40, 0x40, 0x40, 0x40})); // earlier B
    b.patches.push_back(mk(0x40000, {0x48, 0x48, 0x48, 0x48})); // effective B
    auto const r = st::library::compute_tune_diff(a, b);
    auto const lines = st::library::interpret_diff(a, b, r);
    // Effective state is identical (0x48), so no scaler-bump claim.
    for (auto const &l : lines) {
        REQUIRE(l.find("scaler bump") == std::string::npos);
    }
}

TEST_CASE("interpret_diff: B drops Requested Torque tables A carried",
          "[library][interpret]") {
    // Build A with Requested Torque patches at addresses inside two
    // tables; B has none. Provide a def-pack-equivalent table range
    // by constructing TableRange directly, then compute_tune_diff
    // with that ranges vector.
    std::vector<st::library::TableRange> ranges;
    ranges.push_back({"rt_a", "Requested Torque A", "Torque",
                      0x50000, 0x50100});
    ranges.push_back({"rt_b", "Requested Torque B", "Torque",
                      0x50100, 0x50200});

    st::library::DecodedPtm a, b;
    a.patches.push_back(mk(0x50010, std::vector<std::uint8_t>(64, 0x11)));
    a.patches.push_back(mk(0x50110, std::vector<std::uint8_t>(64, 0x22)));

    auto const r = st::library::compute_tune_diff(a, b, ranges);
    auto const lines = st::library::interpret_diff(a, b, r);
    REQUIRE(any_contains(lines, "Requested Torque"));
}

TEST_CASE("interpret_diff: B adds Wastegate edits A didn't carry — boost-up layer",
          "[library][interpret]") {
    std::vector<st::library::TableRange> ranges;
    ranges.push_back({"wg_i", "Wastegate Duty Initial", "Boost",
                      0x60000, 0x60100});

    st::library::DecodedPtm a, b;
    b.patches.push_back(mk(0x60010, std::vector<std::uint8_t>(64, 0x55)));

    auto const r = st::library::compute_tune_diff(a, b, ranges);
    auto const lines = st::library::interpret_diff(a, b, r);
    REQUIRE(any_contains(lines, "Wastegate Duty"));
    REQUIRE(any_contains(lines, "boost-up"));
}

#include "st/library/atlas.hpp"

TEST_CASE("interpret_inspect: atlas anchor prose replaces hardcoded HPFP rule",
          "[library][interpret][atlas]") {
    auto const atlas_r = st::library::Atlas::load_from_string(R"toml(
[atlas]
schema_version = 1
generated = "2026-06-13"
platform = "subaru.va.wrx.fa20dit"
primary_cid = "LF79103P"

[[anchor]]
id = "hpfp_cluster"
display_name = "HPFP cluster"
rom_offset_start = 301976  # 0x49B98
rom_offset_end = 301992    # 0x49BA8
length = 16
prose = "ATLAS-PROSE-FOR-HPFP"
)toml");
    REQUIRE(atlas_r.has_value());
    auto const atlas = *atlas_r;

    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x49B9E, {0x01, 0x02, 0x03}));
    std::vector<st::library::TableHit> hits;

    // No atlas → hardcoded fallback fires.
    auto const lines_legacy = st::library::interpret_inspect(decoded, hits);
    REQUIRE(any_contains(lines_legacy, "HPFP cluster"));
    REQUIRE(any_contains(lines_legacy, "FA24 cam-lobe"));

    // With atlas → atlas prose replaces the hardcoded one.
    auto const lines_atlas = st::library::interpret_inspect(decoded, hits, &atlas);
    REQUIRE(any_contains(lines_atlas, "ATLAS-PROSE-FOR-HPFP"));
    // The hardcoded prose must NOT also fire (otherwise the user sees two
    // anchor sentences for the same offset).
    for (auto const &l : lines_atlas) {
        REQUIRE(l.find("FA24 cam-lobe") == std::string::npos);
    }
}

TEST_CASE("interpret_inspect: atlas safety-pair flag fires when LHS touched without RHS",
          "[library][interpret][atlas]") {
    auto const atlas_r = st::library::Atlas::load_from_string(R"toml(
[atlas]
schema_version = 1
generated = "2026-06-13"
platform = "subaru.va.wrx.fa20dit"
primary_cid = "LF79103P"

[[safety_pair]]
id = "boost_without_wastegate"
title = "Boost Target / Wastegate authority"
severity = "high"
lhs_patterns = ["Boost Target Main"]
rhs_patterns = ["Wastegate Duty Maximum", "Wastegate Duty Initial"]
rationale = "PI controller saturates without duty headroom."
)toml");
    REQUIRE(atlas_r.has_value());
    auto const atlas = *atlas_r;

    st::library::DecodedPtm decoded;
    decoded.patches.push_back(mk(0x20000, {0x00}));
    std::vector<st::library::TableHit> hits;
    // LHS touched, RHS NOT touched → flag fires.
    hits.push_back(mk_hit("bt", "Boost Target Main", 1, 256));
    auto const lines = st::library::interpret_inspect(decoded, hits, &atlas);
    REQUIRE(any_contains(lines, "Safety-pair flag"));
    REQUIRE(any_contains(lines, "high"));
    REQUIRE(any_contains(lines, "boost_without_wastegate"));

    // Add the RHS to balance → flag clears.
    hits.push_back(mk_hit("wgm", "Wastegate Duty Maximum", 1, 192));
    auto const lines_balanced = st::library::interpret_inspect(decoded, hits, &atlas);
    for (auto const &l : lines_balanced) {
        REQUIRE(l.find("Safety-pair flag") == std::string::npos);
    }
}
