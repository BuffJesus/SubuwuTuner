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

#include <cstdio>
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
        // Cross-session AuditLog wiring (analyst Issue #8): tee every
        // append into <config>/audit-by-cid/<CID>.log so a tech can
        // pull this car's history across every project that touched
        // it. Identity comes from the active VehicleProfile; empty CID
        // → tee is a no-op (project log only).
        auto const identity = resolve_active_vehicle_identity(settings);
        audit_log->set_default_cid(identity.cid);
        audit_log->set_default_vin(identity.vin);
        audit_log->set_per_cid_sink(audit_by_cid_dir());
        (void)audit_log->log(st::audit::EntryKind::ProjectOpened, "ui",
                             "Project opened",
                             {{"dir", project->dir().string()},
                              {"display_name", project->display_name()}});
    }
    enqueue_toast(*this, ToastKind::Success,
                  std::string{"Loaded "} + path.filename().string());
    push_recent(recents, path);
    save_recents(recents);
    // Load any persisted sidebar category ordering — empty when the
    // user hasn't dragged anything yet. The sidebar falls back to
    // pack-default first-occurrence order in that case.
    sidebar_category_order = load_sidebar_category_order(project->dir());
    sidebar_hidden_categories = load_sidebar_hidden_categories(project->dir());
    {
        auto const pinned = load_compare_pinned(project->dir());
        compare_pinned_table_ids.clear();
        for (auto const &id : pinned) {
            compare_pinned_table_ids.insert(id);
        }
    }
    // Restore the last-comparison setup (ROM A/B paths, epsilon,
    // include_identical, filter chip) so the user picks up where
    // they left off. Reset to defaults FIRST so a project-switch
    // into a project with no compare.config doesn't leak the
    // previous project's Compare picker values.
    compare_rom_a_path[0] = '\0';
    compare_rom_b_path[0] = '\0';
    compare_epsilon = 0.0f;
    compare_include_identical = false;
    compare_filter_chip[0] = '\0';
    if (auto cfg = load_compare_config(project->dir()); cfg.has_value()) {
        std::snprintf(compare_rom_a_path, sizeof compare_rom_a_path, "%s",
                      cfg->rom_a_path.c_str());
        std::snprintf(compare_rom_b_path, sizeof compare_rom_b_path, "%s",
                      cfg->rom_b_path.c_str());
        compare_epsilon = cfg->epsilon;
        compare_include_identical = cfg->include_identical;
        std::snprintf(compare_filter_chip, sizeof compare_filter_chip, "%s",
                      cfg->filter_chip.c_str());
    }
    // Restore the prior pack-lint result so the Settings chip + welcome
    // panel surface the last verdict without forcing the user to
    // re-validate. Missing or malformed snapshot → "not validated"
    // (status stays -1) and the rest of the UI behaves as before.
    settings_pack_lint_status = -1;
    settings_pack_lint_pack_id.clear();
    settings_pack_lint_message.clear();
    settings_pack_lint_validated_at.clear();
    if (auto snap = load_pack_lint(project->dir()); snap.has_value()) {
        // Only honor the snapshot when the recorded pack_id matches
        // the loaded one — pack-id drift (user repointed the project at
        // a different pack) invalidates the verdict.
        if (snap->pack_id == project->definition().pack().id) {
            settings_pack_lint_status = snap->status;
            settings_pack_lint_pack_id = snap->pack_id;
            settings_pack_lint_message = snap->message;
            settings_pack_lint_validated_at = snap->last_validated_at;
        }
    }
    auto const &tables = project->definition().tables();
    if (!tables.empty()) {
        select_table(tables.front().id);
    }
}

void AppState::select_table(std::string const &id) {
    // Mark the sidebar's auto-open-and-scroll behavior for this frame.
    // Callers might be the sidebar itself (where the user is already
    // looking at the row) or a cross-reference jump from a different
    // panel — the request is harmless in the first case (already-open
    // groups stay open) and meaningful in the second.
    sidebar_open_selected_group_request = true;
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

void AppState::refresh_audit_identity() {
    if (!audit_log.has_value())
        return;
    auto const identity = resolve_active_vehicle_identity(settings);
    audit_log->set_default_cid(identity.cid);
    audit_log->set_default_vin(identity.vin);
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
    sidebar_category_order.clear();
    sidebar_hidden_categories.clear();
    compare_pinned_table_ids.clear();
    compare_pinned_only = false;
}

} // namespace st::ui
