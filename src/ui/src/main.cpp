// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// subuwutuner-gui — Dear ImGui front-end.
//
// Opens a .stune project passed as argv[1], renders a dockable sidebar of
// tables, and shows the selected table as a read-only grid. Editing,
// project-management menus, and file-open dialogs land in follow-ups; this
// pass lays the polish foundation: docking + viewports, tuned palette,
// system-font probing.

#include "st/autotune.hpp"
#include "st/core/version.hpp"
#include "st/edit.hpp"
#include "st/feature.hpp"
#include "st/flash.hpp"
#include "st/log/adaptive_history.hpp"
#include "st/log/coldstart.hpp"
#include "st/log/ebcs.hpp"
#include "st/log/knock_dashboard.hpp"
#include "st/policy.hpp"
#include "st/config.hpp"
#include "st/defs/pack_registry.hpp"
#include "st/project.hpp"
#include "st/transport/factory.hpp"
#include "st/transport/mock.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/uds_trace.hpp"

#include "icon_data.hpp"

// Shared UI headers — checkpoint 1 of the main.cpp split. Struct/enum
// definitions live here; function implementations are still in this
// file until the per-modal / per-panel moves land.
#include "app_state.hpp"
#include "persistence.hpp"
#include "theme.hpp"
#include "actions.hpp"
#include "project_io.hpp"
#include "widgets/widgets.hpp"
#include "widgets/adapter_picker.hpp"
#include "modals/modals.hpp"
#include "panels/panels.hpp"

// ImGui + backends.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // DockBuilder*
#include <implot.h>
#include <ios>
#include <limits>
#include <memory>
#include <nfd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Everything below is wrapped in `namespace st::ui { ... }` for
// checkpoint 1 of the split. Header decls (in app_state.hpp etc.) are
// at st::ui scope; main.cpp's function bodies sit directly in st::ui
// so they satisfy those decls with external linkage. The only inner
// anonymous namespace kept is the one around g_current_theme (a true
// file-local cache variable). Per-file anon namespaces come back when
// each modal / panel moves to its own .cpp.

namespace st::ui {

// Widget primitives moved to widgets/widgets.cpp (decls in widgets/widgets.hpp).
// Toast helpers moved to panels/toasts.cpp (decls in panels/panels.hpp).

// ---------------------------------------------------------------------
// Recents — one-line-per-entry config persisted between cold starts.
// Lives next to other user-config in the OS-conventional location:
//   Windows: %LOCALAPPDATA%\SubuwuTuner\recents.txt
//   Mac:     ~/Library/Application Support/SubuwuTuner/recents.txt
//   Linux:   $XDG_CONFIG_HOME/subuwutuner/recents.txt
//            (fallback: $HOME/.config/subuwutuner/recents.txt)
//
// Format: one entry per line, "<ISO-8601 UTC>\t<absolute path>".
// Cap at 8 entries; most recent first. A malformed line is silently
// skipped — recents are a convenience, not a source of truth.
// ---------------------------------------------------------------------


// Selection, TableViewMode, WorkspaceMode, ConfirmAction, ToastKind,
// Toast, AdapterPickerState moved to app_state.hpp.

// Adapter picker helpers (render_adapter_picker / adapter_picker_to_spec /
// adapter_is_trace_mode) stay file-local for now — their bodies use ImGui +
// transport-factory details. Header is widgets/adapter_picker.hpp.

// AppState moved to app_state.hpp.
//
// The three methods that have non-trivial bodies (try_open_project,
// select_table, close_project) are declared in the header and defined
// out-of-class here so the bodies can call file-local helpers like
// enqueue_toast / push_recent / save_recents that still live in main.cpp.

void AppState::try_open_project(std::filesystem::path const &path) {
    auto r = st::Project::open(path);
    if (!r.has_value()) {
        auto const err = "Failed to open " + path.string() + ": " + r.error().to_string();
        status_msg = err;
        enqueue_toast(*this, ToastKind::Danger, err);
        project.reset();
        selected_table_id.clear();
        current_table_data.reset();
        selection.reset();
        last_save_iso.reset();
        return;
    }
    project = std::move(*r);
    status_msg.clear();
    selected_table_id.clear();
    current_table_data.reset();
    selection.reset();
    dirty = false;
    last_save_iso.reset();
    enqueue_toast(*this, ToastKind::Success,
                  std::string{"Loaded "} + path.filename().string());
    push_recent(recents, path);
    save_recents(recents);
    auto const &tables = project->definition().tables();
    if (!tables.empty()) {
        select_table(tables.front().id);
    }
}

void AppState::select_table(std::string const &id) {
    selected_table_id = id;
    current_table_data.reset();
    selection.reset();
    selected_z = 0;
    if (!project.has_value()) {
        return;
    }
    auto const *table = project->definition().find_table(id);
    if (table == nullptr) {
        return;
    }
    auto td = project->definition().read_table_values(project->working_rom(), *table);
    if (td.has_value()) {
        current_table_data = std::move(*td);
    }
}

void AppState::close_project() {
    project.reset();
    selected_table_id.clear();
    current_table_data.reset();
    selection.reset();
    selected_z = 0;
    status_msg.clear();
    dirty = false;
    last_save_iso.reset();
}

// Native folder picker for a .stune project directory. nfd handles the OS
// dialog; we only have to feed the result back through Project::open.
void open_project_dialog(AppState &state) {
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::PickFolder(out_path);
    if (r == NFD_OKAY) {
        state.try_open_project(std::filesystem::path(out_path.get()));
    } else if (r == NFD_ERROR) {
        state.status_msg = std::string{"Open dialog error: "} + NFD::GetError();
    }
}

void save_project(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    if (auto s = state.project->save_working_rom(); !s.has_value()) {
        // Failure: keep status_msg for persistent visibility AND fire
        // a danger toast for the immediate "this just happened" beat.
        // The user might be staring at the save button when this fires;
        // the toast confirms they triggered something.
        auto const err = "Save failed: " + s.error().to_string();
        state.status_msg = err;
        enqueue_toast(state, ToastKind::Danger, err);
        return;
    }
    // Success: status bar already shows "Saved Nm ago" + last_save_iso
    // tooltip, so the persistent status_msg is redundant. Toast does the
    // transient confirmation and disappears on its own — no stale
    // "Saved." sticking around for minutes after.
    state.status_msg.clear();
    enqueue_toast(state, ToastKind::Success, "Saved.");
    state.dirty = false;
    state.last_save_iso = iso8601_utc_now();
}

// Writes the currently-selected table to `path` as a CSV in the same
// format `subuwutuner-cli project-export-csv` emits (identity headers
// + `row,col,value` rows). When `diff_only` is true, emits only cells
// whose working value differs from the source — the share-a-tune-diff
// shape. Returns nullopt on success or an error message for the
// status bar.
std::optional<std::string>
write_current_table_csv(AppState const &state, std::filesystem::path const &path, bool diff_only) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    if (state.selected_table_id.empty()) {
        return std::string{"No table selected."};
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        return "Table '" + state.selected_table_id + "' not found in pack.";
    }
    auto const working_td =
        state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!working_td.has_value()) {
        return "read working: " + working_td.error().to_string();
    }
    std::optional<st::Definition::TableData> source_td;
    if (diff_only) {
        auto s = state.project->definition().read_table_values(state.project->source_rom(), *table);
        if (!s.has_value()) {
            return "read source: " + s.error().to_string();
        }
        source_td = std::move(*s);
    }
    auto const *scaling = state.project->definition().find_scaling(table->scaling);
    int const prec = scaling != nullptr ? scaling->precision : 6;

    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        return "cannot open " + path.string();
    }
    out << "# pack_id = \"" << state.project->definition().pack().id << "\"\n";
    out << "# table   = \"" << table->id << "\"\n";
    out << "row,col,value\n";
    char buf[64];
    for (std::size_t r = 0; r < working_td->values.size(); ++r) {
        for (std::size_t c = 0; c < working_td->values[r].size(); ++c) {
            double const v = working_td->values[r][c];
            if (diff_only) {
                if (r >= source_td->values.size() || c >= source_td->values[r].size())
                    continue;
                if (v == source_td->values[r][c])
                    continue;
            }
            std::snprintf(buf, sizeof buf, "%zu,%zu,%.*f\n", r, c, prec, v);
            out << buf;
        }
    }
    if (!out) {
        return "write failed: " + path.string();
    }
    // Success — caller composes the status message.
    return std::nullopt;
}

// Convenience: NFD-driven save dialog wrapper. Default filename is
// "<table_id>[.diff].csv" so the file falls under the table being
// exported on disk without the user having to think about it.
void export_current_table_csv_dialog(AppState &state, bool diff_only) {
    if (!state.project.has_value() || state.selected_table_id.empty()) {
        state.status_msg = "Select a table first.";
        return;
    }
    std::string default_name = state.selected_table_id;
    if (diff_only)
        default_name += ".diff";
    default_name += ".csv";

    nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::SaveDialog(out_path, filters, 1, nullptr, default_name.c_str());
    if (r == NFD_CANCEL) {
        return;
    }
    if (r == NFD_ERROR) {
        state.status_msg = std::string{"Save dialog error: "} + NFD::GetError();
        return;
    }
    std::filesystem::path const path{out_path.get()};
    if (auto err = write_current_table_csv(state, path, diff_only); err.has_value()) {
        state.status_msg = "Export failed: " + *err;
        return;
    }
    state.status_msg = "Exported " + state.selected_table_id + (diff_only ? " (diff) " : " ") +
                       "to " + path.filename().string() + ".";
}

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
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!td.has_value()) {
        return "read working: " + td.error().to_string();
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
    if (auto wb = state.project->definition().write_table_values(state.project->working_rom(),
                                                                 *table, *td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[64];
    std::snprintf(descbuf, sizeof descbuf, "csv import (%zu cell%s)", parsed.cells.size(),
                  parsed.cells.size() == 1 ? "" : "s");
    state.project->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                          std::move(*after), std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(*td);
    }
    state.dirty = true;

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
// after the user explicitly confirms (matching the CLI's --dry-run
// safety net for shared / untrusted CSVs).
void import_csv_into_current_table_dialog(AppState &state) {
    if (!state.project.has_value() || state.selected_table_id.empty()) {
        state.status_msg = "Select a table first.";
        return;
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        state.status_msg = "Table not in pack.";
        return;
    }
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!td.has_value()) {
        state.status_msg = "Import failed: read working: " + td.error().to_string();
        return;
    }
    std::size_t const rows = td->values.size();
    std::size_t const cols = rows > 0 ? td->values[0].size() : 0;
    if (rows == 0 || cols == 0) {
        state.status_msg = "Import failed: cannot import into a "
                           "zero-dimension table.";
        return;
    }

    nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
    if (r == NFD_CANCEL) {
        return;
    }
    if (r == NFD_ERROR) {
        state.status_msg = std::string{"Open dialog error: "} + NFD::GetError();
        return;
    }
    std::filesystem::path const path{out_path.get()};

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        state.status_msg = "Import failed: cannot open " + path.string();
        return;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string const text = std::move(buf).str();

    st::EditCsvParseOptions opts;
    opts.expected_pack_id = state.project->definition().pack().id;
    opts.expected_table_id = table->id;
    opts.table_rows = rows;
    opts.table_cols = cols;
    auto parsed = st::parse_edit_csv(text, opts);
    if (!parsed.has_value()) {
        state.status_msg = "Import failed: " + parsed.error().to_string();
        return;
    }
    if (parsed->cells.empty()) {
        state.status_msg = "Import: no edit rows parsed; nothing to do.";
        return;
    }

    // Stash the parse + the current values for the modal preview.
    state.csv_import_parsed = std::move(*parsed);
    state.csv_import_table_id = table->id;
    state.csv_import_source_path = path;
    state.csv_import_before_values = std::move(*td);
    state.show_csv_import_modal = true;
}


// Undo/redo share the same restore-and-writeback shape; only the snapshot
// side and the rollback direction differ. `forward = false` for undo,
// `forward = true` for redo. Dispatches on payload kind — TableEdit goes
// through the definition's read_table_values / write_table_values seam,
// ByteEdit writes raw bytes directly (no table layer needed).
void apply_history_step(AppState &state, st::edit::Edit const &edit, bool forward) {
    auto const rollback_cursor = [&] {
        if (forward) {
            (void)state.project->history().undo();
        } else {
            (void)state.project->history().redo();
        }
    };

    if (auto const *te = edit.as_table(); te != nullptr) {
        auto const *tbl = state.project->definition().find_table(te->table_id);
        if (tbl == nullptr) {
            state.status_msg = "history: table not in pack: " + te->table_id;
            rollback_cursor();
            return;
        }

        auto td = state.project->definition().read_table_values(state.project->working_rom(), *tbl);
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

        auto wb =
            state.project->definition().write_table_values(state.project->working_rom(), *tbl, *td);
        if (!wb.has_value()) {
            state.status_msg = "history writeback: " + wb.error().to_string();
            rollback_cursor();
            return;
        }

        if (te->table_id == state.selected_table_id) {
            state.current_table_data = std::move(*td);
        }
    } else if (auto const *be = edit.as_byte(); be != nullptr) {
        auto &rom = state.project->working_rom();
        for (auto const &c : be->changes) {
            auto const v = forward ? c.after : c.before;
            if (auto s = rom.write_u8(c.address, v); !s.has_value()) {
                char buf[80];
                std::snprintf(buf, sizeof buf, "history byte writeback @0x%zX: ", c.address);
                state.status_msg = std::string{buf} + s.error().to_string();
                rollback_cursor();
                return;
            }
        }
    }

    state.status_msg.clear();
    // Undo / redo modifies the working ROM in memory; flag dirty so the
    // unsaved-changes guard catches an undo-then-quit-without-save.
    state.dirty = true;
}

// Forward decl — parse_tsv lives further down in the anon namespace
// near the other clipboard helpers. paste_clipboard_at_cursor uses it.
std::vector<std::vector<double>> parse_tsv(std::string_view text);

// Reads the system clipboard, parses it as TSV, and pastes the values
// starting at the cursor cell. The selection rect is set to the paste
// destination first (clipped to table bounds) so apply_op's single
// history record covers exactly the cells that changed.
void paste_clipboard_at_cursor(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
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

void do_undo(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto const *e = state.project->history().undo();
    if (e != nullptr) {
        apply_history_step(state, *e, /*forward=*/false);
    }
}

void do_redo(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto const *e = state.project->history().redo();
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

// render_unsaved_modal moved to modals/unsaved.cpp (along with
// modal_save_label, modal_discard_label, modal_subtitle as file-local).
// render_csv_import_modal moved to modals/csv_import.cpp.
// render_maf_autotune_modal moved to modals/autotune_maf.cpp (along
// with run_maf_autotune_preview + apply_maf_autotune_proposal helpers).
// render_kp_autotune_modal moved to modals/autotune_knock.cpp (along
// with run_knock_pull_preview + apply_knock_pull_proposal helpers).
// render_flash_modal moved to modals/flash.cpp (along with PendingFlash
// + build_pending_flash as file-local).
// render_shortcuts_modal moved to modals/shortcuts.cpp (along with
// ShortcutRow, ShortcutGroup, shortcuts_reference as file-local).
// render_about_modal moved to modals/about.cpp.
// render_new_project_modal moved to modals/new_project.cpp.

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
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

// Case-insensitive substring search. Used by the sidebar table filter
// to match against both human-readable name and the snake_case id.
// Empty needle matches everything (so an empty filter shows the full
// list). ASCII-only — calibration table names use ASCII identifiers.


// ===========================================================================
// Read-ROM-from-car modal
// ===========================================================================
//
// Renders the Tools → Read ROM from Car flow. State machine drives the body:
// Idle (form) → Running (progress) → Done (save dialog) / Failed (error) /
// Cancelled (status note). All long-running work runs on `state.read_rom_worker`;
// the modal just polls atomics + status enum on every frame.
//
// ----- v1.1 Write-ROM plan (NOT IMPLEMENTED — design notes only) -----
//
// The flash side of this same UX is meaningfully more dangerous than the
// read side, and the existing render_flash_modal already covers most of
// it for file-based flashing. The "Write ROM to Car" GUI button would be
// a thin wrapper that:
//
//   1. Routes through the same render_flash_modal policy gate (engine-
//      safety hard-stop + emissions confirmation + tuner-reason field).
//   2. Adds a "Pick adapter" sub-form identical to this read modal's
//      (transport kind + device/DLL path) — could literally share the
//      ImGui code via a render_adapter_picker(state) helper.
//   3. Adds a brick-protection confirmation (docs/05 §4): battery > 12.0V
//      via a UDS PID poll, ignition state check, etc.
//   4. Background-threads Flasher::execute(plan), with the same atomic
//      progress + cancel pattern this read modal uses. Flasher::execute
//      already emits a structured FlashReport per step — surface that
//      as a per-sector ledger in the modal body.
//   5. Verify pass (the existing post-erase-write loop in execute()
//      already does this). Failure path: keep partial-flash report
//      visible + offer "Resume from journal" if applicable.
//
// Scope to defer:
//   - Resume-from-journal UI is its own modal/dialog.
//   - Vehicle-state precondition checks (battery, ignition, transmission
//      in N/P) need new transport calls + a "preflight checks passed"
//      panel. Hardware-gated — wire after OBDX arrives.
// render_settings_modal moved to modals/settings.cpp.
// render_def_registry_modal moved to modals/def_registry.cpp.
// render_read_rom_modal moved to modals/read_rom.cpp.
// open_command_palette + render_command_palette moved to panels/command_palette.cpp
// (PaletteCommand / PaletteCommandKind / build_palette_commands /
// dispatch_palette_command are file-local there).

// render_menubar moved to panels/menubar.cpp.
// render_sidebar moved to panels/sidebar.cpp.
// render_workspace_rail + render_dockspace_host + apply_workspace_mode
// + build_workspace_layout moved to panels/workspace.cpp (g_request_dock_reset
// is now file-local there; request_layout_reset is exposed via panels.hpp).


// render_welcome_panel moved to panels/welcome.cpp.
// render_stats_panel moved to panels/stats.cpp.
// render_knock_dashboard_panel moved to panels/knock_dashboard.cpp.
// render_adaptive_history_panel moved to panels/adaptive_history.cpp.
// render_coldstart_panel moved to panels/coldstart.cpp.
// render_ebcs_panel moved to panels/ebcs.cpp.
// render_dtcs_panel moved to panels/dtcs.cpp.
// render_history_panel moved to panels/history.cpp.
// render_features_designer moved to panels/features_designer.cpp.
// render_table_view moved to panels/table_view.cpp (along with
// GridStats, compute_stats, heatmap_color, text_right_aligned,
// render_table_heatmap, render_table_grid as file-local helpers).


// Status-bar TTL pass. Called once per frame before render_status_bar
// to fade out stale transient messages. Detects "new message" via a
// shadow string + per-message timestamp; after ~5s of the same
// content, clears it. Call sites just write to state.status_msg — no
// per-callsite TTL bookkeeping. Edge cases:
//  - Two consecutive identical messages (e.g. "Saved." twice) only
//    reset the timer on STRING change. That's the right trade-off:
//    rapid duplicate writes are usually the same event, and the
//    alternative (timer reset on every frame the string is set) would
//    pin the message forever.
//  - Time source is ImGui::GetTime() (wall-clock seconds since app
//    start, monotonic). No allocations, no calls outside ImGui.

// Render-frame callable shared between the main loop and GLFW's window-refresh
// callback. Windows enters a modal resize loop while the user drags a border;
// the main thread is blocked inside glfwPollEvents for the entire drag, so the
// only way to keep painting is to render from inside the WM_PAINT-driven
// refresh callback that GLFW dispatches during the modal loop.
//
// Lifted to outer st::ui scope so main() (at global scope) can address it
// after the anon namespace closes.
std::function<void()> g_render_frame;

} // namespace st::ui

int main(int argc, char *argv[]) {
    using namespace st::ui;
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == 0) {
        std::fprintf(stderr, "subuwutuner-gui: glfwInit failed\n");
        return 1;
    }

    // Stick to OpenGL 3.0 core-ish — the lowest target ImGui supports cleanly
    // across Win/Mac/Linux without needing extension-loader gymnastics.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow *window = glfwCreateWindow(1400, 880, "SubuwuTuner", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "subuwutuner-gui: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Set the window title-bar icon from the compiled-in RGBA blob
    // (icon_data.hpp, regenerated by scripts/embed_icon.py from
    // assets/icon.png). GLFW takes ownership of nothing — pixels stay
    // in our static .rodata segment. The Windows Explorer / taskbar
    // EXE icon is set separately via subuwutuner.rc + windres in
    // src/ui/CMakeLists.txt; both originate from the same PNG.
    {
        GLFWimage icon_image{};
        icon_image.width = st::ui::icon::kWidth;
        icon_image.height = st::ui::icon::kHeight;
        icon_image.pixels = const_cast<unsigned char *>(st::ui::icon::kRgba);
        glfwSetWindowIcon(window, 1, &icon_image);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto &io = ImGui::GetIO();
    // Deliberately NOT setting ImGuiConfigFlags_NavEnableKeyboard. With
    // it on, ImGui auto-focuses the first focusable widget in each
    // window and draws a permanent nav-focus rectangle around it —
    // which reads as "one cell is highlighted and never unhighlights"
    // in the data grid. Tab-cycling through hundreds of cells is not
    // a workflow this app targets, and every actual keyboard nav we
    // care about (arrow keys in the grid, Ctrl+F focus on the filter,
    // Enter/Esc in modals, Ctrl+{O,S,Z,Y,Q}) is handled explicitly via
    // IsKeyPressed and works without the nav system being on.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Force a tab bar on every docked window — even alone-in-node ones.
    // Without this, ImGui auto-hides the tab on single-window nodes and
    // there's no visible drag handle, so the panel can't be undocked or
    // moved by dragging once it's settled.
    io.ConfigDockingAlwaysTabBar = true;

    Fonts const fonts = load_fonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    state.recents = load_recents();
    state.settings = load_settings();
    // Best-effort lookup of the bundled fixtures/demo.stune. Welcome
    // panel renders a "Try the demo project" button when set; absent
    // in installs that didn't ship the demo (the call returns
    // nullopt and the button just doesn't render).
    state.demo_project_path = resolve_demo_project_path(argc >= 1 ? argv[0] : nullptr);
    // Apply the persisted theme before any user-visible frame renders.
    apply_theme(state.settings.theme);
    std::string_view const arg1 = (argc >= 2) ? argv[1] : "";
    if (arg1 == "-h" || arg1 == "--help" || arg1 == "/?") {
        std::fputs("Usage: subuwutuner-gui [PROJECT.stune]\n"
                   "  Open the optional .stune project on launch. Without an "
                   "argument, the GUI starts on the welcome panel.\n",
                   stderr);
        return 0;
    }
    if (!arg1.empty()) {
        state.try_open_project(argv[1]);
    } else {
        state.status_msg = "Open a .stune project: File → Open (Ctrl+O).";
    }

    auto render_frame = [&]() {
        // The user clicked the window's close button (or any other
        // path that toggled GLFW's close flag). Route it through the
        // same confirm-action machinery as Ctrl+Q / File → Quit so an
        // accidental click on the X doesn't lose unsaved edits. The
        // GLFW flag is reset so request_action's confirm path can
        // decide whether to actually quit.
        if (glfwWindowShouldClose(window) != 0) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            request_action(state, ConfirmAction::Quit);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Global keyboard shortcuts. IsKeyChordPressed is mod-aware, so
        // Ctrl+Shift+Z and Ctrl+Z don't collide.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) {
            request_action(state, ConfirmAction::Quit);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
            request_action(state, ConfirmAction::OpenDialog);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            save_project(state);
        }
        // Ctrl+F focuses the sidebar's table filter. Only meaningful
        // when a project is open; harmless otherwise (the next
        // render_sidebar call will reset the flag without acting on
        // it).
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
            state.focus_table_filter = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
            do_redo(state);
        } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            do_undo(state);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            do_redo(state);
        }
        // Ctrl+K opens the command palette. Highest-leverage discoverability
        // affordance — every menu item, every panel toggle, every recent
        // project, every table in the loaded pack is searchable from one
        // input. open_command_palette resets buffer + selection so each
        // open lands on a clean state.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K)) {
            open_command_palette(state);
        }

        tick_status_msg(state);
        render_menubar(state);
        // Workspace rail before the dockspace — its window position is
        // viewport-anchored and doesn't depend on dockspace layout, but
        // rendering it first keeps Z order intuitive when both happen
        // to overlap during the GLFW window-refresh callback path.
        render_workspace_rail(state);
        render_dockspace_host(state);
        render_sidebar(state);
        render_table_view(state, fonts);
        render_stats_panel(state);
        render_dtcs_panel(state);
        render_history_panel(state);
        render_knock_dashboard_panel(state);
        render_adaptive_history_panel(state);
        render_coldstart_panel(state);
        render_ebcs_panel(state);
        render_features_designer(state);
        render_status_bar(state);
        // Toasts last so they layer over panels + the status bar's
        // window (each toast is its own undecorated viewport-anchored
        // window). Render order doesn't matter for modals — those
        // dim the background regardless.
        render_toasts(state);
        render_unsaved_modal(state);
        render_csv_import_modal(state);
        render_new_project_modal(state);
        render_maf_autotune_modal(state);
        render_kp_autotune_modal(state);
        render_flash_modal(state);
        render_read_rom_modal(state);
        render_def_registry_modal(state);
        render_settings_modal(state);
        render_shortcuts_modal(state);
        render_about_modal(state);
        // Command palette rendered last so it stacks above every other
        // modal — Ctrl+K is meant as a global escape hatch.
        render_command_palette(state);

        if (state.show_imgui_demo) {
            ImGui::ShowDemoWindow(&state.show_imgui_demo);
        }

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport: render OS-level windows that ImGui has spawned for
        // panels the user tore off. The GL context juggling is the standard
        // pattern from the ImGui examples.
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            GLFWwindow *prev_ctx = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(prev_ctx);
        }

        // OS window title reflects current state: project name + dirty
        // marker so taskbar / alt-tab show what's open and whether
        // there are unsaved changes. Static cache avoids the glfw call
        // every frame.
        {
            static std::string last_title;
            std::string desired;
            if (state.project.has_value()) {
                if (state.dirty) {
                    desired = "\xE2\x97\x8F ";
                }
                desired += state.project->display_name();
                desired += " \xE2\x80\x94 SubuwuTuner";
            } else {
                desired = "SubuwuTuner";
            }
            if (desired != last_title) {
                glfwSetWindowTitle(window, desired.c_str());
                last_title = std::move(desired);
            }
        }

        glfwSwapBuffers(window);
    };

    g_render_frame = render_frame;
    glfwSetWindowRefreshCallback(window, [](GLFWwindow * /*w*/) {
        if (g_render_frame) {
            g_render_frame();
        }
    });

    while (!state.user_confirmed_quit) {
        glfwPollEvents();
        render_frame();
    }
    g_render_frame = nullptr;

    // If a Read-ROM-from-Car worker is still running (user closed the GUI
    // mid-read instead of clicking Cancel), signal it to abort and join
    // before the AppState destructor runs. Without this the std::thread
    // destructor would terminate() on a joinable thread.
    if (state.read_rom_worker.joinable()) {
        if (state.read_rom_cancel)
            state.read_rom_cancel->store(true, std::memory_order_release);
        state.read_rom_worker.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
