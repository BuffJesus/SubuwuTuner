// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// File-dialog wrappers around Project::open / save_working_rom +
// CSV import/export. These are thin NFD adapters on top of the
// pure-data action seam in actions.hpp.

#ifndef ST_UI_PROJECT_IO_HPP
#define ST_UI_PROJECT_IO_HPP

#include "app_state.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace st::ui {

void open_project_dialog(AppState &state);
void save_project(AppState &state);

// Pure: writes the currently-selected table to `path` as CSV in the same
// shape `subuwutuner-cli project-export-csv` emits. Returns nullopt on
// success or an error message for the status bar / toast.
std::optional<std::string>
write_current_table_csv(AppState const &state, std::filesystem::path const &path, bool diff_only);

void export_current_table_csv_dialog(AppState &state, bool diff_only);
void import_csv_into_current_table_dialog(AppState &state);

} // namespace st::ui

#endif
