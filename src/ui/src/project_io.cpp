// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// File-dialog wrappers around Project::open / save_working_rom +
// CSV import/export. Thin NFD adapters on top of the pure-data
// action seam in actions.hpp; the import-staging-then-preview shape
// matches the CLI's `project-edit-csv --dry-run` safety net.

#include "project_io.hpp"

#include "app_state.hpp"
#include "panels/panels.hpp" // enqueue_toast
#include "persistence.hpp"

#include "st/audit.hpp"
#include "st/edit.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace st::ui {

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
    // save_all persists working + every additional with non-empty
    // history (Issue #10 phase 3). User who edited only the working
    // slot sees byte-identical disk output as the v1 save_working_rom.
    if (auto s = state.project->save_all(); !s.has_value()) {
        // Failure: keep status_msg for persistent visibility AND fire
        // a danger toast for the immediate "this just happened" beat.
        // The user might be staring at the save button when this fires;
        // the toast confirms they triggered something.
        auto const err = "Save failed: " + s.error().to_string();
        state.persisted_state_verified = false;
        state.status_msg = err;
        enqueue_toast(state, ToastKind::Danger, err);
        if (state.audit_log.has_value()) {
            (void)state.audit_log->log(st::audit::EntryKind::Custom, "ui",
                                        "Project save failed",
                                        {{"error", s.error().to_string()}});
        }
        return;
    }
    // Success: status bar already shows "Saved Nm ago" + last_save_iso
    // tooltip, so the persistent status_msg is redundant. Toast does the
    // transient confirmation and disappears on its own — no stale
    // "Saved." sticking around for minutes after.
    state.status_msg.clear();
    enqueue_toast(state, ToastKind::Success, "Saved and verified.");
    state.dirty = false;
    state.last_save_iso = iso8601_utc_now();
    state.persisted_state_verified = true;
    if (state.audit_log.has_value()) {
        (void)state.audit_log->log(
            st::audit::EntryKind::ProjectSaved, "ui", "Project saved",
            {{"display_name", state.project->display_name()},
             {"dir", state.project->dir().string()}});
    }
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
    // Export the ROM the user is currently looking at (Issue #10
    // sweep). When the active slot is an additional ROM, exporting
    // working_rom() would surface bytes that don't match the on-screen
    // grid — confusing the "I see this, share this" mental model.
    // view_rom() resolves through active_rom_id; nullptr fallback only
    // triggers when there's no project (we returned above).
    auto const *read_rom = state.view_rom();
    if (read_rom == nullptr) {
        return std::string{"No project loaded."};
    }
    auto const working_td =
        state.project->definition().read_table_values(*read_rom, *table);
    if (!working_td.has_value()) {
        return "read active ROM: " + working_td.error().to_string();
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
    // CSV import targets the editable slot via apply_op — use the
    // active editable ROM for sizing too. Source is read-only so the
    // import path stays gated by active_rom_mut() at the apply step.
    st::Rom const *editable_rom = state.project->active_rom_mut();
    if (editable_rom == nullptr) {
        editable_rom = &state.project->working_rom();
    }
    auto td = state.project->definition().read_table_values(*editable_rom, *table);
    if (!td.has_value()) {
        state.status_msg = "Import failed: read active: " + td.error().to_string();
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

} // namespace st::ui
