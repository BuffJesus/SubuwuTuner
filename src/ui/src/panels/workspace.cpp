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

#include "app_state.hpp"
#include "panels/panels.hpp"
#include "widgets/widgets.hpp"

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>
#include <optional>
#include <string>

namespace st::ui {

// Flag set by request_layout_reset() and consumed at the top of the
// next render_dockspace_host() invocation. Drives the "Reset window
// layout" command in View / Ctrl+K — drops the saved dock tree and
// rebuilds via build_workspace_layout below.
namespace {
bool g_request_dock_reset = false;
}
void request_layout_reset() {
    g_request_dock_reset = true;
}

// Apply a workspace preset: flip show_*_panel flags into a coherent set
// for the chosen mode, leave dock positions alone (imgui.ini owns
// those). User can still toggle individual panels afterwards — the
// workspace highlight in the rail just tracks the last preset applied.
void apply_workspace_mode(AppState &state, WorkspaceMode mode) {
    state.workspace_mode = mode;
    switch (mode) {
    case WorkspaceMode::Tune:
        state.show_tables_panel = true;
        state.show_table_view_panel = true;
        state.show_stats_panel = true;
        state.show_history_panel = true;
        state.show_dtcs_panel = true;
        state.show_knock_dashboard_panel = false;
        state.show_adaptive_history_panel = false;
        state.show_coldstart_panel = false;
        state.show_ebcs_panel = false;
        state.show_log_explorer_panel = false;
        state.show_features_designer = false;
        break;
    case WorkspaceMode::Datalog:
        // Tables + Table both hide — Datalog is the log-analysis
        // workspace, not the editor. The datalog quartet (Knock /
        // Adaptive / Cold-Start / EBCS) owns the full work area
        // with DTCs in a bottom strip for code cross-reference.
        state.show_tables_panel = false;
        state.show_table_view_panel = false;
        state.show_stats_panel = false;
        state.show_history_panel = false;
        // DTCs stays — datalog workflows frequently cross-reference
        // active codes against observed signals.
        state.show_dtcs_panel = true;
        state.show_knock_dashboard_panel = true;
        state.show_adaptive_history_panel = true;
        state.show_coldstart_panel = true;
        state.show_ebcs_panel = true;
        state.show_log_explorer_panel = true;
        state.show_features_designer = false;
        break;
    case WorkspaceMode::Features:
        // Full-bleed designer. Sidebar + Table editor + everything else
        // off so the canvas owns every pixel.
        state.show_tables_panel = false;
        state.show_table_view_panel = false;
        state.show_stats_panel = false;
        state.show_history_panel = false;
        state.show_dtcs_panel = false;
        state.show_knock_dashboard_panel = false;
        state.show_adaptive_history_panel = false;
        state.show_coldstart_panel = false;
        state.show_ebcs_panel = false;
        state.show_log_explorer_panel = false;
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
    // Defensive clamp at >= 1px even though main.cpp's
    // glfwSetWindowSizeLimits keeps the OS window comfortably above the
    // chrome budget. A negative height to SetNextWindowSize is UB.
    float const rail_h = std::max(1.0f, vp->WorkSize.y - kStatusBarHeight);
    ImGui::SetNextWindowSize(ImVec2(kWorkspaceRailWidth, rail_h));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 8.0f));

    ImGuiWindowFlags const flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##workspace_rail", nullptr, flags);
    ImGui::PopStyleVar(3);

    constexpr float kBtnW = 56.0f; // rail 64 minus 4px padding each side
    constexpr float kBtnH = 56.0f; // extra height for icon-over-caption stack
    constexpr float kStripeW = 3.0f;
    constexpr float kIconScale = 1.55f; // icon drawn larger than caption

    // Segoe MDL2 Assets / Fluent Icons codepoints (private-use area
    // U+E700–U+F8FF), loaded as a merged font in theme.cpp's load_fonts.
    // UTF-8 encoded inline so the literals are portable in source.
    // - E70F  Edit (pencil)       → Tune  (calibration editing)
    // - E9D9  LineChart           → Log   (datalog signals)
    // - E945  Lightning           → Feat  (custom features)
    char const *const kIconEdit = "\xEE\x9C\x8F";
    char const *const kIconChart = "\xEE\xA7\x99";
    char const *const kIconLightning = "\xEE\xA5\x85";

    auto const accent_base = accent_for(current_theme()).base;
    auto const draw_workspace = [&](WorkspaceMode mode, char const *icon, char const *caption,
                                    char const *tooltip) {
        bool const is_active = (state.workspace_mode == mode);
        ImGui::PushID(caption);
        ImVec2 const cursor_screen = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##ws", ImVec2(kBtnW, kBtnH))) {
            apply_workspace_mode(state, mode);
        }
        bool const hovered = ImGui::IsItemHovered();

        auto *const dl = ImGui::GetWindowDrawList();
        // Active-edge stripe — sits OUTSIDE the button on the rail's
        // left edge, vertically inset so the corners breathe. Previous
        // implementation used push_primary_button_colors() for the active
        // state, which collided with hover styling (active+hover landed
        // in the same shade so the user lost the "selected" signal under
        // the cursor). A 3px brand-purple stripe reads unambiguously.
        if (is_active) {
            float const x0 = cursor_screen.x - kStripeW - 1.0f;
            float const x1 = cursor_screen.x - 1.0f;
            float const y0 = cursor_screen.y + 6.0f;
            float const y1 = cursor_screen.y + kBtnH - 6.0f;
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::GetColorU32(accent_base),
                              kStripeW * 0.5f);
        }

        // Icon (top half, ~1.55× scale) + caption (bottom half, normal
        // scale). Caption stays so the rail self-documents — first-run
        // users don't have to recognize the MDL2 glyph to know what's
        // what. Active state colors both icon + caption with the
        // brand accent.
        ImFont *const font = ImGui::GetFont();
        float const base_size = ImGui::GetFontSize();
        float const icon_size = base_size * kIconScale;
        ImVec2 const icon_sz = font->CalcTextSizeA(icon_size, FLT_MAX, 0.0f, icon);
        ImVec2 const caption_sz = ImGui::CalcTextSize(caption);

        constexpr float kStackGap = 2.0f;
        float const stack_h = icon_sz.y + kStackGap + caption_sz.y;
        float const button_center_y = cursor_screen.y + kBtnH * 0.5f;
        float const icon_y = button_center_y - stack_h * 0.5f;
        float const caption_y = icon_y + icon_sz.y + kStackGap;
        float const button_center_x = cursor_screen.x + kBtnW * 0.5f;

        ImU32 const text_col =
            is_active ? ImGui::GetColorU32(accent_base) : ImGui::GetColorU32(ImGuiCol_Text);
        dl->AddText(font, icon_size, ImVec2(button_center_x - icon_sz.x * 0.5f, icon_y), text_col,
                    icon);
        dl->AddText(ImVec2(button_center_x - caption_sz.x * 0.5f, caption_y), text_col, caption);

        if (hovered) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    };

    draw_workspace(WorkspaceMode::Tune, kIconEdit, "Tune",
                   "Tune workspace.  (Ctrl+1)\n"
                   "Tables · Table · Stats · History · DTCs.");
    draw_workspace(WorkspaceMode::Datalog, kIconChart, "Log",
                   "Datalog workspace.  (Ctrl+2)\n"
                   "Knock Dashboard · Adaptive History · Cold-Start · EBCS.");
    draw_workspace(WorkspaceMode::Features, kIconLightning, "Feat",
                   "Custom Features workspace.  (Ctrl+3)\n"
                   "Node-graph designer for rev limiters, FFS, etc.");

    // Flash used to live at the bottom of the rail. Removed 2026-06-01
    // because the rail is a workspace switcher and Flash is a one-shot
    // (not a workspace); it lives on the table-view toolbar + File menu.
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
                            ImVec2 const node_size, bool const compact) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, node_size);

    switch (mode) {
    case WorkspaceMode::Tune: {
        if (compact) {
            // On narrow windows preserve a useful editor/welcome surface.
            // Tables gets a discoverable left strip; secondary inspection
            // panels become tabs beside Table instead of consuming a fixed
            // right rail that can squeeze the center down to nothing.
            ImGuiID left = 0;
            ImGuiID center = 0;
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.28f,
                                        &left, &center);
            ImGui::DockBuilderDockWindow("Tables", left);
            ImGui::DockBuilderDockWindow("Table", center);
            ImGui::DockBuilderDockWindow("Stats", center);
            ImGui::DockBuilderDockWindow("History", center);
            ImGui::DockBuilderDockWindow("DTCs", center);
            break;
        }
        // Tables left (18%) | Table central | right rail (22%) split
        // vertically: Stats on top (60%), History+DTCs as tabs on the
        // bottom (40%). Stats is the panel you actually read while
        // editing a map (histogram + min/mean/max) — gets the larger
        // share. History is glanceable when you want to retrace edits;
        // DTCs is "set once, ignore" — fine as a tucked tab next to
        // History. Old layout stuffed all three into one tab strip,
        // which hid Stats behind a click and wasted the vertical space
        // in the right rail.
        ImGuiID left = 0;
        ImGuiID middle = 0;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.18f, &left, &middle);
        ImGuiID right = 0;
        ImGuiID center = 0;
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, 0.22f, &right, &center);
        ImGuiID right_bottom = 0;
        ImGuiID right_top = 0;
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, &right_bottom, &right_top);
        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Table", center);
        ImGui::DockBuilderDockWindow("Stats", right_top);
        ImGui::DockBuilderDockWindow("History", right_bottom);
        ImGui::DockBuilderDockWindow("DTCs", right_bottom);
        break;
    }
    case WorkspaceMode::Datalog: {
        if (compact) {
            // A bottom DTC strip makes the main log controls unreachable at
            // laptop sizes. Keep every analysis surface available as a tab.
            ImGui::DockBuilderDockWindow("Log Explorer", dockspace_id);
            ImGui::DockBuilderDockWindow(
                "Knock Dashboard###Knock Dashboard (Preview)", dockspace_id);
            ImGui::DockBuilderDockWindow(
                "Adaptive History###Adaptive History (Preview)", dockspace_id);
            ImGui::DockBuilderDockWindow(
                "Cold-Start Analysis###Cold-Start Analysis (Preview)", dockspace_id);
            ImGui::DockBuilderDockWindow(
                "EBCS PID Assistant###EBCS PID Assistant (Preview)", dockspace_id);
            ImGui::DockBuilderDockWindow("DTCs", dockspace_id);
            break;
        }
        // Full-width datalog work area — Knock / Adaptive / Cold-Start
        // / EBCS as a tab strip in the central node, DTCs as a bottom
        // strip for active-code cross-reference against observed
        // signals. Tables + Table editor are suppressed at the panel
        // level for this mode (show_tables_panel /
        // show_table_view_panel both false). DTCs gets 25% (bumped from
        // 22%) so a 4-5 code list doesn't need scrolling on a default
        // window.
        ImGuiID bottom = 0;
        ImGuiID top = 0;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.25f, &bottom, &top);
        ImGui::DockBuilderDockWindow("Log Explorer", top);
        ImGui::DockBuilderDockWindow("Knock Dashboard###Knock Dashboard (Preview)", top);
        ImGui::DockBuilderDockWindow("Adaptive History###Adaptive History (Preview)", top);
        ImGui::DockBuilderDockWindow("Cold-Start Analysis###Cold-Start Analysis (Preview)", top);
        ImGui::DockBuilderDockWindow("EBCS PID Assistant###EBCS PID Assistant (Preview)", top);
        ImGui::DockBuilderDockWindow("DTCs", bottom);
        break;
    }
    case WorkspaceMode::Features: {
        // Full-bleed canvas. The node graph needs every pixel; no
        // sidebar, no side rails. Features Designer takes the whole
        // dockspace as the central node. Tables + Table windows are
        // suppressed at the panel level via show_tables_panel /
        // show_table_view_panel — apply_workspace_mode flips both
        // false for this mode.
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
    // Defensive clamp at >= 1px. main.cpp's glfwSetWindowSizeLimits keeps
    // the OS window above the chrome budget, but a host window with
    // negative dimensions is UB so guard regardless.
    float const dock_w = std::max(1.0f, vp->WorkSize.x - kWorkspaceRailWidth);
    float const dock_h = std::max(1.0f, vp->WorkSize.y - kStatusBarHeight);
    ImGui::SetNextWindowSize(ImVec2(dock_w, dock_h));
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
    bool const compact = dock_w < 900.0f;
    static std::optional<bool> prior_compact;

    // Reset request from a workspace switch, the View menu, or the
    // command palette. Drops the saved tree so the rebuild branch
    // below fires and lands the canonical layout for the current mode.
    bool const requested_reset = g_request_dock_reset;
    bool const compact_transition =
        prior_compact.has_value() && *prior_compact != compact;
    prior_compact = compact;
    if (requested_reset || compact_transition) {
        ImGui::DockBuilderRemoveNode(id);
        g_request_dock_reset = false;
    }

    // First ever run (no imgui.ini) OR a layout reset: build the dock
    // tree for the current workspace. Otherwise respect the user's
    // saved-and-customized layout from imgui.ini.
    if (ImGui::DockBuilderGetNode(id) == nullptr) {
        build_workspace_layout(state.workspace_mode, id, ImVec2(dock_w, dock_h), compact);
    }

    ImGui::DockSpace(id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

} // namespace st::ui
