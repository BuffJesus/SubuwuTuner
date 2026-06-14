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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
void render_compare_panel(AppState &state);
void render_audit_panel(AppState &state);
void render_dtcs_panel(AppState &state);
void render_history_panel(AppState &state);
void render_features_designer(AppState &state);
void render_ap3_browser_panel(AppState &state);
void render_library_panel(AppState &state);
[[nodiscard]] bool ap3_browser_should_hint(AppState const &state);

// Snapshot of the AP3 panel's currently-connected device, for the
// status bar to surface inline when the user has the browser open.
// Returns nullopt when no AP is connected or no query_state has
// succeeded yet. Populated from the RE8b CORRECTED status probes
// (cmd 0x2e / 0x30 / 0x31) plus the cmd 0x28 vehicle_descriptor.
struct EtsStatusSnapshot {
    std::string vehicle_descriptor;   // cmd 0x28 — "2017 USDM WRX MT ..."
    std::string vehicle_id;           // cmd 0x28 field[5] — "SUBA_US_WRXM_CF_17_F"
    std::string hardware_type;        // cmd 0x2e — "AP-V3" / etc.
    std::string vehicle_manufacturer; // cmd 0x30 — "Subaru"
    std::string ap_manufacturer;      // cmd 0x31 — "COBB Tuning"
    bool vehicle_paired{false};              // Installed vs Not Installed
    bool paired_known{false};       // distinguishes nullopt from false
    // /backupcksum contents (MD5 of the plaintext stock ROM per J3 RE)
    // when the Device tab has cached it. Lowercase hex, 32 chars.
    // Empty / nullopt when not yet fetched. Library panel cross-refs
    // this against TuneIndex::lookup_by_md5 to flag the currently-
    // flashed entry.
    std::optional<std::string> device_backupcksum;
    // Basenames of the .ptm files the AP exposes under /maps/. Empty
    // when the /maps/ tab hasn't been visited (or replug cleared the
    // cache). Library panel uses set-membership to flag rows as
    // "on AP".
    std::vector<std::string> ap_maps_filenames;
};
[[nodiscard]] std::optional<EtsStatusSnapshot> ets_status_snapshot();

// True when the ETS browser panel has an open USB channel, i.e. a Push
// originating outside the panel can ride the panel's worker queue
// instead of opening a second channel. The panel guarantees only one
// channel is open per process; the Save & Push wizard uses this guard
// to either route through the panel or surface a "connect first" hint.
[[nodiscard]] bool ets_panel_has_channel();

// Push `bytes` to the connected AP at `/maps/<filename>`. Blocks until
// the cmd 0x22 setup + 0x23 data + 0x05 NAND barrier complete; intended
// to be called from a worker thread (Save & Push modal does this).
// Returns the absolute on-AP path on success; returns the error string
// on failure. Refreshes the `/maps/` listing in the panel after a
// successful push so the new entry is immediately visible if the user
// flips to the AP browser.
//
// Workflow-gated: when ST_HAVE_AP_WORKFLOW isn't defined this returns
// the workflow-off error string without touching the wire.
struct EtsPushResult {
    bool ok{false};
    std::string ap_path; // absolute "/maps/<filename>" on ok
    std::string error;   // populated when !ok
};
[[nodiscard]] EtsPushResult ets_panel_push_to_maps(
    std::string filename, std::vector<std::uint8_t> bytes);

// Synchronous read of an arbitrary AP file via the panel's open
// channel. Returns the raw bytes on success; on failure returns an
// empty vector and writes the error string into `err`. Caller must
// have verified ets_panel_has_channel(). Workflow-gated: returns
// empty + an error when ST_HAVE_AP_WORKFLOW is undefined.
[[nodiscard]] std::vector<std::uint8_t>
ets_panel_read_file_sync(std::string_view ap_path, std::string &err);

// Library-panel snapshot for the status bar. Mirrors the AP status
// snapshot pattern: file-static inside library.cpp; returns nullopt
// when no index has been loaded so the status bar can degrade
// gracefully. Used by the bottom-bar Library chip + (future)
// keyboard-shortcut Ctrl+L jump-to-Library.
struct LibraryStatusSnapshot {
    std::size_t ptm_count{0};
    std::size_t stune_count{0};
    // Lowercase hex MD5 of the row that matches the AP's
    // /backupcksum if known. Empty when no match / no AP / no
    // index. The fields below render directly in the AP browser's
    // device header so it doesn't have to re-query the index.
    std::string currently_flashed_md5;
    std::string currently_flashed_name;    // basename of the matched .ptm
    std::string currently_flashed_vendor;  // e.g. "COBB", "Fehr"
    std::string currently_flashed_stage;   // e.g. "Stage1", "Stage0"
    std::string currently_flashed_variant; // e.g. "v401", "WRK3", "BigSF"
    std::string currently_flashed_fuel;    // e.g. "91", "93"
};
[[nodiscard]] std::optional<LibraryStatusSnapshot> library_status_snapshot();

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
