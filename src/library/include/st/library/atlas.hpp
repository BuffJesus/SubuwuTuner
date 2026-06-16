// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::Atlas — machine-readable consolidated tuning knowledge.
//
// The Atlas is a TOML file shipped under `fixtures/tuner_atlas/` that
// consolidates per-table semantics, tuner-cluster fingerprints,
// corpus-derived safety pairs, and RE-anchored heuristic constants
// (HPFP cluster, MAF Sensor Scale, etc.) into a single load.
//
// Built by `findings/tuning-knowledge-2026-06-13/atlas-build/build_atlas.py`
// from the markdown knowledge base. Schema versioned:
// - v1 covered a single primary CID (LF79103P / FA20DIT)
// - v2 (current) adds optional per-table multi-CID address mappings and
//   a top-level [[cid_coverage]] summary. v2 is backwards-compatible:
//   the v2 loader reads v1 atlases unchanged, and a v2 atlas with no
//   cid_address entries is semantically identical to a v1 atlas.
//
// Consumers:
// - `st::library::interpret_inspect` / `interpret_diff` (atlas-driven
//   heuristic prose, replacing hardcoded rules)
// - `subuwutuner-cli tuner-atlas` (browse from CLI)
// - GUI (future: per-table context badges in the editor)

#ifndef ST_LIBRARY_ATLAS_HPP
#define ST_LIBRARY_ATLAS_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::library {

// Per-CID address binding for a single calibration table. v2-only; v1
// atlases have no such entries.
struct AtlasTablePerCid {
    std::string cid;
    std::uint32_t address{0};
    std::uint32_t size_bytes{0};
};

struct AtlasTable {
    std::string id;
    std::string display_name;
    std::uint32_t address{0};
    std::uint32_t size_bytes{0};
    std::string dimension; // "1d" / "2d" / "3d" / ""
    std::string storage;   // "uint8" / "uint16" / "float" / ""
    std::string units;
    std::string purpose;
    std::string fa24_portability; // "portable" / "needs_retune" / "fa20_specific" / "unclear"
    bool needs_def_promotion{false};
    bool common_core{false};
    bool high_variance{false};
    std::vector<std::string> clusters;
    std::vector<std::string> co_edits;
    // v2+: the CID this record's `address` / `size_bytes` apply to. Empty
    // for v1 atlases — callers should fall back to Atlas::primary_cid().
    std::string primary_cid;
    // v2+: optional per-CID address bindings for this same logical table.
    // Empty when the table is exclusive to `primary_cid` or when no
    // cross-CID mapping has been recorded yet.
    std::vector<AtlasTablePerCid> cid_addresses;
};

struct AtlasTuner {
    std::string id;
    std::string display_name;
    std::uint32_t patches_typical{0};
    std::uint32_t mean_tables_touched{0};
    std::string philosophy;
    std::vector<std::string> signature_tables;
    std::vector<std::string> skipped_tables;
};

struct AtlasSafetyPair {
    std::string id;
    std::string title;
    std::string severity; // "high" / "med" / "low"
    std::vector<std::string> lhs_patterns;
    std::vector<std::string> rhs_patterns;
    std::string rationale;
};

struct AtlasAnchor {
    std::string id;
    std::string display_name;
    std::uint32_t rom_offset_start{0};
    std::uint32_t rom_offset_end{0};
    std::uint32_t length{0};
    std::string prose;
};

// v2+: per-CID coverage summary. One row per CID that the atlas knows
// anything about. `table_count` is the number of [[table]] records that
// have an address binding for this CID (either the primary `address`
// field or an entry in `cid_addresses`). `note` is free-form prose
// (e.g. "skipped: RH850 Gen-B uses RAM pointers, not flash offsets").
struct AtlasCidCoverage {
    std::string cid;
    std::uint32_t table_count{0};
    std::string note;
};

class Atlas {
public:
    [[nodiscard]] static int schema_version_supported() noexcept {
        return 2;
    }

    // Load from a TOML file on disk. Returns ParseError on malformed
    // TOML, UnsupportedVersion when the schema is newer than we know,
    // IoFailure on read errors.
    [[nodiscard]] static Result<Atlas>
    load_from_file(std::filesystem::path const &path);

    // Load from an in-memory TOML string. Same error mapping as
    // load_from_file.
    [[nodiscard]] static Result<Atlas> load_from_string(std::string_view toml);

    // Header fields.
    [[nodiscard]] int schema_version() const noexcept {
        return schema_version_;
    }
    [[nodiscard]] std::string const &generated() const noexcept {
        return generated_;
    }
    [[nodiscard]] std::string const &platform() const noexcept {
        return platform_;
    }
    [[nodiscard]] std::string const &primary_cid() const noexcept {
        return primary_cid_;
    }

    // Bulk accessors.
    [[nodiscard]] std::vector<AtlasTable> const &tables() const noexcept {
        return tables_;
    }
    [[nodiscard]] std::vector<AtlasTuner> const &tuners() const noexcept {
        return tuners_;
    }
    [[nodiscard]] std::vector<AtlasSafetyPair> const &safety_pairs() const noexcept {
        return safety_pairs_;
    }
    [[nodiscard]] std::vector<AtlasAnchor> const &anchors() const noexcept {
        return anchors_;
    }
    // v2+: empty on v1 atlases.
    [[nodiscard]] std::vector<AtlasCidCoverage> const &cid_coverage() const noexcept {
        return cid_coverage_;
    }

    // Lookup by id. Returns nullptr when no match.
    [[nodiscard]] AtlasTable const *find_table(std::string_view id) const noexcept;
    [[nodiscard]] AtlasTuner const *find_tuner(std::string_view id) const noexcept;
    [[nodiscard]] AtlasSafetyPair const *find_safety_pair(std::string_view id) const noexcept;
    [[nodiscard]] AtlasAnchor const *find_anchor(std::string_view id) const noexcept;
    [[nodiscard]] AtlasCidCoverage const *find_cid_coverage(std::string_view cid) const noexcept;

    // Resolve a (table_id, cid) pair to a per-CID address binding.
    //
    // Lookup order:
    //   1. If `cid` matches the table's `primary_cid` (or, when that is
    //      empty as on v1 atlases, the atlas-level primary_cid()), an
    //      AtlasTablePerCid synthesised from the table's own
    //      `address` / `size_bytes` is returned.
    //   2. Otherwise an entry in `cid_addresses` is searched.
    //
    // Returns std::nullopt when no binding for the requested CID exists.
    // The return is by value so the synthesised case has somewhere to
    // live; callers that want a pointer into `cid_addresses` should use
    // the table accessor directly.
    [[nodiscard]] std::optional<AtlasTablePerCid>
    find_table_for_cid(std::string_view table_id,
                       std::string_view cid) const noexcept;

    // Find an anchor whose [rom_offset_start, rom_offset_end) range
    // contains the given ROM offset. Returns nullptr when no anchor
    // covers it. Used by interpret_inspect to surface anchor prose
    // when a patch lands inside a known surface.
    [[nodiscard]] AtlasAnchor const *anchor_for_offset(std::uint32_t rom_offset) const noexcept;

private:
    Atlas() = default;

    int schema_version_{0};
    std::string generated_;
    std::string platform_;
    std::string primary_cid_;
    std::vector<AtlasTable> tables_;
    std::vector<AtlasTuner> tuners_;
    std::vector<AtlasSafetyPair> safety_pairs_;
    std::vector<AtlasAnchor> anchors_;
    std::vector<AtlasCidCoverage> cid_coverage_;
};

} // namespace st::library

#endif // ST_LIBRARY_ATLAS_HPP
