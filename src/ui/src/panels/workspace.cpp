// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Workspace rail + dockspace host. Each WorkspaceMode (Tune / Datalog
// / Features) owns a complete dock tree built by build_workspace_layout
// — switching modes drops the saved tree and rebuilds, so each
// workspace lands in its canonical arrangement. apply_workspace_mode
// flips the per-panel show_* flags into a coherent set for the chosen
// mode and triggers a layout reset; request_layout_reset is the
// publicly-exposed seam for the View menu's "Reset window layout"
// item and the Ctrl+K palette's matching command.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <string>

namespace st::ui {

// Flag set by request_layout_reset() and consumed at the top of the
// next render_dockspace_host() invocation. Drives the "Reset window
// layout" command in View / Ctrl+K — drops the saved dock tree and
// rebuilds via build_workspace_layout below.
namespace {
bool g_request_dock_reset = false;
}
void request_layout_reset() { g_request_dock_reset = true; }

// Apply a workspace preset: flip show_*_panel flags into a coherent set
// for the chosen mode, leave dock positions alone (imgui.ini owns
// those). User can still toggle individual panels afterwards — the
// workspace highlight in the rail just tracks the last preset applied.
void apply_workspace_mode(AppState &state, WorkspaceMode mode) {
    state.workspace_mode = mode;
    switch (mode) {
    case WorkspaceMode::Tune:
        state.show_stats_panel = true;
        state.show_history_panel = true;
        state.show_dtcs_panel = true;
        state.show_knock_dashboard_panel = false;
        state.show_adaptive_history_panel = false;
        state.show_coldstart_panel = false;
        state.show_ebcs_panel = false;
        state.show_features_designer = false;
        break;
    case WorkspaceMode::Datalog:
        state.show_stats_panel = false;
        state.show_history_panel = false;
        // DTCs stays — datalog workflows frequently cross-reference
        // active codes against observed signals.
        state.show_dtcs_panel = true;
        state.show_knock_dashboard_panel = true;
        state.show_adaptive_history_panel = true;
        state.show_coldstart_panel = true;
        state.show_ebcs_panel = true;
        state.show_features_designer = false;
        break;
    case WorkspaceMode::Features:
        state.show_stats_panel = false;
        state.show_history_panel = false;
        state.show_dtcs_panel = false;
        state.show_knock_dashboard_panel = false;
        state.show_adaptive_history_panel = false;
        state.show_coldstart_panel = false;
        state.show_ebcs_panel = false;
        state.show_features_designer = true;
        break;
    }
    // Tell render_dockspace_host to rebuild the dock tree for the new
    // mode on the next frame. Distinct work areas — switching mode
    // resets the user's intra-workspace dock rearrangements to that
    // workspace's canonical layout. Users who customize within a
    // workspace see the reset on switch; intentional, makes switching
    // feel like flipping tabs in a browser rather than toggling
    // panels.
    request_layout_reset();
}

// Left-rail workspace switcher. Sits between the menubar and the status
// bar, full-height on the left edge. Three preset buttons (Tune / Log /
// Feat) flip panel visibility via apply_workspace_mode; a separator
// then a Flash quick-action button at the bottom. Active preset is
// drawn in the brand-accent color so the user always knows which
// workspace they're in.
void render_workspace_rail(AppState &state) {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(
        ImVec2(kWorkspaceRailWidth, vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 8.0f));

    ImGuiWindowFlags const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##workspace_rail", nullptr, flags);
    ImGui::PopStyleVar(3);

    constexpr float kBtnW = 48.0f;
    constexpr float kBtnH = 44.0f;
    auto const draw_workspace = [&](WorkspaceMode mode, char const *label,
                                    char const *tooltip) {
        bool const is_active = (state.workspace_mode == mode);
        if (is_active) {
            push_primary_button_colors();
        }
        if (ImGui::Button(label, ImVec2(kBtnW, kBtnH))) {
            apply_workspace_mode(state, mode);
        }
        if (is_active) {
            pop_primary_button_colors();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    };

    draw_workspace(WorkspaceMode::Tune, "Tune",
                   "Tune workspace.\n"
                   "Tables · Table · Stats · History · DTCs.");
    draw_workspace(WorkspaceMode::Datalog, "Log",
                   "Datalog workspace.\n"
                   "Knock Dashboard · Adaptive History · Cold-Start · EBCS.");
    draw_workspace(WorkspaceMode::Features, "Feat",
                   "Custom Features workspace.\n"
                   "Node-graph designer for rev limiters, FFS, etc.");

    // Push the Flash quick-action to the bottom of the rail. Spacer
    // soaks up the remaining vertical room; the button below it sits
    // just above the status bar gap.
    float const remaining =
        ImGui::GetContentRegionAvail().y - kBtnH - 8.0f;
    if (remaining > 0.0f) {
        ImGui::Dummy(ImVec2(0.0f, remaining));
    }
    // Visual divider above the Flash button so it reads as a separate
    // category from the workspace cluster above.
    {
        ImVec2 const p = ImGui::GetCursorScreenPos();
        auto *const dl = ImGui::GetWindowDrawList();
        ImU32 const col = ImGui::GetColorU32(ImGuiCol_Separator);
        dl->AddLine(ImVec2(p.x + 4.0f, p.y), ImVec2(p.x + kBtnW - 4.0f, p.y), col);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }
    // Flash quick-action — not a workspace, just a launcher for the
    // policy-gated flash modal. Disabled without a project; tooltip
    // explains why.
    ImGui::BeginDisabled(!state.project.has_value());
    if (ImGui::Button("Flash", ImVec2(kBtnW, kBtnH))) {
        state.show_flash_modal = true;
        state.flash_confirm_checked = false;
        state.flash_reason[0] = '\0';
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.project.has_value()) {
            ImGui::SetTooltip("Flash the working ROM to the ECU.\n"
                              "Opens the policy-gated flash modal.");
        } else {
            ImGui::SetTooltip("No project open — open one first (Ctrl+O).");
        }
    }
    ImGui::End();
}
// Per-workspace dock layouts. Each WorkspaceMode owns its own central-
// area composition and side-rail configuration; switching modes
// rebuilds the tree from scratch so each workspace lands in its
// canonical arrangement.
//
// Window-label invariants: DockBuilderDockWindow hashes the full
// string passed, so the labels must match each panel's Begin() string
// byte-for-byte — including the `###id` suffixes the Preview panels
// use to preserve ImGui IDs across the title cleanup.
void build_workspace_layout(WorkspaceMode const mode, ImGuiID const dockspace_id,
                            ImVec2 const node_size) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, node_size);

    switch (mode) {
    case WorkspaceMode::Tune: {
        // Tables left (22%) | Table central | Stats+History+DTCs right (25%).
        // Default tuning surface — what every other mode is compared to.
        ImGuiID left = 0;
        ImGuiID middle = 0;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.22f, &left, &middle);
        ImGuiID right = 0;
        ImGuiID center = 0;
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, 0.25f, &right, &center);
        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Table", center);
        ImGui::DockBuilderDockWindow("Stats", right);
        ImGui::DockBuilderDockWindow("History", right);
        ImGui::DockBuilderDockWindow("DTCs", right);
        break;
    }
    case WorkspaceMode::Datalog: {
        // Tables left narrower (16%) — datalog panels need width for
        // charts. Central area is a tabbed strip of the four datalog
        // panels (Knock / Adaptive / Cold-Start / EBCS); DTCs gets the
        // bottom strip for cross-referencing active codes against
        // observed signals.
        ImGuiID left = 0;
        ImGuiID middle = 0;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.16f, &left, &middle);
        ImGuiID bottom = 0;
        ImGuiID top = 0;
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Down, 0.22f, &bottom, &top);
        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Knock Dashboard###Knock Dashboard (Preview)", top);
        ImGui::DockBuilderDockWindow("Adaptive History###Adaptive History (Preview)", top);
        ImGui::DockBuilderDockWindow(
            "Cold-Start Analysis###Cold-Start Analysis (Preview)", top);
        ImGui::DockBuilderDockWindow("EBCS PID Assistant###EBCS PID Assistant (Preview)",
                                     top);
        ImGui::DockBuilderDockWindow("DTCs", bottom);
        break;
    }
    case WorkspaceMode::Features: {
        // Full-bleed canvas. The node graph needs every pixel; no
        // sidebar, no side rails. Features Designer takes the whole
        // dockspace as the central node. Other always-on windows
        // (Tables, Table) are flagged hidden via the mode-switch's
        // visibility flips — they're not docked here but the workspace
        // rail click also zeroed their show_*_panel flags.
        ImGui::DockBuilderDockWindow(
            "Custom Features Designer###Custom Features Designer (Preview)", dockspace_id);
        break;
    }
    }

    ImGui::DockBuilderFinish(dockspace_id);
}

// Look up the dockspace's central node — the tab strip that Table
// lives in. Panels call this before their Begin() to set
// SetNextWindowDockID(...FirstUseEver) so their first appearance is a
// tab in the central area instead of a free-floating window plopped
// at the top-left.
//
// Returns 0 if the dockspace hasn't been built yet this session
// (caller skips the SetNextWindowDockID call in that case — the
// first-run build below sets the layout up directly).
ImGuiID central_dock_node_id() {
    ImGuiID const id = ImGui::GetID("MainDockSpace");
    if (auto const *central = ImGui::DockBuilderGetCentralNode(id); central != nullptr) {
        return central->ID;
    }
    return 0;
}

// One transparent host window covering the work area (minus our manual status
// bar and the left workspace rail) hosts the dockspace.
// ImGuiDockNodeFlags_PassthruCentralNode keeps the empty central area
// transparent so the GL clear color shows through.
//
// Takes AppState so it can rebuild the dock tree according to the
// current workspace_mode when apply_workspace_mode() (or the View →
// Reset window layout command) fires a layout-reset request.
void render_dockspace_host(AppState &state) {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + kWorkspaceRailWidth, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - kWorkspaceRailWidth,
                                    vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    auto const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##dockspace_host", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID const id = ImGui::GetID("MainDockSpace");

    // Reset request from a workspace switch, the View menu, or the
    // command palette. Drops the saved tree so the rebuild branch
    // below fires and lands the canonical layout for the current mode.
    bool const requested_reset = g_request_dock_reset;
    if (requested_reset) {
        ImGui::DockBuilderRemoveNode(id);
        g_request_dock_reset = false;
    }

    // First ever run (no imgui.ini) OR a layout reset: build the dock
    // tree for the current workspace. Otherwise respect the user's
    // saved-and-customized layout from imgui.ini.
    if (ImGui::DockBuilderGetNode(id) == nullptr) {
        build_workspace_layout(state.workspace_mode, id, vp->WorkSize);
    }

    ImGui::DockSpace(id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

} // namespace st::ui
