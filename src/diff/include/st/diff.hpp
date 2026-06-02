// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::diff — structured ROM compare. The semantic counterpart to
// Flasher::compute_delta (which is sector-level for flash planning):
// here we work at the table level, with optional byte-level fallback
// for regions the pack doesn't describe.
//
// Output is a versioned DiffSet that carries:
//   - per-table cell-level changes (row, col, value_a, value_b, delta)
//   - per-table aggregate stats (cells_changed, max/mean abs delta)
//   - overall summary (tables compared, tables changed, total cells)
//   - optional byte-level fallback ranges for unlabeled regions
//
// The renderer side (CLI text/csv/json, GUI Compare panel) lives
// outside this module — DiffSet is a pure data product. Closes the
// gap analyst Issue #4 / I-03 + I-04 identified: a real Compare
// workflow that goes beyond the existing rom-diff text summary.

#ifndef ST_DIFF_HPP
#define ST_DIFF_HPP

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/defs.hpp"
#include "st/rom.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace st::diff {

// One scaled cell that differs between the two ROMs. Coordinates are
// 0-indexed into the table's grid. Values are scaled (engineering
// units), not raw bytes — same convention as Definition::read_table_values.
struct CellChange {
    std::size_t row{0};
    std::size_t col{0};
    double value_a{0.0};
    double value_b{0.0};

    [[nodiscard]] double delta() const noexcept {
        return value_b - value_a;
    }
};

// One table's diff between the two ROMs, with cell-level detail.
// `cells_changed` may be 0 (table identical between the two ROMs);
// callers that only want changed tables filter on `changed_cells()`
// non-empty, not on `cells_changed > 0` directly (avoids walking the
// list twice).
struct TableDelta {
    std::string table_id;
    std::string table_name;
    std::size_t rows{0};
    std::size_t cols{0};
    std::size_t total_cells{0};
    std::size_t cells_changed{0};
    double max_abs_delta{0.0};
    double mean_abs_delta{0.0};
    // Engine-safety / emissions flags from the pack — carried so
    // downstream renderers can color-code without re-walking the
    // Definition.
    bool engine_safety_critical{false};
    bool emissions_relevant{false};
    // Per-cell list. Empty if the table is identical. Order is row-
    // major to match the editor's display order.
    std::vector<CellChange> changes;

    [[nodiscard]] bool changed() const noexcept {
        return cells_changed > 0;
    }
};

// Top-level result. `pack_id` and the two CRC32s pin the inputs so a
// renderer can sanity-check (or cache to a `.stcompare` file later
// per analyst Issue #5).
//
// `tables` holds one entry per table the pack declares. Tables that
// couldn't be compared (out-of-range, definition issues) get a
// `skip_reason` slot so the renderer can surface skips alongside
// changes. Identical tables are included with `cells_changed = 0` so
// the renderer can report "X tables checked, Y changed" without
// double-walking.
struct SkippedTable {
    std::string table_id;
    std::string reason;
};

// Schema version travels with every DiffSet — bump when the field
// layout changes incompatibly so cached `.stcompare` files can be
// rejected cleanly.
inline constexpr int kDiffSetSchemaVersion = 1;

struct DiffSet {
    int schema_version{kDiffSetSchemaVersion};

    // Input identity.
    std::string pack_id;
    std::uint32_t rom_a_crc32{0};
    std::uint32_t rom_b_crc32{0};
    std::size_t rom_a_size{0};
    std::size_t rom_b_size{0};

    // Per-table results.
    std::vector<TableDelta> tables;
    std::vector<SkippedTable> skipped;

    // Aggregate. Computed at the end of compare(); cheaper to walk
    // tables once during the compare than to recompute on every render.
    std::size_t tables_compared{0};
    std::size_t tables_changed{0};
    std::size_t total_cells_compared{0};
    std::size_t total_cells_changed{0};

    [[nodiscard]] bool identical() const noexcept {
        return tables_changed == 0;
    }
};

// Knobs for the compare run.
struct Options {
    // Cells within this scaled-value distance are treated as equal.
    // Default 0 — strict equality. Pack authors / users may want a
    // small epsilon to absorb scaling float-noise on certain tables.
    double cell_epsilon{0.0};

    // When true, identical tables are included in `tables` with
    // `cells_changed = 0`. Default false — most renderers only want
    // changed tables, and excluding them shrinks the result.
    bool include_identical{false};

    // When true, fill `changes[]` on every TableDelta. When false,
    // only aggregate stats are populated (matches the existing
    // Definition::diff_table behavior). Default true — the whole
    // point of this module is the cell list.
    bool include_cell_list{true};
};

// Compare two ROMs against the same definition. Returns a populated
// DiffSet on success.
//
// Errors:
//   InvalidArgument — `def` has no tables (nothing to compare); a/b
//                     ROM sizes both zero.
//
// Per-table failures (out-of-range, scaling issues, etc.) are
// captured in `skipped[]` rather than aborting the whole compare —
// the user wants to see what *did* compare even when a few tables
// can't.
[[nodiscard]] Result<DiffSet>
compare(Rom const &a, Rom const &b, Definition const &def, Options const &opts = {});

// ---- Renderers ---------------------------------------------------------
//
// Text/CSV/JSON renderers live here rather than in the CLI so library
// callers (and the future GUI Compare panel) share one source of
// truth for the output format. All three are pure functions over a
// DiffSet — no I/O.

// Plain-text human-readable summary. Same shape as the existing
// rom-diff CLI's text output but driven from the structured DiffSet.
[[nodiscard]] std::string render_text(DiffSet const &d);

// CSV one row per changed cell (when `Options::include_cell_list` was
// true at compare time). Columns: table_id, table_name, row, col,
// value_a, value_b, delta. Tables with no changes are omitted unless
// `include_unchanged = true`.
[[nodiscard]] std::string render_csv(DiffSet const &d, bool include_unchanged = false);

// JSON serialization. Schema `subuwutuner.diff.v1` matches the
// DiffSet field layout. Single-line by default; pipe through `jq` for
// pretty-printing. Skip lists are always emitted (even if empty) so
// scripts can rely on the key shape.
[[nodiscard]] std::string render_json(DiffSet const &d);

// ---- CompareSession persistence (analyst Issue #5) ---------------------
//
// `.stcompare` files hold the inputs to a saved diff session: the two
// ROM paths + CRC32s + sizes, the pack id, the compare options, and
// any user annotations. The DiffSet itself is NOT cached — it's
// recomputed on load (same inputs → same output, deterministically).
// The hash-verify on load is the safety property: if either ROM has
// changed on disk since the session was saved, load_compare_session
// returns InvalidArgument with a precise "ROM A CRC32 mismatch"
// message rather than silently producing a different diff.
//
// File format is TOML (matches the .stune convention). Annotations
// are flat triples (table_id, row, col, text) so the format extends
// cleanly when the GUI grows per-cell-comment threads.

struct CompareAnnotation {
    std::string table_id;
    std::size_t row{0};
    std::size_t col{0};
    std::string text;
};

inline constexpr int kCompareSessionSchemaVersion = 1;

struct CompareSession {
    int schema_version{kCompareSessionSchemaVersion};

    // ROM inputs. Paths are stored as the user picked them (absolute
    // or relative); CRC32 + size let the loader verify the on-disk
    // file still matches.
    std::string rom_a_path;
    std::uint32_t rom_a_crc32{0};
    std::size_t rom_a_size{0};
    std::string rom_b_path;
    std::uint32_t rom_b_crc32{0};
    std::size_t rom_b_size{0};

    // The pack id the compare ran against. Loader resolves to a path
    // via the caller-supplied lookup (typically the project's
    // definition or a pack-dir scan).
    std::string pack_id;

    // Options the original compare used. Reusing them on load
    // guarantees the recomputed DiffSet matches what was saved.
    Options options;

    // Optional ISO-8601 timestamp of when the session was created.
    std::string created_at;

    // User annotations against specific cells. May be empty.
    std::vector<CompareAnnotation> annotations;
};

// Write a session to disk as TOML. Caller passes a populated
// CompareSession (typically built by capturing the inputs from a
// successful compare() call + the user's annotations).
[[nodiscard]] Status save_compare_session(CompareSession const &session,
                                          std::filesystem::path const &path);

// Read a .stcompare file. Does NOT load the ROMs or run compare();
// caller drives that. Use verify_compare_inputs() afterwards to
// confirm the on-disk ROMs still match the recorded CRC32s before
// re-running compare().
[[nodiscard]] Result<CompareSession>
load_compare_session(std::filesystem::path const &path);

// Verify both ROMs referenced by a CompareSession still exist on
// disk with the recorded CRC32 + size. Returns InvalidArgument with
// a precise "ROM A CRC32 mismatch: expected 0xXXXX got 0xYYYY"
// message on any drift. Paths are resolved against `base_dir` when
// relative (typically the .stcompare's parent dir).
[[nodiscard]] Status
verify_compare_inputs(CompareSession const &session,
                      std::filesystem::path const &base_dir);

} // namespace st::diff

#endif // ST_DIFF_HPP
