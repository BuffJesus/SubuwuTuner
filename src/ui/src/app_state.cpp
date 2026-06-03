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
        audit_log.reset();
        selected_table_id.clear();
        current_table_data.reset();
        selection.reset();
        last_save_iso.reset();
        active_rom_id.clear();
        return;
    }
    project = std::move(*r);
    status_msg.clear();
    selected_table_id.clear();
    current_table_data.reset();
    selection.reset();
    dirty = false;
    last_save_iso.reset();
    // Sync the GUI's view selection from whatever the project
    // persisted. New / older projects come back with "" = working,
    // which keeps the v1 behavior.
    active_rom_id = project->active_rom_id();
    // Open the audit log handle for this project (analyst Issue #8).
    // Failure is non-fatal — the project still loads, just without
    // audit subscription. The audit panel surfaces past entries from
    // any prior session even if this open fails.
    audit_log.reset();
    if (auto al = st::audit::AuditLog::open(project->dir() / "audit.log");
        al.has_value()) {
        audit_log.emplace(std::move(*al));
        (void)audit_log->log(st::audit::EntryKind::ProjectOpened, "ui",
                             "Project opened",
                             {{"dir", project->dir().string()},
                              {"display_name", project->display_name()}});
    }
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
    auto const *rom = view_rom();
    if (rom == nullptr) {
        return;
    }
    auto td = project->definition().read_table_values(*rom, *table);
    if (td.has_value()) {
        current_table_data = std::move(*td);
    }
}

void AppState::close_project() {
    project.reset();
    audit_log.reset();
    selected_table_id.clear();
    current_table_data.reset();
    selection.reset();
    selected_z = 0;
    status_msg.clear();
    dirty = false;
    last_save_iso.reset();
    active_rom_id.clear();
}

} // namespace st::ui
