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
#include "theme.hpp"       // apply_theme, accent_for
#include "widgets/widgets.hpp"

#include "st/config.hpp"
#include "st/profile.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
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
        // Project metadata buffers — load from the currently open
        // project. Empty when no project is loaded; the editor section
        // below renders a hint in that case.
        if (state.project.has_value()) {
            std::snprintf(state.settings_project_display_name,
                          sizeof state.settings_project_display_name, "%s",
                          state.project->display_name().c_str());
            std::snprintf(state.settings_project_notes,
                          sizeof state.settings_project_notes, "%s",
                          state.project->notes().c_str());
        } else {
            state.settings_project_display_name[0] = '\0';
            state.settings_project_notes[0] = '\0';
        }
        state.settings_project_dirty = false;
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
    ImGui::SameLine();
    // Reveal in OS file browser — mirrors the audit panel's reveal
    // button. Disabled when the config file hasn't been created yet
    // (default-paths-in-use state).
    bool const cfg_exists = std::filesystem::exists(st::config::default_config_path());
    ImGui::BeginDisabled(!cfg_exists);
    if (ImGui::SmallButton("Reveal##cfg_reveal")) {
        std::string cmd;
#if defined(_WIN32)
        cmd = "explorer /select,\"" + cfg_path + "\"";
#elif defined(__APPLE__)
        cmd = "open -R \"" + cfg_path + "\"";
#else
        cmd = "xdg-open \"" + st::config::default_config_path().parent_path().string() + "\"";
#endif
        (void)std::system(cmd.c_str());
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(cfg_exists
                              ? "Reveal config.toml in the OS file browser."
                              : "Click Save first to create the config file.");
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

    // -------- Theme picker with live preview swatch ------------------
    // Closes the last item in the 2026-06-02 UX-polish backlog. The
    // theme also persists via the View → Theme menu + the first-run
    // wizard; this surface adds a side-by-side preview so the user
    // can see the accent / chip palette under each theme before
    // committing. Clicking Dark / Light applies the theme immediately
    // (apply_theme already mutates the live ImGui style); the Settings
    // Save button flushes settings.txt to disk.
    ImGui::TextUnformatted("Theme");
    bool const is_dark_now = state.settings.theme == Theme::Dark;
    if (ImGui::RadioButton("Dark##settings_theme_dark", is_dark_now)) {
        state.settings.theme = Theme::Dark;
        apply_theme(state.settings.theme);
        save_settings(state.settings);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Light##settings_theme_light",
                           state.settings.theme == Theme::Light)) {
        state.settings.theme = Theme::Light;
        apply_theme(state.settings.theme);
        save_settings(state.settings);
    }
    // Side-by-side preview swatches. Each row shows the theme's name,
    // accent triple (base / hover / active), and the chip palette
    // colors a tuner sees most often — accent / warn / caution /
    // danger / ok / muted. Renders both themes regardless of the
    // active selection so the user can compare without flipping.
    auto const draw_swatch = [](ImVec4 const &c) {
        constexpr float kSwatchW = 22.0f;
        constexpr float kSwatchH = 16.0f;
        ImVec2 const p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImVec2(p.x + kSwatchW, p.y + kSwatchH),
            ImGui::ColorConvertFloat4ToU32(c), 3.0f);
        ImGui::Dummy(ImVec2(kSwatchW, kSwatchH));
    };
    auto const draw_theme_row = [&](Theme t, char const *label) {
        // Mark the active theme so the user can see at a glance which
        // row is rendering the current style.
        bool const active = state.settings.theme == t;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_accent());
        }
        ImGui::Text("%s%s", active ? "\xE2\x96\xB6 " : "  ", label);
        if (active) {
            ImGui::PopStyleColor();
        }
        auto const acc = accent_for(t);
        ImGui::SameLine();
        draw_swatch(acc.base);
        ImGui::SameLine();
        draw_swatch(acc.hover);
        ImGui::SameLine();
        draw_swatch(acc.active);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Accent triple — base / hover / active.\n"
                              "Used for primary-action buttons (Save, Apply,\n"
                              "Compare) + slider grabs + active header rows.");
        }
    };
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    draw_theme_row(Theme::Dark, "Dark ");
    draw_theme_row(Theme::Light, "Light");

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
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // -------- Project metadata --------------------------------------
    // Display name + notes for the currently-open project. Empty when
    // no project is loaded. Edits flush on the modal's Save button
    // alongside the installer-paths so a single Save persists everything.
    ImGui::TextUnformatted("Project metadata");
    if (state.project.has_value()) {
        ImGui::SetNextItemWidth(input_w);
        if (ImGui::InputText("Display name##settings_proj_display",
                             state.settings_project_display_name,
                             sizeof state.settings_project_display_name)) {
            state.settings_project_dirty = true;
        }
        ImGui::SetNextItemWidth(input_w);
        if (ImGui::InputTextMultiline("Notes##settings_proj_notes",
                                      state.settings_project_notes,
                                      sizeof state.settings_project_notes,
                                      ImVec2(input_w, 80.0f))) {
            state.settings_project_dirty = true;
        }
        text_subtle("Stored in <project>/project.toml. Saved when you click Save below.");
    } else {
        text_subtle("Open a project first to edit its display name + notes.");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // -------- Validate active pack -----------------------------------
    // Surfaces `subuwutuner-cli pack-lint` against whatever pack the
    // currently open project loaded. Pack authors and tuners both want
    // a "is the loaded pack well-formed?" indicator that doesn't make
    // them drop to a terminal. Result is cached on AppState so flipping
    // away and back keeps the last status visible until the user
    // re-validates.
    ImGui::TextUnformatted("Active pack");
    ImGui::BeginDisabled(!state.project.has_value());
    if (ImGui::Button("Validate pack##settings_pack_lint", ImVec2(160.0f, 0.0f))) {
        auto const v = state.project->definition().validate();
        state.settings_pack_lint_pack_id = state.project->definition().pack().id;
        if (v.has_value()) {
            state.settings_pack_lint_status = 0;
            state.settings_pack_lint_message.clear();
        } else {
            // One Status carries every violation joined by '\n'.
            // Count lines so the chip can say "N violations" rather
            // than "validation failed" — pack authors fix the count
            // down, not flip a boolean.
            auto const msg = v.error().to_string();
            int violations = msg.empty() ? 0 : 1;
            for (char c : msg) {
                if (c == '\n') {
                    ++violations;
                }
            }
            state.settings_pack_lint_status = violations;
            state.settings_pack_lint_message = msg;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.project.has_value()) {
            ImGui::SetTooltip("Run Definition::validate() on the currently\n"
                              "loaded pack. Equivalent to:\n"
                              "  subuwutuner-cli pack-lint <pack>");
        } else {
            ImGui::SetTooltip("Open a project first — Validate works\n"
                              "against the project's loaded pack.");
        }
    }
    if (state.project.has_value()) {
        ImGui::SameLine();
        text_subtle("Pack: %s",
                    state.project->definition().pack().id.c_str());
    } else {
        ImGui::SameLine();
        text_subtle("(no project loaded)");
    }
    if (state.settings_pack_lint_status >= 0) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        if (state.settings_pack_lint_status == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
            ImGui::Text("\xE2\x9C\x93  OK  \xC2\xB7  %s",
                        state.settings_pack_lint_pack_id.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
            ImGui::Text("\xE2\x9C\x95  %d violation%s  \xC2\xB7  %s",
                        state.settings_pack_lint_status,
                        state.settings_pack_lint_status == 1 ? "" : "s",
                        state.settings_pack_lint_pack_id.c_str());
            ImGui::PopStyleColor();
            // Bounded violation viewport — full text in a scrollable
            // child so a 60-line dump doesn't push Save off-screen.
            // Height caps at ~6 rows; user scrolls for the rest.
            ImGui::BeginChild("##pack_lint_msg",
                              ImVec2(0.0f,
                                     ImGui::GetTextLineHeightWithSpacing() * 6.0f),
                              ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(state.settings_pack_lint_message.c_str());
            ImGui::EndChild();
        }
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
        // Project metadata side — independent of the config-file save.
        // We only write project.toml when the user actually edited the
        // fields; otherwise leave the on-disk mtime alone.
        if (state.settings_project_dirty && state.project.has_value()) {
            state.project->set_display_name(state.settings_project_display_name);
            state.project->set_notes(state.settings_project_notes);
            if (auto sp = state.project->save_metadata(); !sp.has_value()) {
                state.settings_status_msg +=
                    "  (project metadata save failed: " + sp.error().to_string() + ")";
                state.settings_status_color = chip_fg_danger();
            }
            state.settings_project_dirty = false;
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
