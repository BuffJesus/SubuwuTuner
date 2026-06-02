// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tools → Settings modal — Phase 2 step 6 of docs/25. Edits the
// definitions_root + rom_dump_root paths in settings.toml. Atomic save
// via st::config::Config::save (tmp + rename). On success we
// invalidate the registry modal's cached scan so it re-walks against
// the new definitions_root next open.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "persistence.hpp" // save_settings
#include "widgets/widgets.hpp"

#include "st/config.hpp"
#include "st/profile.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>

namespace st::ui {

void render_settings_modal(AppState &state) {
    if (state.show_settings_modal) {
        ImGui::OpenPopup("\xEE\x9C\x93  Settings##settings_modal");
        state.show_settings_modal = false;
        // Refresh from disk every time the modal opens so concurrent
        // edits via the CLI `config set` subcommand don't get clobbered.
        auto cfg_r = st::config::Config::load();
        if (cfg_r.has_value()) {
            std::snprintf(state.settings_def_root_input,
                          sizeof state.settings_def_root_input, "%s",
                          cfg_r->paths().definitions_root.string().c_str());
            std::snprintf(state.settings_rom_dump_root_input,
                          sizeof state.settings_rom_dump_root_input, "%s",
                          cfg_r->paths().rom_dump_root.string().c_str());
            state.settings_status_msg.clear();
        } else {
            state.settings_status_msg =
                "Load failed: " + cfg_r.error().to_string();
            state.settings_status_color = chip_fg_danger();
        }
        state.settings_loaded_once = true;
        // Re-scan vehicle profiles every open — cheap (small dir) and
        // covers the user creating a new profile via the CLI while the
        // GUI is running.
        if (auto profs = st::profile::list(st::profile::default_profile_dir());
            profs.has_value()) {
            state.settings_profiles_cache = std::move(*profs);
        } else {
            state.settings_profiles_cache.clear();
        }
        state.settings_profiles_loaded = true;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Width floor prevents the AlwaysAutoResize+TextWrapped shrink
    // loop (same bug fixed in render_read_rom_modal). Settings
    // modal renders TextWrapped for the post-save status_msg.
    ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 200.0f),
                                        ImVec2(640.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("\xEE\x9C\x93  Settings##settings_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("Config file:");
    ImGui::SameLine();
    auto const cfg_path = st::config::default_config_path().string();
    ImGui::TextDisabled("%s", cfg_path.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s%s",
            cfg_path.c_str(),
            std::filesystem::exists(st::config::default_config_path())
                ? ""
                : "\n(not present — defaults in use until first Save)");
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    float const avail = ImGui::GetContentRegionAvail().x;
    float const btn_w = 96.0f;
    float const input_w = std::max(120.0f, avail - btn_w - 16.0f);

    ImGui::TextUnformatted("Definitions root");
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputText("##settings_def_root", state.settings_def_root_input,
                     sizeof state.settings_def_root_input);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##settings_def_pick", ImVec2(btn_w, 0.0f))) {
        NFD::UniquePathU8 out;
        if (NFD::PickFolder(out) == NFD_OKAY) {
            std::snprintf(state.settings_def_root_input,
                          sizeof state.settings_def_root_input, "%s",
                          out.get());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Where PackRegistry looks for <platform>.zip archives and\n"
            "loose <id>.toml overrides. See docs/17 and docs/25.");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextUnformatted("ROM dump root");
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputText("##settings_rom_root", state.settings_rom_dump_root_input,
                     sizeof state.settings_rom_dump_root_input);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##settings_rom_pick", ImVec2(btn_w, 0.0f))) {
        NFD::UniquePathU8 out;
        if (NFD::PickFolder(out) == NFD_OKAY) {
            std::snprintf(state.settings_rom_dump_root_input,
                          sizeof state.settings_rom_dump_root_input, "%s",
                          out.get());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Default destination for rom-pull captures when no\n"
            "--output is given. Must be writable by the running user\n"
            "(not Program Files).");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // -------- Active vehicle profile (analyst Issue #7) --------------
    // Picker over .stprofile files in default_profile_dir(). Selecting
    // a profile writes settings.toml immediately — no separate Save
    // because the active-profile id is GUI state, not part of the
    // installer paths the Save button below covers.
    ImGui::TextUnformatted("Active vehicle profile");
    auto const &profs = state.settings_profiles_cache;
    auto const current_idx = [&]() -> int {
        if (state.settings.active_vehicle_profile_id.empty())
            return -1;
        for (int i = 0; i < static_cast<int>(profs.size()); ++i) {
            if (profs[static_cast<std::size_t>(i)].id == state.settings.active_vehicle_profile_id)
                return i;
        }
        return -1;
    }();
    char const *current_label = "(none)";
    std::string current_label_buf;
    if (current_idx >= 0) {
        auto const &p = profs[static_cast<std::size_t>(current_idx)];
        current_label_buf = p.display_name.empty() ? p.id
                                                  : (p.display_name + " — " + p.id);
        current_label = current_label_buf.c_str();
    } else if (!state.settings.active_vehicle_profile_id.empty()) {
        current_label_buf = state.settings.active_vehicle_profile_id + " (missing on disk)";
        current_label = current_label_buf.c_str();
    }
    ImGui::SetNextItemWidth(input_w);
    if (ImGui::BeginCombo("##settings_active_profile", current_label)) {
        bool const none_sel = state.settings.active_vehicle_profile_id.empty();
        if (ImGui::Selectable("(none)", none_sel)) {
            state.settings.active_vehicle_profile_id.clear();
            save_settings(state.settings);
        }
        for (auto const &p : profs) {
            std::string const label = p.display_name.empty() ? p.id
                                                            : (p.display_name + " — " + p.id);
            bool const sel = (p.id == state.settings.active_vehicle_profile_id);
            if (ImGui::Selectable(label.c_str(), sel)) {
                state.settings.active_vehicle_profile_id = p.id;
                save_settings(state.settings);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh##profiles", ImVec2(btn_w, 0.0f))) {
        if (auto r = st::profile::list(st::profile::default_profile_dir());
            r.has_value()) {
            state.settings_profiles_cache = std::move(*r);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-scan %s for .stprofile files.",
                          st::profile::default_profile_dir().string().c_str());
    }
    if (current_idx >= 0) {
        auto const &p = profs[static_cast<std::size_t>(current_idx)];
        text_subtle("%s %s %s%s%s%s%s",
                    p.year.empty() ? "" : p.year.c_str(),
                    p.make.empty() ? "" : p.make.c_str(),
                    p.model.empty() ? "" : p.model.c_str(),
                    p.transmission.empty() ? "" : "  ·  ",
                    p.transmission.c_str(),
                    p.transport_hint.empty() ? "" : "  ·  ",
                    p.transport_hint.c_str());
    } else {
        text_subtle("Manage profiles via `subuwutuner-cli profile create/import/show`. "
                    "Profiles dir: %s",
                    st::profile::default_profile_dir().string().c_str());
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

    // Button-row convention (matches unsaved_modal, csv_import_modal,
    // autotune modals, new_project_modal):
    //   primary: 160 wide, accent-filled, Enter shortcut, tooltip
    //   secondary/destructive: ~140 wide, no accent, tooltip
    //   cancel/close: ~110 wide, Esc shortcut, tooltip
    bool const want_save = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);

    bool save_clicked = false;
    {
        push_primary_button_colors();
        save_clicked = ImGui::Button("Save", ImVec2(160.0f, 0.0f));
        pop_primary_button_colors();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write definitions_root and rom_dump_root\n"
                              "to settings.toml.  (Enter)");
        }
    }
    if (save_clicked || want_save) {
        auto cfg_r = st::config::Config::load();
        if (!cfg_r.has_value()) {
            state.settings_status_msg =
                "Load failed: " + cfg_r.error().to_string();
            state.settings_status_color = chip_fg_danger();
        } else {
            cfg_r->paths().definitions_root = state.settings_def_root_input;
            cfg_r->paths().rom_dump_root = state.settings_rom_dump_root_input;
            auto s = cfg_r->save();
            if (s.has_value()) {
                state.settings_status_msg =
                    "Saved to " + cfg_r->source_path().string();
                state.settings_status_color = chip_fg_ok();
                // Invalidate the registry modal's cached scan so the
                // next open re-walks against the new definitions_root.
                state.def_registry_scanned = false;
                state.def_registry_rows.clear();
                state.def_registry_warnings.clear();
            } else {
                state.settings_status_msg =
                    "Save failed: " + s.error().to_string();
                state.settings_status_color = chip_fg_danger();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore Defaults", ImVec2(140.0f, 0.0f))) {
        auto defs = st::config::Config::defaults();
        std::snprintf(state.settings_def_root_input,
                      sizeof state.settings_def_root_input, "%s",
                      defs.paths().definitions_root.string().c_str());
        std::snprintf(state.settings_rom_dump_root_input,
                      sizeof state.settings_rom_dump_root_input, "%s",
                      defs.paths().rom_dump_root.string().c_str());
        state.settings_status_msg =
            "Defaults restored in form (click Save to persist).";
        state.settings_status_color = chip_fg_warn();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Populate the form with built-in defaults.\n"
                          "Click Save to persist them.");
    }
    ImGui::SameLine();
    bool const close_clicked = ImGui::Button("Close", ImVec2(110.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Close without persisting any pending changes.  (Esc)");
    }
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }

    if (!state.settings_status_msg.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        ImGui::PushStyleColor(ImGuiCol_Text, state.settings_status_color);
        ImGui::TextWrapped("%s", state.settings_status_msg.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndPopup();
}

} // namespace st::ui
