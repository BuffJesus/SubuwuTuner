// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Help → Keyboard shortcuts reference modal. Read-only, scrollable,
// two columns. Companion ShortcutRow / ShortcutGroup / shortcuts_reference
// are file-local (no cross-file callers per the resume handoff).

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

#include <vector>

namespace st::ui {
namespace {

// Static help reference. Lists every binding the GUI listens for,
// grouped by context. Single source of truth — when a new shortcut
// lands, add a row here.
struct ShortcutRow {
    char const *binding;
    char const *description;
};
struct ShortcutGroup {
    char const *heading;
    std::vector<ShortcutRow> rows;
};

std::vector<ShortcutGroup> const &shortcuts_reference() {
    static std::vector<ShortcutGroup> const groups = {
        {"Global",
         {
             {"Ctrl+K", "Open command palette (search every action + table)"},
             {"F1", "Open in-app help (topics + glossary)"},
             {"Ctrl+O", "Open project…"},
             {"Ctrl+S", "Save project"},
             {"Ctrl+Q", "Quit (with unsaved-changes guard)"},
             {"Ctrl+F", "Focus the sidebar table filter"},
             {"Ctrl+Z", "Undo last edit"},
             {"Ctrl+Shift+Z", "Redo (alt: Ctrl+Y)"},
         }},
        {"Workspaces",
         {
             {"Ctrl+1", "Switch to Tune workspace"},
             {"Ctrl+2", "Switch to Datalog workspace"},
             {"Ctrl+3", "Switch to Features workspace"},
         }},
        {"Table grid",
         {
             {"Arrows", "Move cursor cell"},
             {"Shift+Arrows", "Extend selection from cursor"},
             {"Click", "Select cell"},
             {"Shift+Click", "Extend selection to clicked cell"},
             {"F2", "Start editing the cursor cell"},
             {"Enter", "Commit value (or Ctrl+Enter to "
                       "fill every selected cell)"},
             {"Esc", "Cancel the in-cell editor"},
             {"Ctrl+C", "Copy selection as tab-separated "
                        "values"},
             {"Ctrl+V", "Paste tab-separated values at "
                        "cursor"},
         }},
        {"Modals",
         {
             {"Enter", "Primary action (Save / Apply / "
                       "Create — whatever is the "
                       "highlighted button)"},
             {"Esc", "Close / cancel without applying"},
         }},
        {"Designer canvas",
         {
             {"Click", "Select a node or edge"},
             {"Shift+Click", "Toggle a node in the selection"},
             {"Click (empty)", "Clear selection"},
             {"Drag (empty)", "Box-select nodes inside the "
                              "rectangle (Shift to add)"},
             {"Delete", "Remove the selected nodes / edge"},
             {"Drag body", "Move the selected nodes as a group "
                           "(snaps to grid on release)"},
             {"Drag pin → pin", "Wire two pins"},
             {"Middle-drag", "Pan the canvas"},
             {"Mouse wheel", "Zoom in/out (anchored to cursor)"},
             {"Reset view", "Toolbar button — reset pan + zoom"},
             {"Esc / Right-click", "Cancel an in-progress wire"},
             {"Right-click", "Context menu (delete node, "
                             "disconnect edge / pin)"},
         }},
    };
    return groups;
}

} // namespace

void render_shortcuts_modal(AppState &state) {
    if (state.show_shortcuts_modal) {
        ImGui::OpenPopup("\xEE\xA4\xAE  Keyboard shortcuts##shortcuts_modal");
        state.show_shortcuts_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 380.0f), ImVec2(900.0f, 720.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA4\xAE  Keyboard shortcuts##shortcuts_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    text_subtle("All bindings the GUI listens for, grouped by "
                "context.");
    ImGui::Spacing();

    for (auto const &group : shortcuts_reference()) {
        ImGui::SeparatorText(group.heading);
        if (ImGui::BeginTable(group.heading, 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("description", ImGuiTableColumnFlags_WidthStretch);
            for (auto const &row : group.rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.binding);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.description);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();
    bool const close_clicked = ImGui::Button("Close", ImVec2(120.0f, 0.0f));
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
