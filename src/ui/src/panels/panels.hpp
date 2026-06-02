// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Forward-decl umbrella for every render_*_panel entry point plus the
// chrome (dockspace, menubar, sidebar, status bar) and the cross-cutting
// status / toast helpers. enqueue_toast is here (not in actions/) because
// it's the toast subsystem's entry point — callers go through it without
// needing to know the toast renderer's internals.
//
// Per-panel companion symbols (GridStats, compute_stats, heatmap_color,
// PaletteCommand, mirror_status_change, toast_lifetime_for, etc.) stay
// file-local to their respective .cpp once moved.

#ifndef ST_UI_PANELS_HPP
#define ST_UI_PANELS_HPP

#include "app_state.hpp"
#include "theme.hpp" // Fonts for render_table_view

#include <imgui.h>

#include <string>

namespace st::ui {

// Shared layout dimensions. Used by dockspace / workspace rail / status
// bar / toasts — multiple .cpps once split, so inline constexpr here.
inline constexpr float kStatusBarHeight = 26.0f;
inline constexpr float kWorkspaceRailWidth = 64.0f;

// Returns the dockspace's central node ID, or 0 if the tree hasn't been
// built yet. Datalog/features panels call this in their Begin() so
// their first appearance is a tab in the central area instead of a
// free-floating window plopped at top-left.
ImGuiID central_dock_node_id();

// Chrome.
void render_dockspace_host(AppState &state);
void render_menubar(AppState &state);
void render_sidebar(AppState &state);
void render_workspace_rail(AppState &state);

// Apply the current workspace_mode's dock-layout setup and request a
// layout rebuild on the next render_dockspace_host call. Also reused
// by render_menubar's "View → Reset window layout" item and the
// command palette's "Reset window layout" command.
void apply_workspace_mode(AppState &state, WorkspaceMode mode);
void request_layout_reset();

// Command palette (Ctrl+K). open_* primes the popup; render_* draws it.
void open_command_palette(AppState &state);
void render_command_palette(AppState &state);

// Panels.
void render_welcome_panel(AppState &state);
void render_stats_panel(AppState &state);
void render_knock_dashboard_panel(AppState &state);
void render_adaptive_history_panel(AppState &state);
void render_coldstart_panel(AppState &state);
void render_ebcs_panel(AppState &state);
void render_gauge_cluster_panel(AppState &state);
void render_dtcs_panel(AppState &state);
void render_history_panel(AppState &state);
void render_features_designer(AppState &state);
void render_table_view(AppState &state, Fonts const &fonts);

// Status bar + toasts. enqueue_toast is declared here because the toast
// stack is owned by the panels subsystem (renders bottom-right above the
// status bar); actions/ + modals/ are consumers.
void render_status_bar(AppState &state);
void render_toasts(AppState &state);
void enqueue_toast(AppState &state, ToastKind kind, std::string text);
void tick_status_msg(AppState &state);

} // namespace st::ui

#endif
