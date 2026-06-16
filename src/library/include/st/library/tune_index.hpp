// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::TuneIndex — C++ loader for the library-index TOML that
// `tools/library_inventory/inventory.py` produces. Surfaces enough
// metadata to drive the GUI Tune Library panel + the AP browser's
// `/backupcksum` cross-reference: lookup-by-MD5, filtered iteration,
// per-entry vendor / vehicle / patch_count / stage / variant.
//
// Format contract: see `tools/library_inventory/inventory.py` +
// `tools/library_inventory/README.md`. Schema is
// `subuwutuner.library-index.v1`; future schema bumps load behind a
// version gate. Missing optional fields default to empty strings.
//
// Loader is forward-tolerant: unknown TOML keys are ignored, unknown
// table arrays are skipped. Strict on the schema field — a missing or
// wrong schema string is a hard error so users can't accidentally
// point us at an unrelated TOML and get garbage results.
//
// This layer is hardware-free, cipher-free, and analyst-lane-free.
// Reads disk, returns data. The GUI / CLI compose lookups on top.

#ifndef ST_LIBRARY_TUNE_INDEX_HPP
#define ST_LIBRARY_TUNE_INDEX_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::library {

// One row in the library index for a `.ptm` tune file. Mirrors the
// Python script's PtmEntry shape but trimmed to what consumers need
// today. Unused-but-present fields in the TOML stay parseable; we
// just don't carry them in C++ until a consumer asks.
struct TuneIndexEntry {
    std::string path;          // absolute, OS-native (forward-slashed on Windows is fine)
    std::uint64_t size{0};
    std::uint64_t mtime{0};    // unix epoch seconds
    std::string md5;           // lowercase hex, 32 chars
    // Optional metadata — empty when the inventory's filename heuristic
    // couldn't infer. Cross-reference against st::library::Atlas for
    // canonical vendor / pattern identification when available.
    std::string vendor;        // "COBB", "NexGen", "Fehr", "NTM", etc.
    std::string stage;         // "Stage0", "Stage1", etc.
    std::string fuel_grade;    // "91", "93", "E85", etc.
    std::string variant;       // "WRK3", "v401", "BigSF", etc. or " / "-joined
    std::string unlocked_sibling; // path to the *_UNLOCKED twin if present
};

// One row for a `.stune` project, sparser than the .ptm row.
struct StuneIndexEntry {
    std::string path;
    std::string project_name;
    std::string ptm_vendor_id;
    std::string ptm_vehicle_id;
    std::string ptm_rom_sum;
    std::string source_bin;        // absolute path to source.bin
    std::string source_bin_md5;    // MD5 of source.bin
};

class TuneIndex {
public:
    // Load from a TOML file at the given path. Returns a populated
    // TuneIndex on success, a Result error on missing-file /
    // parse-failure / schema-mismatch. An empty index (no .ptm or
    // .stune rows) is success — the user just has no library yet.
    [[nodiscard]] static Result<TuneIndex> load_from_file(std::filesystem::path const &path);

    // Same but from an in-memory TOML string. Useful for tests.
    [[nodiscard]] static Result<TuneIndex> load_from_string(std::string_view toml_text);

    [[nodiscard]] std::vector<TuneIndexEntry> const &ptm_entries() const noexcept {
        return ptm_entries_;
    }
    [[nodiscard]] std::vector<StuneIndexEntry> const &stune_entries() const noexcept {
        return stune_entries_;
    }

    // Find a .ptm entry by content hash. Case-insensitive comparison;
    // the caller can pass either lower- or upper-case hex. Returns
    // nullptr if no entry matches. Pointer is valid for the lifetime
    // of this TuneIndex; do not retain across reloads.
    [[nodiscard]] TuneIndexEntry const *lookup_by_md5(std::string_view md5) const noexcept;

    // Filter helpers — return indices into ptm_entries() so callers
    // can keep their own selection state. Both are simple substring
    // (case-insensitive) matches against the obvious field. Empty
    // needle returns all entries.
    [[nodiscard]] std::vector<std::size_t>
    filter_by_name(std::string_view needle) const;
    [[nodiscard]] std::vector<std::size_t>
    filter_by_vendor(std::string_view vendor) const;

    // Bookkeeping — what the loader read out of the schema/header
    // fields. Useful for diagnostics ("loaded index from path X with
    // schema v1, N entries").
    [[nodiscard]] std::string const &schema() const noexcept { return schema_; }
    [[nodiscard]] std::filesystem::path const &source_path() const noexcept { return source_path_; }

    // Internal factory used by the load_from_* parsers. Public so the
    // free-function parser in tune_index.cpp can construct without a
    // friend declaration leaking the toml++ types into this header.
    // External callers should use load_from_file / load_from_string.
    [[nodiscard]] static TuneIndex construct_(std::string schema,
                                               std::filesystem::path source_path,
                                               std::vector<TuneIndexEntry> ptm_entries,
                                               std::vector<StuneIndexEntry> stune_entries);

private:
    TuneIndex() = default;

    std::string schema_;
    std::filesystem::path source_path_;
    std::vector<TuneIndexEntry> ptm_entries_;
    std::vector<StuneIndexEntry> stune_entries_;
};

// Conventional default path resolution for the library index. Used by
// the GUI panel + the doctor modal to find an existing index without
// requiring the user to type a path. Falls back through:
//   1. ST_LIBRARY_INDEX env var (absolute path to library_index.toml)
//   2. <library_root from config>/library_index.toml — once
//      st::config::Paths grows a library_root field. Today this is
//      not wired (analyst handoff Strategic Q A is settled; the C++
//      side will absorb library_root in a follow-up commit).
//   3. ./library_index.toml in the current working directory
// Returns std::nullopt when no resolution succeeds. Caller decides
// whether to prompt the user, render an empty-state, or fail.
[[nodiscard]] std::optional<std::filesystem::path> default_index_path();

} // namespace st::library

#endif // ST_LIBRARY_TUNE_INDEX_HPP
