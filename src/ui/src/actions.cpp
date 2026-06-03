// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// User-action surface: undo/redo, copy/paste, the action router behind
// the unsaved-changes modal, the CSV-import bulk-edit applier. The
// `apply_op` template body is in actions.hpp (template-call sites span
// modals + panels, so the body has to be visible at each one).

#include "actions.hpp"

#include "app_state.hpp"
#include "panels/panels.hpp" // apply_workspace_mode, enqueue_toast
#include "project_io.hpp"    // open_project_dialog

#include "st/audit.hpp"
#include "st/edit.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {

// Forward decl — parse_tsv lives further down near the other
// clipboard helpers. paste_clipboard_at_cursor uses it.
std::vector<std::vector<double>> parse_tsv(std::string_view text);

// Applies a previously-parsed CSV cell list to the currently-selected
// table as a single bulk edit recorded through edit::History. Used by
// both the import-preview modal's Apply button and any non-interactive
// caller. Returns nullopt on success or an error message.
std::optional<std::string> apply_parsed_csv_edits(AppState &state,
                                                  std::string const &target_table_id,
                                                  st::EditCsvParseResult const &parsed) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    if (parsed.cells.empty()) {
        return std::string{"Nothing to apply."};
    }
    // CSV import targets the active slot (Issue #10 phase 3).
    st::Rom *target_rom = state.project->active_rom_mut();
    if (target_rom == nullptr) {
        return std::string{"This ROM is read-only — switch View → Active ROM "
                           "to an editable slot."};
    }
    auto td = state.project->definition().read_table_values(*target_rom, *table);
    if (!td.has_value()) {
        return "read active rom: " + td.error().to_string();
    }
    // Bounding rect over all touched cells — same shape the CLI records.
    std::size_t r_min = parsed.cells[0].row, r_max = parsed.cells[0].row;
    std::size_t c_min = parsed.cells[0].col, c_max = parsed.cells[0].col;
    for (auto const &e : parsed.cells) {
        r_min = std::min(r_min, e.row);
        r_max = std::max(r_max, e.row);
        c_min = std::min(c_min, e.col);
        c_max = std::max(c_max, e.col);
    }
    st::edit::Rect const rect{r_min, r_max, c_min, c_max};
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        return "snapshot before: " + before.error().to_string();
    }
    for (auto const &e : parsed.cells)
        td->values[e.row][e.col] = e.value;
    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        return "snapshot after: " + after.error().to_string();
    }
    if (auto wb = state.project->definition().write_table_values(*target_rom, *table, *td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[64];
    std::snprintf(descbuf, sizeof descbuf, "csv import (%zu cell%s)", parsed.cells.size(),
                  parsed.cells.size() == 1 ? "" : "s");
    state.project->active_history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                                 std::move(*after),
                                                                 std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(*td);
    }
    state.dirty = true;
    // Audit the bulk import (analyst Issue #8). CSV apply can mutate
    // hundreds of cells at once — a high-signal event the timeline
    // should capture alongside save / autotune / read.
    if (state.audit_log.has_value()) {
        std::vector<std::pair<std::string, std::string>> fields{
            {"table", table->id},
            {"cells", std::to_string(parsed.cells.size())}};
        // Issue #10 sweep — CSV import targets active_rom_mut() through
        // apply_op; annotate so the audit timeline doesn't blur imports
        // across different ROMs.
        if (!state.active_rom_id.empty() && state.active_rom_id != "working") {
            fields.emplace_back("rom", state.active_rom_id);
        }
        (void)state.audit_log->log(
            st::audit::EntryKind::EditCommitted, "ui.csv_import",
            std::string{descbuf}, std::move(fields));
    }

    std::string status = "Imported " + std::to_string(parsed.cells.size()) + " cell" +
                         (parsed.cells.size() == 1 ? "" : "s") + " into " + table->id + ".";
    if (!parsed.warnings.empty()) {
        status += "  Warning: " + parsed.warnings.front().message;
        if (parsed.warnings.size() > 1) {
            status += "  (+" + std::to_string(parsed.warnings.size() - 1) + " more)";
        }
    }
    state.status_msg = std::move(status);
    return std::nullopt;
}

// NFD-driven open dialog wrapper for CSV import. Parses the file
// through st::parse_edit_csv and, on success, stages the parsed cells
// in AppState and opens the preview modal — the apply happens only

// Undo/redo share the same restore-and-writeback shape; only the snapshot
// side and the rollback direction differ. `forward = false` for undo,
// `forward = true` for redo. Dispatches on payload kind — TableEdit goes
// through the definition's read_table_values / write_table_values seam,
// ByteEdit writes raw bytes directly (no table layer needed).
void apply_history_step(AppState &state, st::edit::Edit const &edit, bool forward) {
    auto const rollback_cursor = [&] {
        if (forward) {
            (void)state.project->active_history().undo();
        } else {
            (void)state.project->active_history().redo();
        }
    };

    // Undo/redo target the active slot (Issue #10 phase 3). Caller
    // (do_undo / do_redo) has already gated on active_rom_mut() so
    // by the time we get here, this should be non-null. Belt and
    // braces: bail out gracefully if not.
    st::Rom *target_rom = state.project->active_rom_mut();
    if (target_rom == nullptr) {
        state.status_msg = "history: active ROM is read-only";
        rollback_cursor();
        return;
    }

    if (auto const *te = edit.as_table(); te != nullptr) {
        auto const *tbl = state.project->definition().find_table(te->table_id);
        if (tbl == nullptr) {
            state.status_msg = "history: table not in pack: " + te->table_id;
            rollback_cursor();
            return;
        }

        auto td = state.project->definition().read_table_values(*target_rom, *tbl);
        if (!td.has_value()) {
            state.status_msg = "history re-read: " + td.error().to_string();
            rollback_cursor();
            return;
        }

        auto const &snap = forward ? te->after : te->before;
        if (auto s = st::edit::restore(*td, snap); !s.has_value()) {
            state.status_msg = "history restore: " + s.error().to_string();
            rollback_cursor();
            return;
        }

        auto wb = state.project->definition().write_table_values(*target_rom, *tbl, *td);
        if (!wb.has_value()) {
            state.status_msg = "history writeback: " + wb.error().to_string();
            rollback_cursor();
            return;
        }

        if (te->table_id == state.selected_table_id) {
            state.current_table_data = std::move(*td);
        }
    } else if (auto const *be = edit.as_byte(); be != nullptr) {
        for (auto const &c : be->changes) {
            auto const v = forward ? c.after : c.before;
            if (auto s = target_rom->write_u8(c.address, v); !s.has_value()) {
                char buf[80];
                std::snprintf(buf, sizeof buf, "history byte writeback @0x%zX: ", c.address);
                state.status_msg = std::string{buf} + s.error().to_string();
                rollback_cursor();
                return;
            }
        }
    }

    state.status_msg.clear();
    // Undo / redo modifies the active ROM in memory; flag dirty so the
    // unsaved-changes guard catches an undo-then-quit-without-save.
    state.dirty = true;
}
// Reads the system clipboard, parses it as TSV, and pastes the values
// starting at the cursor cell. The selection rect is set to the paste
// destination first (clipped to table bounds) so apply_op's single
// history record covers exactly the cells that changed.
void paste_clipboard_at_cursor(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    if (state.project->active_rom_mut() == nullptr) {
        state.status_msg = "Paste: this ROM is read-only "
                           "(switch View → Active ROM to an editable slot).";
        return;
    }
    char const *clip = ImGui::GetClipboardText();
    if (clip == nullptr || *clip == '\0')
        return;
    auto grid = parse_tsv(std::string_view{clip});
    if (grid.empty() || grid.front().empty())
        return;

    auto &td = *state.current_table_data;
    std::size_t const cur_r = state.selection.r_cursor;
    std::size_t const cur_c = state.selection.c_cursor;
    if (cur_r >= td.values.size() || td.values[cur_r].empty())
        return;

    // Clip the paste rect to actual table bounds (Excel-style truncate,
    // not wrap).
    std::size_t const grid_rows = grid.size();
    std::size_t grid_cols = 0;
    for (auto const &row : grid) {
        if (row.size() > grid_cols)
            grid_cols = row.size();
    }
    std::size_t const r1 = std::min<std::size_t>(cur_r + grid_rows - 1, td.values.size() - 1);
    std::size_t const c1 =
        std::min<std::size_t>(cur_c + grid_cols - 1, td.values[cur_r].size() - 1);

    // Snap selection to the paste destination so apply_op picks the
    // right rect.
    state.selection.r_anchor = cur_r;
    state.selection.r_cursor = r1;
    state.selection.c_anchor = cur_c;
    state.selection.c_cursor = c1;

    apply_op(state, "paste", [&grid, cur_r, cur_c](auto &t, auto rect) -> st::Status {
        for (std::size_t dr = 0;
             dr < grid.size() && cur_r + dr < t.values.size() && cur_r + dr <= rect.r_end; ++dr) {
            auto &tt_row = t.values[cur_r + dr];
            auto const &g_row = grid[dr];
            for (std::size_t dc = 0;
                 dc < g_row.size() && cur_c + dc < tt_row.size() && cur_c + dc <= rect.c_end;
                 ++dc) {
                tt_row[cur_c + dc] = g_row[dc];
            }
        }
        return st::ok();
    });
}

// Read the source ROM's values for the selection and copy them onto
// the working ROM via apply_op. One history entry per call so a single
// Ctrl+Z restores the user's edits in that region. Useful workflow:
// "I edited a corner of this map and don't like it — revert just
// those cells, not the whole table."
void reset_selection_to_source(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    if (state.project->active_rom_mut() == nullptr) {
        state.status_msg = "Reset to source: this ROM is read-only "
                           "(switch View → Active ROM to an editable slot).";
        return;
    }
    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl == nullptr) {
        state.status_msg = "Reset: table missing from pack";
        return;
    }
    // Read the source ROM through the same scaling pipeline as the
    // working ROM — so what we copy back is the exact value a fresh
    // open of the source would show, not raw bytes.
    auto src_td = state.project->definition().read_table_values(state.project->source_rom(), *tbl);
    if (!src_td.has_value()) {
        state.status_msg = "Reset: read source: " + src_td.error().to_string();
        return;
    }
    auto const rect = state.selection.as_rect();
    apply_op(state, "reset to source", [&src = *src_td, rect](auto &t, auto r) -> st::Status {
        for (std::size_t row = r.r_start;
             row <= r.r_end && row < t.values.size() && row < src.values.size(); ++row) {
            for (std::size_t col = r.c_start;
                 col <= r.c_end && col < t.values[row].size() && col < src.values[row].size();
                 ++col) {
                t.values[row][col] = src.values[row][col];
            }
        }
        (void)rect; // rect == r when apply_op fires
        return st::ok();
    });
}

void set_active_view_rom(AppState &state, std::string const &id) {
    if (!state.project.has_value()) {
        return;
    }
    if (auto s = state.project->set_active_rom_id(id); !s.has_value()) {
        state.status_msg = "Active ROM: " + s.error().to_string();
        return;
    }
    state.active_rom_id = state.project->active_rom_id();
    if (auto s = state.project->save_metadata(); !s.has_value()) {
        // Memory state already updated; surface the disk-write failure but
        // don't roll back — the in-memory switch is still useful for this
        // session even if the project file couldn't be rewritten.
        state.status_msg = "Active ROM saved in memory but save_metadata failed: " +
                           s.error().to_string();
    }
    // Cancel any in-flight inline cell editor — the ROM under it
    // just changed, so the typed buffer references stale values.
    state.editing_cell = false;
    state.editor_just_opened = false;
    if (!state.selected_table_id.empty()) {
        state.select_table(state.selected_table_id);
    }
    auto const &active = state.active_rom_id;
    enqueue_toast(state, ToastKind::Info,
                  std::string{"Active ROM: "} +
                      (active.empty() ? "working" : active));
}

void do_undo(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    // Undo/redo route through the active slot's history + ROM
    // (Issue #10 phase 3). Source is the only read-only active slot;
    // additional ROMs have their own per-ROM history and undo their
    // own edits independently of working.
    if (state.project->active_rom_mut() == nullptr) {
        state.status_msg = "Undo: this ROM is read-only.";
        return;
    }
    auto const *e = state.project->active_history().undo();
    if (e != nullptr) {
        apply_history_step(state, *e, /*forward=*/false);
    }
}

void do_redo(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    if (state.project->active_rom_mut() == nullptr) {
        state.status_msg = "Redo: this ROM is read-only.";
        return;
    }
    auto const *e = state.project->active_history().redo();
    if (e != nullptr) {
        apply_history_step(state, *e, /*forward=*/true);
    }
}

// ---------------------------------------------------------------------
// Unsaved-changes guard.
//
// `request_action` is the single entry point for any user gesture that
// would lose in-memory edits if executed directly: Open Project (which
// replaces the current project), Open a recent (same), Close Project,
// and Quit (Ctrl+Q / menu / window-X click). When the project has
// pending edits, the modal opens and the action is queued; otherwise
// the action runs immediately. `execute_action` performs the action.
// ---------------------------------------------------------------------

void execute_action(AppState &state, ConfirmAction action, std::filesystem::path const &path) {
    switch (action) {
    case ConfirmAction::None:
        break;
    case ConfirmAction::OpenDialog:
        open_project_dialog(state);
        break;
    case ConfirmAction::OpenRecent:
        state.try_open_project(path);
        break;
    case ConfirmAction::NewProject:
        state.show_new_project_modal = true;
        break;
    case ConfirmAction::Close:
        state.close_project();
        break;
    case ConfirmAction::Quit:
        state.user_confirmed_quit = true;
        break;
    }
}

void request_action(AppState &state, ConfirmAction action, std::filesystem::path path) {
    if (action == ConfirmAction::None)
        return;
    bool const need_confirm = state.dirty && state.project.has_value();
    if (need_confirm) {
        state.next_action = action;
        state.next_recent = std::move(path);
        state.show_unsaved_modal = true;
    } else {
        execute_action(state, action, path);
    }
}
// Format the rect of `td` as TSV (rows on lines, cells tab-separated)
// and put it on the system clipboard via ImGui's clipboard helper.
// Format matches Excel/Sheets clipboard convention so pasted values
// round-trip cleanly to a spreadsheet for batch analysis.
void copy_rect_to_clipboard(st::Definition::TableData const &td, st::edit::Rect const &rect,
                            int precision) {
    std::string out;
    char buf[32];
    for (std::size_t r = rect.r_start; r <= rect.r_end && r < td.values.size(); ++r) {
        bool first = true;
        for (std::size_t c = rect.c_start; c <= rect.c_end && c < td.values[r].size(); ++c) {
            if (!first)
                out.push_back('\t');
            first = false;
            std::snprintf(buf, sizeof buf, "%.*f", precision, td.values[r][c]);
            out += buf;
        }
        out.push_back('\n');
    }
    ImGui::SetClipboardText(out.c_str());
}

// Menubar wrapper around copy_rect_to_clipboard. Pulls scaling precision
// off the current selected table so the clipboard formatting matches the
// grid. Edit menu and Ctrl+C both route through here.
void do_copy_selection(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    int precision = 0;
    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl != nullptr) {
        auto const *scal = state.project->definition().find_scaling(tbl->scaling);
        if (scal != nullptr) {
            precision = scal->precision;
        }
    }
    copy_rect_to_clipboard(*state.current_table_data, state.selection.as_rect(), precision);
}

// Parse a TSV-ish payload (Excel/Sheets clipboard format) into a 2D
// grid of doubles. Tolerant of trailing newlines, mixed CRLF/LF, and
// non-numeric cells (those become 0.0). Empty input returns an empty
// grid.
std::vector<std::vector<double>> parse_tsv(std::string_view text) {
    std::vector<std::vector<double>> grid;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t line_end = i;
        while (line_end < text.size() && text[line_end] != '\n' && text[line_end] != '\r') {
            ++line_end;
        }
        std::string_view line{text.data() + i, line_end - i};
        std::vector<double> row;
        std::size_t cs = 0;
        while (cs <= line.size()) {
            std::size_t ce = cs;
            while (ce < line.size() && line[ce] != '\t')
                ++ce;
            std::string_view cell{line.data() + cs, ce - cs};
            // Trim whitespace at both ends.
            while (!cell.empty() && std::isspace(static_cast<unsigned char>(cell.front()))) {
                cell.remove_prefix(1);
            }
            while (!cell.empty() && std::isspace(static_cast<unsigned char>(cell.back()))) {
                cell.remove_suffix(1);
            }
            double v = 0.0;
            if (!cell.empty()) {
                std::string tmp{cell};
                (void)std::sscanf(tmp.c_str(), "%lf", &v);
                // sscanf failure leaves v at 0; tolerate so a stray
                // empty cell doesn't fail the whole paste.
            }
            row.push_back(v);
            if (ce >= line.size())
                break;
            cs = ce + 1;
        }
        if (!row.empty())
            grid.push_back(std::move(row));
        // Advance past the newline (handle both CRLF and LF).
        if (line_end < text.size()) {
            if (text[line_end] == '\r' && line_end + 1 < text.size() &&
                text[line_end + 1] == '\n') {
                line_end += 2;
            } else {
                line_end += 1;
            }
        }
        i = line_end;
    }
    return grid;
}

// Cross-reference jump from a gauge / overlay / log row → Table editor.
// Pre-jump validation: a project must be open and the named table must
// exist in its definition. Failures land as toasts so the user sees
// *why* the click did nothing — silent no-ops are the second-worst UX
// outcome after a crash. On success: workspace switches to Tune (so the
// Table panel is visible), the selection is moved, and the Table window
// is focus-requested so it surfaces in front of whatever the user had
// docked over it.
bool jump_to_table(AppState &state, std::string_view table_id) {
    if (table_id.empty()) {
        return false;
    }
    std::string const id{table_id};
    if (!state.project.has_value()) {
        enqueue_toast(state, ToastKind::Warn,
                      "No project loaded — open a tune first, then try again.");
        return false;
    }
    if (state.project->definition().find_table(id) == nullptr) {
        enqueue_toast(state, ToastKind::Warn,
                      "Table '" + id + "' isn't in this pack — channel and pack don't match.");
        return false;
    }
    if (state.workspace_mode != WorkspaceMode::Tune) {
        apply_workspace_mode(state, WorkspaceMode::Tune);
    }
    state.select_table(id);
    // Focus the Table window so the editor surfaces. ImGui matches the
    // raw title string passed to Begin(...) here — render_table_view
    // uses "Table" with no id-suffix, so the literal does too.
    ImGui::SetWindowFocus("Table");
    enqueue_toast(state, ToastKind::Info, "Jumped to '" + id + "'.");
    return true;
}

} // namespace st::ui
