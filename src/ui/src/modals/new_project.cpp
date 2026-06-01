// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// New-project modal. GUI parity with `subuwutuner-cli project-new`:
// three path inputs (source ROM, def pack folder, target dir) plus an
// optional display name. Each Browse… button fires NFD and rewrites
// the matching buffer. Create calls Project::create and, on success,
// hands the new directory off to try_open_project so the caller lands
// in the open project immediately.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"
#include "st/project.hpp"
#include "st/rom.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>

namespace st::ui {

void render_new_project_modal(AppState &state) {
    if (state.show_new_project_modal) {
        ImGui::OpenPopup("New project##new_project_modal");
        state.show_new_project_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 280.0f), ImVec2(900.0f, 600.0f));
    if (!ImGui::BeginPopupModal("New project##new_project_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // One row = label, text-input that fills the horizontal space, and
    // a Browse... button. ImGui::Begin/EndDisabled wraps the text input
    // to keep it read-only — the canonical-path constraint is "use
    // Browse..."; typing in random paths invites mistakes.
    auto const path_row = [](char const *label, char *buf, std::size_t buf_size, char const *btn_id,
                             char const *tooltip) -> bool {
        ImGui::TextUnformatted(label);
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w - 8.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText((std::string{"##"} + btn_id).c_str(), buf, buf_size,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        bool const clicked =
            ImGui::Button((std::string{"Browse…##"} + btn_id).c_str(), ImVec2(btn_w, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return clicked;
    };

    if (path_row("Source ROM", state.np_source_path, sizeof state.np_source_path, "src",
                 "Pick the stock ROM dump (.bin). Copied into the\n"
                 "project as source.bin and never modified.")) {
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::OpenDialog(out);
        if (r == NFD_OKAY) {
            std::snprintf(state.np_source_path, sizeof state.np_source_path, "%s", out.get());
        } else if (r == NFD_ERROR) {
            state.status_msg = std::string{"Source dialog error: "} + NFD::GetError();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // Definition-pack row: custom because it needs two pickers
    // (multi-file directory layout vs. single-file pack).
    {
        ImGui::TextUnformatted("Definition pack");
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w * 2.0f - 16.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText("##def_path", state.np_def_path, sizeof state.np_def_path,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Folder…##def_folder", ImVec2(btn_w, 0.0f))) {
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::PickFolder(out);
            if (r == NFD_OKAY) {
                std::snprintf(state.np_def_path, sizeof state.np_def_path, "%s", out.get());
            } else if (r == NFD_ERROR) {
                state.status_msg = std::string{"Def dialog error: "} + NFD::GetError();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pick a directory laid out as a multi-file\n"
                              "pack (must contain pack.toml).");
        }
        ImGui::SameLine();
        if (ImGui::Button("File…##def_file", ImVec2(btn_w, 0.0f))) {
            nfdu8filteritem_t filters[1] = {{"TOML pack", "toml"}};
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
            if (r == NFD_OKAY) {
                std::snprintf(state.np_def_path, sizeof state.np_def_path, "%s", out.get());
            } else if (r == NFD_ERROR) {
                state.status_msg = std::string{"Def dialog error: "} + NFD::GetError();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pick a single-file TOML pack\n"
                              "(one ROM/CID per file).");
        }
        // First-run hint. The single most common new-user question
        // ("where do I get a pack?") deserves a visible answer in
        // the modal, not a buried doc reference. Kept compact +
        // muted (text_subtle, not TextDisabled — this is a hint,
        // not an inactive control) so it doesn't compete with the
        // inputs.
        text_subtle("Don't have a pack? Try the bundled "
                    "fixtures/demo-pack/ to explore the UI, or see "
                    "docs/11-definition-format.md to author one.");
    }

    // Inline ROM/def match check. Recompute only when either path has
    // changed since the last run — both loaders touch disk, and the
    // modal would otherwise re-load every frame.
    if (state.np_source_path[0] != '\0' && state.np_def_path[0] != '\0') {
        std::string const cur_source = state.np_source_path;
        std::string const cur_def = state.np_def_path;
        if (cur_source != state.np_cached_source_path || cur_def != state.np_cached_def_path) {
            state.np_cached_source_path = cur_source;
            state.np_cached_def_path = cur_def;
            // Path changed — last create attempt's error is stale.
            state.np_create_error.clear();
            auto rom_r = st::Rom::from_file(cur_source);
            if (!rom_r.has_value()) {
                state.np_match_status = AppState::NpMatchStatus::LoadFailed;
                state.np_match_message = "ROM load failed: " + rom_r.error().to_string();
            } else {
                auto def_r = st::Definition::from_file(cur_def);
                if (!def_r.has_value()) {
                    state.np_match_status = AppState::NpMatchStatus::LoadFailed;
                    state.np_match_message = "Pack load failed: " + def_r.error().to_string();
                } else {
                    auto const name = def_r->matches(*rom_r);
                    if (name.has_value()) {
                        state.np_match_status = AppState::NpMatchStatus::Match;
                        state.np_match_message = "Matches \"" + *name + "\"";
                    } else {
                        state.np_match_status = AppState::NpMatchStatus::NoMatch;
                        state.np_match_message = "No matching identification "
                                                 "in this pack — Create still "
                                                 "allowed.";
                    }
                }
            }
        }
    } else {
        state.np_match_status = AppState::NpMatchStatus::None;
        state.np_match_message.clear();
        state.np_cached_source_path.clear();
        state.np_cached_def_path.clear();
    }
    if (state.np_match_status != AppState::NpMatchStatus::None) {
        ImVec4 color{1, 1, 1, 1};
        char const *prefix = "";
        switch (state.np_match_status) {
        case AppState::NpMatchStatus::Match:
            color = ImVec4(0.40f, 0.82f, 0.45f, 1.0f);
            prefix = "\xE2\x9C\x93 "; // ✓
            break;
        case AppState::NpMatchStatus::NoMatch:
            color = ImVec4(0.95f, 0.78f, 0.30f, 1.0f);
            prefix = "\xE2\x9A\xA0 "; // ⚠
            break;
        case AppState::NpMatchStatus::LoadFailed:
            color = chip_fg_danger();
            prefix = "\xE2\x9C\x97 "; // ✗
            break;
        case AppState::NpMatchStatus::None:
            break;
        }
        ImGui::TextColored(color, "%s%s", prefix, state.np_match_message.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    if (path_row("Project directory", state.np_dir_path, sizeof state.np_dir_path, "dir",
                 "Pick the empty target directory. project.toml,\n"
                 "source.bin, and working.bin land here.")) {
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::PickFolder(out);
        if (r == NFD_OKAY) {
            std::snprintf(state.np_dir_path, sizeof state.np_dir_path, "%s", out.get());
        } else if (r == NFD_ERROR) {
            state.status_msg = std::string{"Dir dialog error: "} + NFD::GetError();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    ImGui::TextUnformatted("Display name (optional)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##np_name", "Defaults to the project directory's basename.",
                             state.np_display_name, sizeof state.np_display_name);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Inline create error from a prior failed click, rendered ABOVE
    // the buttons so retries don't have to chase the eye to the
    // bottom of the modal. Kept compact (one red line) — detailed
    // diagnostics go in tooltips on the offending field.
    if (!state.np_create_error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "\xE2\x9C\x97 Create failed: %s",
                           state.np_create_error.c_str());
        ImGui::Spacing();
    }

    bool const have_source = state.np_source_path[0] != '\0';
    bool const have_def = state.np_def_path[0] != '\0';
    bool const have_dir = state.np_dir_path[0] != '\0';
    bool const load_failed = state.np_match_status == AppState::NpMatchStatus::LoadFailed;
    bool const can_create = have_source && have_def && have_dir && !load_failed;

    bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    bool create_clicked = false;
    {
        push_primary_button_colors();
        ImGui::BeginDisabled(!can_create);
        create_clicked = ImGui::Button("Create", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        pop_primary_button_colors();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!have_source || !have_def || !have_dir) {
                ImGui::SetTooltip("Source ROM, definition pack, and "
                                  "project directory are all required.");
            } else if (load_failed) {
                ImGui::SetTooltip("Cannot create: ROM or pack failed to "
                                  "load. See message above.");
            } else {
                ImGui::SetTooltip("Create the project and open it.");
            }
        }
    }
    ImGui::SameLine();
    bool const cancel_clicked = ImGui::Button("Cancel", ImVec2(110.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Close without creating.  (Esc)");
    }

    auto const reset_fields = [&]() {
        state.np_source_path[0] = '\0';
        state.np_def_path[0] = '\0';
        state.np_dir_path[0] = '\0';
        state.np_display_name[0] = '\0';
        state.np_match_status = AppState::NpMatchStatus::None;
        state.np_match_message.clear();
        state.np_cached_source_path.clear();
        state.np_cached_def_path.clear();
        state.np_create_error.clear();
    };

    if (create_clicked && can_create) {
        std::filesystem::path const dir{state.np_dir_path};
        std::string name{state.np_display_name};
        if (name.empty()) {
            name = dir.filename().string();
            if (name.empty())
                name = "Untitled";
        }
        auto r = st::Project::create(dir, std::filesystem::path{state.np_source_path},
                                     std::filesystem::path{state.np_def_path}, name);
        if (!r.has_value()) {
            // Inline error only — status_msg would compete with the
            // modal's own surface. The error renders above the
            // buttons on next frame.
            state.np_create_error = r.error().to_string();
        } else {
            reset_fields();
            ImGui::CloseCurrentPopup();
            // try_open_project handles status_msg + recents update.
            state.try_open_project(dir);
        }
    } else if (cancel_clicked || want_cancel) {
        reset_fields();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace st::ui
