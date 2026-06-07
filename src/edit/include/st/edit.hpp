// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_EDIT_HPP
#define ST_EDIT_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace st::edit {

// Inclusive rectangular selection over a TableData's values grid.
// For a 1D table (one row), set r_start = r_end = 0.
struct Rect {
    std::size_t r_start{0};
    std::size_t r_end{0};
    std::size_t c_start{0};
    std::size_t c_end{0};

    [[nodiscard]] constexpr std::size_t rows() const noexcept {
        return r_end >= r_start ? r_end - r_start + 1 : 0;
    }

    [[nodiscard]] constexpr std::size_t cols() const noexcept {
        return c_end >= c_start ? c_end - c_start + 1 : 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return rows() == 0 || cols() == 0;
    }
};

// Build a Rect covering every cell in the table. Convenient for "edit the
// whole table" operations.
[[nodiscard]] Rect whole_table(Definition::TableData const &td) noexcept;

// ---- Cell operations ----------------------------------------------------
//
// Every operation:
//   * mutates `td.values` in place
//   * returns OutOfRange if the rect would step past the grid
//   * returns InvalidArgument if the rect is empty (start > end)
//
// Operations work in *scaled engineering units*. Converting back to raw bytes
// for ROM writeback is the responsibility of a future projection layer.

[[nodiscard]] Status set_cells(Definition::TableData &td, Rect r, double value);
[[nodiscard]] Status add_cells(Definition::TableData &td, Rect r, double delta);
[[nodiscard]] Status multiply_cells(Definition::TableData &td, Rect r, double factor);

// Multiply every cell in the rect by (1 + pct/100). pct=10 means "+10%".
[[nodiscard]] Status percent_scale_cells(Definition::TableData &td, Rect r, double pct);

// Box-blur the selection: each cell becomes the average of its and its
// neighbors' values, clamped to the selection's edges. Repeat `iterations`
// times. Pure smoothing — does not change cells outside the rect.
[[nodiscard]] Status smooth_cells(Definition::TableData &td, Rect r, int iterations = 1);

// Bilinear interpolation across the rect using the rect's corners as anchors.
// For a 1D selection (rows() == 1), this is plain linear interpolation
// between the two endpoint columns; same when cols() == 1. For a single cell
// (rows() == cols() == 1), this is a no-op.
[[nodiscard]] Status interpolate_cells(Definition::TableData &td, Rect r);

// ---- Snapshots & undo/redo ----------------------------------------------
//
// A Snapshot captures the values of a rectangular region of a TableData. It
// is the unit of "what was here before the edit" and "what is here after the
// edit." snapshot() + restore() are the only knobs the undo/redo stack
// needs.

struct Snapshot {
    Rect rect{};
    std::vector<std::vector<double>> values;
};

[[nodiscard]] Result<Snapshot> snapshot(Definition::TableData const &td, Rect r);
[[nodiscard]] Status restore(Definition::TableData &td, Snapshot const &s);

// One logical change to one table — the original rect-scoped form.
struct TableEdit {
    std::string table_id;
    Snapshot before;
    Snapshot after;
};

// One logical change to one or more raw ROM bytes. Used for edits that
// don't map onto a Definition table — DTC enable-bits, calibration ID
// tweaks, future scalar-not-in-table parameters. Multi-change so that a
// user gesture touching N bytes (e.g. `--code P0420,P0430`) records as
// one undoable unit. Apply/revert lives in the project layer because
// ByteEdit is rom-bytes against an absolute address, not table-relative.
struct ByteEdit {
    struct Change {
        std::size_t address{0};
        std::uint8_t before{0};
        std::uint8_t after{0};
    };
    std::vector<Change> changes;
};

// An Edit records one logical change. The History owns a sequence of
// these and lets the caller walk backwards (undo) or forwards (redo)
// without caring whether the underlying op was rect-scoped (TableEdit)
// or byte-scoped (ByteEdit). The caller dispatches on the payload and
// applies the appropriate side — History is a record-keeper, not a model.

struct Edit {
    std::string description;
    // Optional transaction tag — empty for a normal single-step edit.
    // Set when a higher-level workflow (e.g. the FA24 swap modal) records
    // a batch of edits that should be undone as one unit. The tag is an
    // opaque string the caller chooses (typically the workflow id like
    // "fa24_swap"); History::undo_while_tag walks back across every
    // adjacent edit carrying the same tag. Persisted to edits.toml as
    // an optional `tag` field; absent / empty reads as a normal edit.
    std::string tag;
    std::variant<TableEdit, ByteEdit> payload{TableEdit{}};

    [[nodiscard]] bool is_table() const noexcept {
        return std::holds_alternative<TableEdit>(payload);
    }
    [[nodiscard]] bool is_byte() const noexcept {
        return std::holds_alternative<ByteEdit>(payload);
    }
    [[nodiscard]] TableEdit const *as_table() const noexcept {
        return std::get_if<TableEdit>(&payload);
    }
    [[nodiscard]] ByteEdit const *as_byte() const noexcept {
        return std::get_if<ByteEdit>(&payload);
    }

    [[nodiscard]] static Edit table(std::string table_id, Snapshot before, Snapshot after,
                                    std::string description) {
        return Edit{
            std::move(description), std::string{},
            TableEdit{std::move(table_id), std::move(before), std::move(after)},
        };
    }
    [[nodiscard]] static Edit bytes(std::vector<ByteEdit::Change> changes,
                                    std::string description) {
        return Edit{std::move(description), std::string{}, ByteEdit{std::move(changes)}};
    }
    [[nodiscard]] Edit with_tag(std::string t) && {
        tag = std::move(t);
        return std::move(*this);
    }
};

class History {
public:
    // Push a new edit. Drops any forward (redo) entries past the current
    // cursor — branching the history.
    void record(Edit e);

    [[nodiscard]] bool can_undo() const noexcept {
        return cursor_ > 0;
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return cursor_ < edits_.size();
    }

    // Step the cursor back by one. Returns a pointer to the Edit that the
    // caller should restore from .before, or nullptr if there's nothing to
    // undo. The pointer is valid until the next record()/clear() call.
    [[nodiscard]] Edit const *undo() noexcept;

    // Walk the cursor back across every adjacent edit at the head of the
    // applied range that carries the given non-empty tag. Returns the
    // edits in undo order (newest first) so the caller can restore each
    // from its .before snapshot in sequence. The pointers are valid
    // until the next record()/clear() call.
    //
    // Used by workflow modals (FA24 swap, future Stage1→2 step) where
    // a single user-visible "Revert all" reverses the whole batch.
    // Only contiguous tag-matching edits at the head of the applied
    // range are walked back — if the user has added unrelated edits
    // after the workflow batch, undo_while_tag stops at the first
    // non-match rather than peeling through them. Empty tag is a no-op.
    [[nodiscard]] std::vector<Edit const *> undo_while_tag(std::string_view tag) noexcept;

    // Step the cursor forward by one. Returns a pointer to the Edit to
    // restore from .after, or nullptr.
    [[nodiscard]] Edit const *redo() noexcept;

    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return edits_.size();
    }
    [[nodiscard]] std::size_t cursor() const noexcept {
        return cursor_;
    }

    // Read-only access to the underlying edit list. Useful for persistence
    // layers that need to serialize the whole record without walking via
    // undo/redo. Index order matches insertion order.
    [[nodiscard]] std::vector<Edit> const &records() const noexcept {
        return edits_;
    }

    // Restore a History from an already-known sequence of edits and a
    // cursor position. Existing contents are discarded. Used by loaders
    // (e.g. .stune project files).
    void load(std::vector<Edit> edits, std::size_t cursor) noexcept;

private:
    std::vector<Edit> edits_;
    std::size_t cursor_{0};
};

} // namespace st::edit

#endif // ST_EDIT_HPP
