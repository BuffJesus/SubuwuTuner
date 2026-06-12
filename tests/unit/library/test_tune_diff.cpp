// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unit tests for st::library::compute_tune_diff — the shared per-layer
// + per-table delta math used by `subuwutuner-cli ptm diff` and the
// GUI diff modal.

#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/library/tune_diff.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

st::library::PatchEntry mk(std::uint32_t offset, std::vector<std::uint8_t> bytes) {
    st::library::PatchEntry p;
    p.rom_offset = offset;
    p.length = static_cast<std::uint32_t>(bytes.size());
    p.bytes = std::move(bytes);
    return p;
}

st::library::PatchEntry mk_ram(std::uint32_t rom, std::int32_t ram,
                                std::vector<std::uint8_t> bytes) {
    auto p = mk(rom, std::move(bytes));
    p.ram_offset = ram;
    return p;
}

} // namespace

TEST_CASE("compute_tune_diff: identical tunes diff to all-zero",
          "[library][tune_diff]") {
    st::library::DecodedPtm a, b;
    a.patches.push_back(mk(0x1000, {1, 2, 3, 4}));
    a.patches.push_back(mk(0x2000, {5, 6, 7, 8}));
    b.patches = a.patches;

    auto const r = st::library::compute_tune_diff(a, b);
    REQUIRE(r.a_total_patches == 2);
    REQUIRE(r.b_total_patches == 2);
    REQUIRE(r.shared_patches == 2);
    REQUIRE(r.a_only_patches == 0);
    REQUIRE(r.b_only_patches == 0);
    REQUIRE(r.changed_at_shared == 0);
    REQUIRE(r.changed_bytes == 0);
}

TEST_CASE("compute_tune_diff: a_only + b_only + changed buckets distinct",
          "[library][tune_diff]") {
    st::library::DecodedPtm a, b;
    // 0x1000 — shared, identical
    a.patches.push_back(mk(0x1000, {1, 2, 3, 4}));
    b.patches.push_back(mk(0x1000, {1, 2, 3, 4}));
    // 0x2000 — shared, different
    a.patches.push_back(mk(0x2000, {10, 20, 30, 40}));
    b.patches.push_back(mk(0x2000, {10, 99, 30, 40})); // 1 byte differs
    // 0x3000 — A only
    a.patches.push_back(mk(0x3000, {1, 2, 3}));
    // 0x4000 — B only
    b.patches.push_back(mk(0x4000, {5, 5, 5, 5, 5}));

    auto const r = st::library::compute_tune_diff(a, b);
    REQUIRE(r.a_total_patches == 3);
    REQUIRE(r.b_total_patches == 3);
    REQUIRE(r.shared_patches == 2);
    REQUIRE(r.a_only_patches == 1);
    REQUIRE(r.b_only_patches == 1);
    REQUIRE(r.changed_at_shared == 1);
    REQUIRE(r.changed_bytes == 1);
    REQUIRE(r.a_only_bytes == 3);
    REQUIRE(r.b_only_bytes == 5);
}

TEST_CASE("compute_tune_diff: length-mismatch trailing bytes count as changed",
          "[library][tune_diff]") {
    st::library::DecodedPtm a, b;
    // Same offset, A is 4 bytes, B is 6 bytes; first 4 bytes equal.
    a.patches.push_back(mk(0x1000, {1, 2, 3, 4}));
    b.patches.push_back(mk(0x1000, {1, 2, 3, 4, 5, 6}));

    auto const r = st::library::compute_tune_diff(a, b);
    REQUIRE(r.shared_patches == 1);
    REQUIRE(r.changed_at_shared == 1);
    // 2 trailing bytes in B beyond A's length.
    REQUIRE(r.changed_bytes == 2);
}

TEST_CASE("compute_tune_diff: per-layer breakdown populated + sorted by impact",
          "[library][tune_diff]") {
    st::library::DecodedPtm a, b;
    // 0x1000 → MainCalibration region in default LF79103P map (0x10000+
    // is the main cal region). Use 0x18000.
    a.patches.push_back(mk(0x18000, {1, 2, 3, 4, 5, 6, 7, 8}));
    // 0x60010 → PrimaryCode region (0x60000..0x80000).
    a.patches.push_back(mk(0x60010, {1, 2}));
    // Same in B — no change.
    b.patches = a.patches;
    // B adds a tuner-addition patch in 0x080000..0x10000 range.
    b.patches.push_back(mk(0x9000, {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}));

    auto const r = st::library::compute_tune_diff(a, b);
    REQUIRE_FALSE(r.by_layer.empty());
    // The b_only patch at 0x9000 contributes 12 bytes; that layer
    // should appear first (sorted by impact descending).
    REQUIRE(r.by_layer.front().b_only_bytes >= 12U);
}

TEST_CASE("compute_tune_diff: identical tunes with duplicate rom_offset diff to zero",
          "[library][tune_diff]") {
    // Real COBB tunes emit clear-then-set byte pairs at hot offsets — 2
    // patches with the same `rom_offset` but different `ram_offset` and
    // bytes. Stage1 91 v401.ptm has 78 such pairs. Pre-fix
    // compute_tune_diff used `std::map<rom_offset, idx>` which silently
    // dropped the second half of each pair, then misclassified the
    // first half as `changed_at_shared` by comparing it against the
    // mapped-over second half. Same bug pattern as a251f47 fixed in
    // `ptm verify`; same fix shape (key on (rom, ram) tuple).
    st::library::DecodedPtm a, b;
    // Clear-then-set pair at rom=0x10000 (MainCalibration). Both tunes
    // emit the exact same 2 patches — the diff must report zero
    // changes.
    a.patches.push_back(mk_ram(0x10000, 0, {0, 0, 0, 0}));
    a.patches.push_back(mk_ram(0x10000, 16, {0xAB, 0xCD, 0xEF, 0x12}));
    b.patches = a.patches;

    auto const r = st::library::compute_tune_diff(a, b);
    REQUIRE(r.a_total_patches == 2);
    REQUIRE(r.b_total_patches == 2);
    REQUIRE(r.shared_patches == 2);
    REQUIRE(r.a_only_patches == 0);
    REQUIRE(r.b_only_patches == 0);
    REQUIRE(r.changed_at_shared == 0);
    REQUIRE(r.changed_bytes == 0);
}

TEST_CASE("compute_tune_diff: duplicate rom_offset — one side missing the set half",
          "[library][tune_diff]") {
    // A has the full clear-then-set pair; B only has the clear half.
    // Correct: 1 shared (the clear half byte-identical), 1 a_only (the
    // set half), 0 b_only, 0 changed_at_shared. Pre-fix:
    // a_by_offset[0x10000] = 1 (the set half); b_by_offset[0x10000] = 0.
    // Iterating A's two patches, both look up b[0] (clear half). The
    // clear half matches its mirror; the set half is compared against
    // b's clear half and bytes differ → phantom `changed_at_shared`.
    st::library::DecodedPtm a, b;
    a.patches.push_back(mk_ram(0x10000, 0, {0, 0, 0, 0}));
    a.patches.push_back(mk_ram(0x10000, 16, {0xAB, 0xCD, 0xEF, 0x12}));
    b.patches.push_back(mk_ram(0x10000, 0, {0, 0, 0, 0}));

    auto const r = st::library::compute_tune_diff(a, b);
    REQUIRE(r.a_total_patches == 2);
    REQUIRE(r.b_total_patches == 1);
    REQUIRE(r.shared_patches == 1);
    REQUIRE(r.a_only_patches == 1);
    REQUIRE(r.b_only_patches == 0);
    REQUIRE(r.changed_at_shared == 0);
    REQUIRE(r.changed_bytes == 0);
    // The orphaned set half contributes its full byte count to a_only.
    REQUIRE(r.a_only_bytes == 4);
}

TEST_CASE("compute_tune_diff: per-table breakdown only when ranges provided",
          "[library][tune_diff]") {
    st::library::DecodedPtm a, b;
    a.patches.push_back(mk(0x10100, {1, 2, 3, 4}));
    b.patches.push_back(mk(0x10100, {5, 6, 7, 8}));

    // Without ranges, by_table stays empty.
    auto const r_no = st::library::compute_tune_diff(a, b);
    REQUIRE(r_no.by_table.empty());

    // With a covering range, by_table populates.
    std::vector<st::library::TableRange> ranges;
    ranges.push_back({"t_throttle", "Throttle Target", "fuel", 0x10100, 0x10110});
    auto const r_yes = st::library::compute_tune_diff(a, b, ranges);
    REQUIRE(r_yes.by_table.size() == 1);
    REQUIRE(r_yes.by_table[0].table_id == "t_throttle");
    REQUIRE(r_yes.by_table[0].changed_at_shared == 1);
}
