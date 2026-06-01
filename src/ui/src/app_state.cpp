// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Non-trivial AppState methods. Kept out-of-class because their bodies
// reach into the toast queue + recents persistence (live in panels/
// + persistence.cpp respectively).

#include "app_state.hpp"

#include "panels/panels.hpp"
#include "persistence.hpp"

#include "st/project.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace st::ui {

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

} // namespace st::ui
