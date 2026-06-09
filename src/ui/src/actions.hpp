// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// User-action surface: undo/redo, copy/paste, the action router behind
// the unsaved-changes modal. `apply_op` is a template (definition in
// header) so each call site can pass a different op lambda. The
// non-template entries cover history navigation and clipboard plumbing.

#ifndef ST_UI_ACTIONS_HPP
#define ST_UI_ACTIONS_HPP

#include "app_state.hpp"

#include "st/audit.hpp"
#include "st/edit.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace st::ui {

// Snapshot, mutate, snapshot, writeback, record. If the writeback fails
// we restore the in-memory TableData so the rendered grid stays
// consistent with the ROM bytes — better than silently diverging.
//
// Templated so each call site can pass a closure that mutates the
// table over the active selection. Body lives here because the
// template's call sites span modal + panel TUs.
template<typename Op>
void apply_op(AppState &state, std::string label, Op &&op) {
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    // Edits route through the active slot (Issue #10 phase 3). The
    // working slot is the v1 default; an additional ROM is editable
    // with its own per-ROM history; source is immutable
    // (active_rom_mut returns nullptr). UI surfaces gate the call
    // sites for source already; this is the defense-in-depth check.
    st::Rom *target_rom = state.project->active_rom_mut();
    if (target_rom == nullptr) {
        state.status_msg = label +
                           ": this ROM is read-only "
                           "(switch View → Active ROM to an editable slot).";
        return;
    }
    auto &td = *state.current_table_data;
    auto const rect = state.selection.as_rect();

    auto before = st::edit::snapshot(td, rect);
    if (!before.has_value()) {
        state.status_msg = label + ": snapshot: " + before.error().to_string();
        return;
    }

    if (auto s = op(td, rect); !s.has_value()) {
        state.status_msg = label + ": " + s.error().to_string();
        return;
    }

    auto after = st::edit::snapshot(td, rect);
    if (!after.has_value()) {
        // op succeeded but post-snapshot failed — try to roll back td so the
        // view matches what's still on disk.
        (void)st::edit::restore(td, *before);
        state.status_msg = label + ": snapshot: " + after.error().to_string();
        return;
    }

    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl == nullptr) {
        (void)st::edit::restore(td, *before);
        state.status_msg = label + ": table missing from pack";
        return;
    }

    auto wb = state.project->definition().write_table_values(*target_rom, *tbl, td);
    if (!wb.has_value()) {
        (void)st::edit::restore(td, *before);
        state.status_msg = label + ": writeback: " + wb.error().to_string();
        return;
    }

    // Capture the label + cell count before moving `label` into the
    // history record — audit hook uses both. Cell count comes from
    // the rect inclusive on both ends.
    std::string const audited_label = label;
    std::size_t const audited_cells = rect.rows() * rect.cols();
    state.project->active_history().record(st::edit::Edit::table(
        state.selected_table_id, std::move(*before), std::move(*after), std::move(label)));
    state.status_msg.clear();
    state.dirty = true;
    // Audit the edit (analyst Issue #8). Per-toolbar-op resolution —
    // can be noisy when the user clicks +5% twenty times in a row, but
    // the audit panel's kind-chip filter (commit 2b12599) lets users
    // collapse the project.edit_committed group when they don't want
    // the noise. Better noise than blind spot for a tuning workflow.
    if (state.audit_log.has_value()) {
        std::vector<std::pair<std::string, std::string>> fields{
            {"table", state.selected_table_id},
            {"cells", std::to_string(audited_cells)}};
        // Include the active rom id when it's not the default so the
        // audit timeline doesn't conflate edits to working vs an
        // additional ROM under the same EditCommitted kind.
        if (!state.active_rom_id.empty() && state.active_rom_id != "working") {
            fields.emplace_back("rom", state.active_rom_id);
        }
        (void)state.audit_log->log(
            st::audit::EntryKind::EditCommitted, "ui.editor", audited_label,
            std::move(fields));
    }
}

// Sibling of apply_op for "I have a table-id but no user-selection."
// Reads the named table fresh from the active ROM, runs the op over
// the whole rect, writes back, and records the edit with an optional
// transaction tag so workflow modals (FA24 swap, future Stage1→2
// step) can group their writes for an atomic Revert All.
//
// Why a separate helper instead of teaching apply_op to operate on an
// explicit table: apply_op's contract is "act on the user's current
// selection," and that's what every call site in the toolbar +
// command palette + cell-editor expects. Splicing in an alternate
// "but if you pass a table_id, ignore the selection" path would make
// the existing call sites hard to reason about. A sibling helper
// reads cleanly at the call site (every workflow modal uses this
// variant exclusively) and keeps apply_op's invariants intact.
//
// The op receives the whole-table Rect — callers that want a sub-rect
// can apply the op manually before calling and pass a no-op. Most
// workflow edits are whole-table (set Engine Displacement to 2.4 L,
// scale every cell by 1.43, add ~10 deg to every cell), so the whole-
// table default fits the use case.
template<typename Op>
void apply_op_table(AppState &state, std::string label, std::string tag,
                    std::string const &table_id, Op &&op) {
    if (!state.project.has_value()) {
        return;
    }
    st::Rom *target_rom = state.project->active_rom_mut();
    if (target_rom == nullptr) {
        state.status_msg = label + ": this ROM is read-only "
                                   "(switch View → Active ROM to an editable slot).";
        return;
    }
    auto const &def = state.project->definition();
    auto const *tbl = def.find_table(table_id);
    if (tbl == nullptr) {
        state.status_msg = label + ": table '" + table_id + "' not in pack";
        return;
    }
    auto td_r = def.read_table_values(*target_rom, *tbl);
    if (!td_r.has_value()) {
        state.status_msg = label + ": read: " + td_r.error().to_string();
        return;
    }
    auto td = std::move(*td_r);
    auto const rect = st::edit::whole_table(td);

    auto before = st::edit::snapshot(td, rect);
    if (!before.has_value()) {
        state.status_msg = label + ": snapshot: " + before.error().to_string();
        return;
    }

    if (auto s = op(td, rect); !s.has_value()) {
        state.status_msg = label + ": " + s.error().to_string();
        return;
    }

    auto after = st::edit::snapshot(td, rect);
    if (!after.has_value()) {
        state.status_msg = label + ": snapshot: " + after.error().to_string();
        return;
    }

    auto wb = def.write_table_values(*target_rom, *tbl, td);
    if (!wb.has_value()) {
        state.status_msg = label + ": writeback: " + wb.error().to_string();
        return;
    }

    std::string const audited_label = label;
    std::size_t const audited_cells = rect.rows() * rect.cols();
    auto edit = st::edit::Edit::table(table_id, std::move(*before), std::move(*after),
                                      std::move(label));
    if (!tag.empty()) {
        edit = std::move(edit).with_tag(std::move(tag));
    }
    state.project->active_history().record(std::move(edit));
    state.status_msg.clear();
    state.dirty = true;

    if (state.audit_log.has_value()) {
        std::vector<std::pair<std::string, std::string>> fields{
            {"table", table_id},
            {"cells", std::to_string(audited_cells)}};
        if (!state.active_rom_id.empty() && state.active_rom_id != "working") {
            fields.emplace_back("rom", state.active_rom_id);
        }
        (void)state.audit_log->log(
            st::audit::EntryKind::EditCommitted, "ui.editor", audited_label,
            std::move(fields));
    }
}

void apply_history_step(AppState &state, st::edit::Edit const &edit, bool forward);
void paste_clipboard_at_cursor(AppState &state);
void reset_selection_to_source(AppState &state);
void do_undo(AppState &state);
void do_redo(AppState &state);
void do_copy_selection(AppState &state);

// Switch which ROM the GUI's read-side surfaces display (Issue #10
// active-ROM helper, shared between View → Active ROM and the
// command palette). Validates id, persists via save_metadata,
// mirrors into AppState, cancels any in-flight cell editor, and
// re-reads the current table so the grid updates in place.
// Toast on success; status_msg on failure.
void set_active_view_rom(AppState &state, std::string const &id);
void copy_rect_to_clipboard(st::Definition::TableData const &td, st::edit::Rect const &rect,
                            int precision);

void execute_action(AppState &state, ConfirmAction action, std::filesystem::path const &path);
void request_action(AppState &state, ConfirmAction action, std::filesystem::path path = {});

// Cross-reference jump from a non-table panel (gauge cluster, knock
// overlay, log viewer) to the Table editor on a specific table id.
// Switches workspace to Tune if not already, selects the table, focuses
// the Table window, and toasts a not-in-this-pack result if the id is
// missing. Returns true when the jump landed on a real table; false on
// "no project" / "table id unknown" — caller can use this to disable a
// menu item if desired (we render the menu unconditionally and let the
// toast carry the failure since the user's intent is still legible).
bool jump_to_table(AppState &state, std::string_view table_id);

// CSV-import application. Modal preview consumes the parsed result and
// commits it through this entry point.
std::optional<std::string> apply_parsed_csv_edits(AppState &state,
                                                  std::string const &target_table_id,
                                                  st::EditCsvParseResult const &parsed);

} // namespace st::ui

#endif
