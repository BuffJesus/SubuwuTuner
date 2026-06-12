// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::TableRange / TableHit + helpers to map a .ptm patch's
// rom_offset to the calibration table whose footprint contains it.
//
// Built on top of a loaded `st::Definition` and the `PatchEntry` records
// produced by `st::library::decode_ptm_xml`. Used by the CLI's
// `ptm inspect / list-patches / import / diff --by-table` subcommands
// to render table names alongside raw addresses.
//
// Why this lives in `st::library` rather than `st::defs`:
// The cross-reference is a feature of the .ptm → project translation,
// not a property of the def itself. Keeping it in library means
// `st::defs` stays slim and a non-.ptm consumer doesn't pay for the
// PatchEntry-keyed aggregation surface.

#ifndef ST_LIBRARY_TABLE_MAPPING_HPP
#define ST_LIBRARY_TABLE_MAPPING_HPP

#include "st/defs.hpp"
#include "st/library/patch_decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace st::library {

// One calibration table's footprint in the ROM. Built by
// `compute_table_ranges` from a loaded `st::Definition`. Ranges are
// half-open: [start, end_exclusive).
struct TableRange {
    std::string id;
    std::string name;
    std::string category;
    std::size_t start{0};
    std::size_t end_exclusive{0};
};

// For each table in `def`, multiply axis lengths × bytes-per-cell to
// get the table's full byte range. Tables without axis_x default to
// cols=1 (a scalar parameter); same for axis_y / axis_z. Tables that
// resolve to a zero-byte footprint are silently dropped from the
// result (rare; usually a malformed pack). Returned ranges are sorted
// ascending by `start` for deterministic iteration and a future
// binary-search variant.
[[nodiscard]] std::vector<TableRange>
compute_table_ranges(st::Definition const &def);

// Find the table whose range contains `rom_offset`. Most-specific
// (smallest containing range) match — mirrors the architectural
// classifier's policy for overlapping ranges (a scalar parameter
// inside a larger table block, for instance). Returns nullptr when no
// table contains the offset.
[[nodiscard]] TableRange const *
find_table_for(std::vector<TableRange> const &ranges,
               std::size_t rom_offset) noexcept;

// One row of the "top tables modified" view in `ptm inspect`.
struct TableHit {
    std::string table_id;
    std::string table_name;
    std::uint32_t patch_count{0};
    std::uint64_t total_bytes{0};
};

// Aggregate patches by the table their rom_offset falls inside.
// Patches that don't map to any table are silently dropped from the
// result — they still show up in the architectural-layer summary, but
// have no table-level home. Output is sorted by patch_count descending
// (ties broken by total_bytes descending) so the heaviest-impact
// tables surface first.
[[nodiscard]] std::vector<TableHit>
aggregate_table_hits(DecodedPtm const &decoded,
                     std::vector<TableRange> const &ranges);

} // namespace st::library

#endif // ST_LIBRARY_TABLE_MAPPING_HPP
