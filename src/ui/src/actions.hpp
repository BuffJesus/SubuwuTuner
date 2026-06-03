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
    // Defense-in-depth (Issue #10): edits target working_rom
    // unconditionally. UI surfaces should already gate the call sites
    // when active_rom_id != working, but if a command-palette entry,
    // menu item, or keyboard shortcut slips through, refuse here.
    if (!state.viewing_working_rom()) {
        state.status_msg = label +
                           ": cannot edit while a non-working ROM is active "
                           "(View → Active ROM → Working).";
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

    auto wb =
        state.project->definition().write_table_values(state.project->working_rom(), *tbl, td);
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
    state.project->history().record(st::edit::Edit::table(
        state.selected_table_id, std::move(*before), std::move(*after), std::move(label)));
    state.status_msg.clear();
    state.dirty = true;
    // Audit the edit (analyst Issue #8). Per-toolbar-op resolution —
    // can be noisy when the user clicks +5% twenty times in a row, but
    // the audit panel's kind-chip filter (commit 2b12599) lets users
    // collapse the project.edit_committed group when they don't want
    // the noise. Better noise than blind spot for a tuning workflow.
    if (state.audit_log.has_value()) {
        (void)state.audit_log->log(
            st::audit::EntryKind::EditCommitted, "ui.editor", audited_label,
            {{"table", state.selected_table_id},
             {"cells", std::to_string(audited_cells)}});
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
