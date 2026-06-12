// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::compute_tune_diff — structured delta between two decoded
// .ptm tunes. Shared between the CLI `ptm diff` and the GUI diff modal.
//
// Bucket strategy:
//   * a_total / b_total / shared / a_only / b_only / changed_at_shared
//     are envelope counts at the rom_offset level.
//   * shared patches with identical byte content don't count toward
//     changed_at_shared; only bytes-different ones do.
//   * The byte-wise `changed_bytes` tally counts per-index differences
//     (p.bytes[i] != pb.bytes[i]) plus any trailing length mismatch.
//
// When a def-pack's table ranges are passed in, the by-table breakdown
// also populates. Patches that don't fall in any range still count
// toward the per-layer view but are dropped from the per-table view.

#ifndef ST_LIBRARY_TUNE_DIFF_HPP
#define ST_LIBRARY_TUNE_DIFF_HPP

#include "st/devices/ap3/architectural_classifier.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace st::library {

struct LayerDiff {
    st::devices::ap3::Layer layer{st::devices::ap3::Layer::Uncharacterized};
    std::uint32_t a_only_patches{0};
    std::uint32_t b_only_patches{0};
    std::uint32_t shared_patches{0};
    std::uint32_t changed_at_shared{0};
    std::uint64_t a_only_bytes{0};
    std::uint64_t b_only_bytes{0};
    std::uint64_t changed_bytes{0};
};

struct TableDiff {
    std::string table_id;
    std::string table_name;
    std::uint32_t a_only_patches{0};
    std::uint32_t b_only_patches{0};
    std::uint32_t shared_patches{0};
    std::uint32_t changed_at_shared{0};
    std::uint64_t a_only_bytes{0};
    std::uint64_t b_only_bytes{0};
    std::uint64_t changed_bytes{0};
};

struct TuneDiffResult {
    std::uint32_t a_total_patches{0};
    std::uint32_t b_total_patches{0};
    std::uint32_t shared_patches{0};
    std::uint32_t a_only_patches{0};
    std::uint32_t b_only_patches{0};
    std::uint32_t changed_at_shared{0};
    std::uint64_t a_only_bytes{0};
    std::uint64_t b_only_bytes{0};
    std::uint64_t changed_bytes{0};
    std::vector<LayerDiff> by_layer;
    std::vector<TableDiff> by_table; // populated when table_ranges non-empty
};

// `table_ranges` is optional — pass an empty vector to skip the
// per-table view. The layer-classifier always runs against the
// default LF79103P region map.
[[nodiscard]] TuneDiffResult
compute_tune_diff(DecodedPtm const &a, DecodedPtm const &b,
                  std::vector<TableRange> const &table_ranges = {});

} // namespace st::library

#endif // ST_LIBRARY_TUNE_DIFF_HPP
