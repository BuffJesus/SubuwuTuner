// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unsaved-changes confirmation modal — gates Open / New / Close / Quit
// when state.dirty. File-local modal_save_label / modal_discard_label /
// modal_subtitle pick action-specific verbs so the button text reads
// natural for whichever flow opened the modal.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "project_io.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

namespace st::ui {
namespace {

// Action-specific labels for the unsaved-changes modal. Keeping these
// pure verbs scoped to the modal so they don't accidentally read like
// general "open / close / quit" handlers elsewhere.
char const *modal_save_label(ConfirmAction a) noexcept {
    switch (a) {
    case ConfirmAction::OpenDialog:
    case ConfirmAction::OpenRecent:
        return "Save and open";
    case ConfirmAction::NewProject:
        return "Save and create new";
    case ConfirmAction::Close:
        return "Save and close";
    case ConfirmAction::Quit:
        return "Save and quit";
    case ConfirmAction::None:
        break;
    }
    return "Save and continue";
}

char const *modal_discard_label(ConfirmAction a) noexcept {
    switch (a) {
    case ConfirmAction::OpenDialog:
    case ConfirmAction::OpenRecent:
        return "Discard and open";
    case ConfirmAction::NewProject:
        return "Discard and create new";
    case ConfirmAction::Close:
        return "Discard and close";
    case ConfirmAction::Quit:
        return "Discard and quit";
    case ConfirmAction::None:
        break;
    }
    return "Discard changes";
}

char const *modal_subtitle(ConfirmAction a) noexcept {
    switch (a) {
    case ConfirmAction::OpenDialog:
    case ConfirmAction::OpenRecent:
        return "Opening another project will replace this one.";
    case ConfirmAction::NewProject:
        return "Creating a new project will replace this one.";
    case ConfirmAction::Close:
        return "Closing this project will reset the editor.";
    case ConfirmAction::Quit:
        return "Quitting will exit SubuwuTuner.";
    case ConfirmAction::None:
        break;
    }
    return "Continuing without saving will discard them.";
}

} // namespace

void render_unsaved_modal(AppState &state) {
    if (state.show_unsaved_modal) {
        ImGui::OpenPopup("Unsaved changes##unsaved");
        state.show_unsaved_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes##unsaved", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ConfirmAction const what = state.next_action;

        ImGui::TextUnformatted("You have unsaved edits in this project.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        text_subtle("%s", modal_subtitle(what));
        ImGui::Dummy(ImVec2(0.0f, kSpaceL));

        // Keyboard shortcuts: Enter = the safe default (Save).
        // Esc = the safe undo (Cancel). Destructive Discard
        // requires an explicit click — no accelerator on purpose.
        bool const want_save = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
        bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);

        constexpr float kBtnW = 180.0f;
        // Save is the default action (Enter) and the safe path — give it
        // accent fill so the eye lands on it first.
        push_primary_button_colors();
        bool const save_clicked = ImGui::Button(modal_save_label(what), ImVec2(kBtnW, 0.0f));
        pop_primary_button_colors();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write the working ROM + edits to disk, "
                              "then proceed.  (Enter)");
        }
        ImGui::SameLine();
        bool const discard_clicked = ImGui::Button(modal_discard_label(what), ImVec2(kBtnW, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Throw away every edit since the last save "
                              "and proceed.");
        }
        ImGui::SameLine();
        bool const cancel_clicked = ImGui::Button("Cancel", ImVec2(kBtnW * 0.7f, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stay here. Don't open, close, or quit.  "
                              "(Esc)");
        }

        if (save_clicked || want_save) {
            save_project(state);
            execute_action(state, state.next_action, state.next_recent);
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        } else if (discard_clicked) {
            state.dirty = false;
            execute_action(state, state.next_action, state.next_recent);
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        } else if (cancel_clicked || want_cancel) {
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace st::ui
