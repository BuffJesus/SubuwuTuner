// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Top main menu bar — File / Edit / View / Tools / Help. Routes
// gesture intents to actions.hpp + project_io.hpp + the per-modal
// "show this" flags on AppState. View → Reset window layout calls
// into request_layout_reset() (declared in panels.hpp, defined
// alongside the dockspace).

#include "st/core/version.hpp"
#include "st/policy.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "modals/modals.hpp" // pack_supports_fa24_swap
#include "panels/panels.hpp"
#include "persistence.hpp"
#include "project_io.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include <cstddef>
#include <cstdio>
#include <imgui.h>
#include <string>

namespace st::ui {

void render_menubar(AppState &state) {
    bool const has_project = state.project.has_value();
    // Edits route through the active slot (Issue #10 phase 3). Each
    // ROM has its own per-ROM history; source is the only slot
    // where editing_allowed is false.
    bool const editing_allowed = has_project && state.project->active_rom_mut() != nullptr;
    bool const can_undo =
        has_project && editing_allowed && state.project->active_history().can_undo();
    bool const can_redo =
        has_project && editing_allowed && state.project->active_history().can_redo();

    // Tooltip on a menu item even when it's disabled — so the user
    // understands WHY it's grayed out rather than just seeing the
    // affordance and wondering. AllowWhenDisabled is the hover flag
    // that makes IsItemHovered fire on a disabled item.
    auto const disabled_tip = [](char const *body) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", body);
        }
    };

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // MDL2 icon prefixes — same idiom as the table-view toolbar.
            //   E710 Add    E197 OpenFile    E74E Save    E711 Cancel
            //   E11B OpenFile (CSV)          E7E8 ChromeClose (Quit)
            if (ImGui::MenuItem("\xEE\x9C\x90  New Project\xE2\x80\xA6")) {
                request_action(state, ConfirmAction::NewProject);
            }
            if (ImGui::MenuItem("\xEE\x86\x97  Open Project\xE2\x80\xA6", "Ctrl+O")) {
                request_action(state, ConfirmAction::OpenDialog);
            }
            if (ImGui::MenuItem("\xEE\x9D\x8E  Save Project", "Ctrl+S", false, has_project)) {
                save_project(state);
            }
            if (!has_project) {
                disabled_tip("No project open — there's nothing to save.\n"
                             "Open a project first (Ctrl+O).");
            }
            if (ImGui::MenuItem("\xEE\x9C\x91  Close Project", nullptr, false, has_project)) {
                request_action(state, ConfirmAction::Close);
            }
            if (!has_project) {
                disabled_tip("No project open.");
            }
            ImGui::Separator();
            // PTM import wizard — decodes a COBB AccessPort .ptm tune
            // into a new SubuwuTuner project. Cipher-gated; the modal
            // surfaces a clear PolicyDenied message when the build
            // doesn't have ST_ENABLE_COBB_AP_CIPHER=ON.
            if (ImGui::MenuItem("\xEE\x86\x97  Import .ptm File\xE2\x80\xA6")) {
                state.show_ptm_import_modal = true;
            }
            if (ImGui::MenuItem("\xEE\xA0\x84  Inspect .ptm File\xE2\x80\xA6")) {
                state.show_ptm_inspect_modal = true;
            }
            if (ImGui::MenuItem("\xEE\x9D\x8E  Export as .ptm\xE2\x80\xA6", nullptr, false,
                                has_project)) {
                state.show_ptm_export_modal = true;
            }
            if (!has_project) {
                disabled_tip("Open a project first. The export modal needs a project\n"
                             "with [ptm_metadata] + ptm_patches.toml (from `ptm import`).");
            }
#ifdef ST_HAVE_AP_WORKFLOW
            // T31: "Save & Push to AP" wizard. Builds the .ptm and
            // streams it to the connected AP's /maps/ folder in one
            // shot. Gated on the workflow flag; the entry is omitted
            // entirely in the default-OFF distribution build.
            if (ImGui::MenuItem("\xEE\xA2\x8C  Save & Push to AP\xE2\x80\xA6", nullptr, false,
                                has_project)) {
                state.show_ptm_save_and_push_modal = true;
            }
            if (!has_project) {
                disabled_tip("Open a project first. Save & Push needs a project\n"
                             "with [ptm_metadata] + ptm_patches.toml.");
            }
#endif
            if (ImGui::MenuItem("\xEE\xA0\x84  Diff Two .ptm Files\xE2\x80\xA6")) {
                state.show_ptm_diff_modal = true;
            }
            ImGui::Separator();
            // CSV import/export — same `# pack_id` / `# table` /
            // `row,col,value` format as project-export-csv / -edit-csv,
            // and same parser, so the two surfaces round-trip with each
            // other.
            bool const can_csv = has_project && !state.selected_table_id.empty();
            if (ImGui::MenuItem("\xEE\x84\x9B  Import CSV into Table\xE2\x80\xA6", nullptr, false,
                                can_csv)) {
                import_csv_into_current_table_dialog(state);
            }
            if (!can_csv) {
                disabled_tip("Select a table first.\n"
                             "Imports a row,col,value CSV as a single bulk edit\n"
                             "(undoable via Ctrl+Z).");
            }
            if (ImGui::MenuItem("\xEE\x9D\xA8  Export Table as CSV\xE2\x80\xA6", nullptr, false,
                                can_csv)) {
                export_current_table_csv_dialog(state, /*diff_only=*/false);
            }
            if (!can_csv) {
                disabled_tip("Select a table first.");
            }
            if (ImGui::MenuItem("\xEE\x9D\xA8  Export Table Edits as CSV\xE2\x80\xA6", nullptr,
                                false, can_csv)) {
                export_current_table_csv_dialog(state, /*diff_only=*/true);
            }
            if (!can_csv) {
                disabled_tip("Select a table first.\n"
                             "Exports only the cells you've changed from "
                             "the source ROM — a compact, share-able tune diff.");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("\xEE\x9F\xA8  Quit", "Ctrl+Q")) {
                request_action(state, ConfirmAction::Quit);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit", has_project)) {
            // E7A7 Undo  E7A6 Redo  E8C8 Copy  E77F Paste  E72C Refresh
            if (ImGui::MenuItem("\xEE\x9E\xA7  Undo", "Ctrl+Z", false, can_undo)) {
                do_undo(state);
            }
            if (has_project && !can_undo) {
                disabled_tip(editing_allowed ? "Nothing to undo — no edits have been made\n"
                                               "to the active ROM."
                                             : "Source ROM is read-only. Switch View →\n"
                                               "Active ROM to an editable slot first.");
            }
            if (ImGui::MenuItem("\xEE\x9E\xA6  Redo", "Ctrl+Shift+Z", false, can_redo)) {
                do_redo(state);
            }
            if (has_project && !can_redo) {
                disabled_tip(editing_allowed ? "Nothing to redo.\n"
                                               "Use Undo first, then Redo to step forward."
                                             : "Source ROM is read-only. Switch View →\n"
                                               "Active ROM to an editable slot first.");
            }
            ImGui::Separator();
            bool const has_selection = state.selection.enabled;
            // Copy is read-only — works against any ROM view. Paste +
            // Reset mutate working_rom and gate on editing_allowed.
            if (ImGui::MenuItem("\xEE\xA3\x88  Copy", "Ctrl+C", false, has_selection)) {
                do_copy_selection(state);
            }
            if (has_project && !has_selection) {
                disabled_tip("Select cells in the grid first.");
            }
            bool const can_paste = has_selection && editing_allowed;
            if (ImGui::MenuItem("\xEE\x9D\xBF  Paste", "Ctrl+V", false, can_paste)) {
                paste_clipboard_at_cursor(state);
            }
            if (has_project && !can_paste) {
                disabled_tip(editing_allowed ? "Select a target cell, then paste TSV from the\n"
                                               "clipboard at the cursor."
                                             : "Source ROM is read-only. Switch View →\n"
                                               "Active ROM to an editable slot first.");
            }
            ImGui::Separator();
            bool const can_reset = has_selection && editing_allowed;
            if (ImGui::MenuItem("\xEE\x9C\xAC  Reset to Source", nullptr, false, can_reset)) {
                reset_selection_to_source(state);
            }
            if (has_project && !can_reset) {
                disabled_tip(editing_allowed ? "Reverts the selected cells to their source-ROM\n"
                                               "values (undoable)."
                                             : "Source ROM is read-only. Switch View →\n"
                                               "Active ROM to an editable slot first.");
            }
            ImGui::Separator();
            // Auto-Tune submenu. Groups the kernel-driven proposal flows so
            // future additions (LTFT, cold-start, etc.) don't sprawl the
            // Edit menu. Disabled-with-tooltip mirrors the rest of Edit.
            // Auto-tune kernels apply their proposals to the active
            // slot (Issue #10 phase 3 — autotune_maf.cpp,
            // autotune_knock.cpp). Gate the submenu off only when the
            // active slot is read-only (source); additional ROMs are
            // editable now with their own per-ROM history.
            if (ImGui::BeginMenu("\xEE\xA5\x90  Auto-Tune", has_project && editing_allowed)) {
                if (ImGui::MenuItem("MAF\xE2\x80\xA6")) {
                    // Default the target table to whatever the user has open
                    // — saves a step when they're already looking at the MAF
                    // scaling. They can still type a different id.
                    std::snprintf(state.maf_at_table_id, sizeof state.maf_at_table_id, "%s",
                                  state.selected_table_id.c_str());
                    state.maf_at_status_msg.clear();
                    state.maf_at_result.reset();
                    state.maf_at_lints.clear();
                    state.maf_at_table_data.reset();
                    state.show_maf_autotune_modal = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Runs the docs/12 MAF auto-tune kernel against\n"
                                      "a CSV datalog and applies the proposal as a\n"
                                      "single undoable edit.");
                }
                if (ImGui::MenuItem("Knock Pull\xE2\x80\xA6")) {
                    std::snprintf(state.kp_at_table_id, sizeof state.kp_at_table_id, "%s",
                                  state.selected_table_id.c_str());
                    state.kp_at_status_msg.clear();
                    state.kp_at_result.reset();
                    state.kp_at_lints.clear();
                    state.kp_at_table_data.reset();
                    state.kp_at_rpm_axis_values.clear();
                    state.kp_at_load_axis_values.clear();
                    state.show_kp_autotune_modal = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Runs the docs/12 knock-based ignition pull\n"
                                      "kernel against a CSV datalog and applies the\n"
                                      "2D proposal as a single undoable edit.");
                }
                ImGui::EndMenu();
            }
            if (has_project && !editing_allowed) {
                disabled_tip("Source ROM is read-only. Switch View → Active\n"
                             "ROM to an editable slot first.");
            }
            if (!has_project) {
                disabled_tip("Open a project first.\n"
                             "Kernels run against a CSV datalog and apply\n"
                             "their proposal as a single undoable edit.");
            }
            ImGui::EndMenu();
        }
        if (!has_project && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            // Edit menu itself is disabled (BeginMenu second arg = false).
            ImGui::SetTooltip("No project open — open one to enable editing.");
        }
        if (ImGui::BeginMenu("Tools")) {
            // E896 Download  E8B7 Folder  E713 Settings/gear
            if (ImGui::MenuItem("\xEE\xA2\x96  Read ROM from Car\xE2\x80\xA6")) {
                // Open the modal in Idle state. Leftover bytes_result from
                // a previous successful run get cleared so the modal opens
                // on the form, not on the post-read save dialog.
                state.read_rom_state = AppState::ReadRomState::Idle;
                state.read_rom_error_msg.clear();
                state.read_rom_bytes_result.clear();
                state.show_read_rom_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Dump the ECU's current calibration via the connected\n"
                                  "USB adapter (OBDX / J2534 / Native). Read-only — no\n"
                                  "ECU writes. Saves to a .bin you can then open as a\n"
                                  "new project via File \xE2\x86\x92 New Project\xE2\x80\xA6");
            }
            // Future: "Write ROM to Car..." (see plan comment above
            // render_read_rom_modal). Wired after OBDX adapter validation
            // + battery / ignition preflight checks land.
            ImGui::Separator();
            if (ImGui::MenuItem("\xEE\xA2\xB7  Browse Definitions\xE2\x80\xA6")) {
                state.show_def_registry_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Walk a definitions directory via PackRegistry and\n"
                                  "list every pack found (loose + zipped). Useful\n"
                                  "for verifying a ship-time install layout where\n"
                                  "<platform>.zip archives replace loose .toml files.");
            }
            if (ImGui::MenuItem("\xEE\xA0\x80  Datalog Channel Catalog\xE2\x80\xA6", nullptr, false,
                                has_project)) {
                state.show_datalog_channels_modal = true;
            }
            if (!has_project) {
                disabled_tip("Open a project first — the catalog lists every SSM\n"
                             "PID + RAM switch from the project's definition pack.");
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Every loggable SSM PID + RAM switch in the loaded\n"
                                  "definition pack. Filterable search; flags the\n"
                                  "default-log channels.");
            }
            if (ImGui::MenuItem("\xEE\xA0\x84  Log Explorer")) {
                apply_workspace_mode(state, WorkspaceMode::Datalog);
                state.show_log_explorer_panel = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open the docked Log workspace, import a CSV datalog,\n"
                                  "search signals, apply workflow profiles, and compare\n"
                                  "selected channels against time.");
            }
#ifdef ST_HAVE_AP_WORKFLOW
            if (ImGui::MenuItem("\xEE\xA0\x84  Pull File from AP\xE2\x80\xA6")) {
                state.show_pull_file_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("One-off file pull from the connected AccessPort —\n"
                                  "for paths outside the canonical /maps + /datalog +\n"
                                  "/presets + /images surface.");
            }
#endif
            if (ImGui::MenuItem("\xEE\x9C\x93  Settings\xE2\x80\xA6")) {
                state.show_settings_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Edit the runtime config that PackRegistry and\n"
                                  "rom-pull consult for default paths. Lives at\n"
                                  "%%APPDATA%%\\SubuwuTuner\\config.toml. Equivalent\n"
                                  "to running `subuwutuner-cli config set` from a\n"
                                  "shell. See docs/25 for precedence rules.");
            }
            if (ImGui::MenuItem("\xEE\xA2\xB0  Run Diagnostics\xE2\x80\xA6")) {
                state.show_doctor_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Install-health triage: build identity, cipher state,\n"
                                  "config-file load, definitions root + pack count,\n"
                                  "AccessPort detection. Same checks as the CLI's\n"
                                  "`subuwutuner-cli doctor`, surfaced visually.\n"
                                  "The 'start here' modal when something isn't working.");
            }
            // Common-workflows submenu — discovery-and-action surface
            // for opinionated multi-table recipes. Each entry routes to
            // a dedicated modal; the enabled-state gates on the loaded
            // pack declaring the workflow's required_tables. Today only
            // FA24 swap is wired; future entries (Stage1→2 step, E85
            // conversion, BRZ stroker) follow the same shape.
            ImGui::Separator();
            if (ImGui::BeginMenu("\xEE\xA2\xA8  Common Workflows")) {
                bool const fa24_ok = pack_supports_fa24_swap(state);
                if (ImGui::MenuItem("FA24 swap (VA WRX)\xE2\x80\xA6", nullptr, false, fa24_ok)) {
                    state.show_fa24_swap_modal = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (fa24_ok) {
                        ImGui::SetTooltip("Guided 3-step recipe for the FA20→FA24 engine swap.\n"
                                          "Applies Engine Displacement + HPFP-Timing + AVCS-\n"
                                          "Reference + Injector-Mult edits atomically. Reversible\n"
                                          "via the status-bar badge after Apply.");
                    } else if (!state.project.has_value()) {
                        ImGui::SetTooltip("Open a project first. The workflow needs a loaded\n"
                                          "calibration pack to know which tables to edit.");
                    } else {
                        ImGui::SetTooltip("This pack doesn't declare FA24-swap support — it's\n"
                                          "missing one or more of: HPFP Base Offset, AVCS\n"
                                          "Intake Cam Target (Closed, Baro Low/High), Injector\n"
                                          "Mult Table, Engine Displacement. Pick a project\n"
                                          "based on LF79101P / LF79103P / LF9L000E to enable.");
                    }
                }
                bool const tgv_egr_ok = pack_supports_tgv_egr_delete(state);
                if (ImGui::MenuItem("TGV + EGR Delete (off-road only)\xE2\x80\xA6", nullptr, false,
                                    tgv_egr_ok)) {
                    state.show_tgv_egr_delete_modal = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (tgv_egr_ok) {
                        ImGui::SetTooltip("Guided 3-step delete: zeros 5 EGR + TGV cal tables\n"
                                          "and disables the P0400 DTC. Per analyst Tasks A-F\n"
                                          "(2026-06-09). Jurisdiction-gated; not for daily-\n"
                                          "drivers on public roads. Reversible via the\n"
                                          "status-bar badge after Apply.");
                    } else if (!state.project.has_value()) {
                        ImGui::SetTooltip("Open a project first. The workflow needs a loaded\n"
                                          "calibration pack to know which tables to edit.");
                    } else {
                        ImGui::SetTooltip("This pack doesn't declare TGV+EGR-delete support —\n"
                                          "missing one or more of: EGR Airflow, EGR Absolute\n"
                                          "Pressure Main, TGV Closed Activation, Ignition\n"
                                          "Timing EGR Adders A/C. Currently supported on\n"
                                          "LF79103P (+ inherited via LF79101P).");
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            bool const is_grid = state.view_mode == TableViewMode::Grid;
            bool const is_heat = state.view_mode == TableViewMode::Heatmap;
            if (ImGui::MenuItem("Grid", nullptr, is_grid, has_project)) {
                state.view_mode = TableViewMode::Grid;
            }
            if (ImGui::MenuItem("Heatmap", nullptr, is_heat, has_project)) {
                state.view_mode = TableViewMode::Heatmap;
            }
            ImGui::Separator();
            // Active ROM picker (Issue #10 read-side foundation). Lists
            // the built-in working + source slots plus every additional
            // ROM in the project. Selecting an entry persists via
            // Project::set_active_rom_id + save_metadata, mirrors the
            // value into AppState, and re-reads the current table from
            // the new view ROM so the grid updates in place. Editing
            // is gated to working — see table_view.cpp's editing_allowed.
            if (ImGui::BeginMenu("Active ROM", has_project)) {
                bool const is_working = state.viewing_working_rom();
                bool const is_source = state.active_rom_id == "source";
                if (ImGui::MenuItem("Working", nullptr, is_working) && !is_working) {
                    set_active_view_rom(state, "working");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("The project's primary editable slot.");
                }
                if (ImGui::MenuItem("Source (read-only)", nullptr, is_source) && !is_source) {
                    set_active_view_rom(state, "source");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("The immutable source ROM captured at\n"
                                      "project creation. Edit affordances\n"
                                      "disable while this slot is active.");
                }
                ImGui::Separator();
                if (state.project.has_value() && !state.project->additional_roms().empty()) {
                    for (auto const &r : state.project->additional_roms()) {
                        bool const is_active = state.active_rom_id == r.id;
                        std::string const label = r.display_name.empty() ? r.id : r.display_name;
                        if (ImGui::MenuItem(label.c_str(), nullptr, is_active) && !is_active) {
                            set_active_view_rom(state, r.id);
                        }
                    }
                } else {
                    // Empty-state row so the user sees the submenu's
                    // structure (Working / Source / + extras) even
                    // when no extra ROMs have been imported yet.
                    // Without this the separator above looks like the
                    // menu got cut off mid-list.
                    ImGui::MenuItem("(no additional ROMs imported)", nullptr, false, false);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip(
                            "Add ROMs via Tools \xE2\x86\x92 Read ROM from Car\xE2\x80\xA6\n"
                            "(or copy a .bin into the project dir and reopen).");
                    }
                }
                ImGui::EndMenu();
            }
            if (!has_project) {
                disabled_tip("Open a project to choose which ROM the grid reads.");
            }
            ImGui::Separator();
            // Panel visibility. Sidebar + Table are always-on (primary
            // navigation); Stats and DTCs are secondary panels the user
            // may want hidden when working in the table grid full-screen.
            ImGui::MenuItem("Project Readiness", nullptr, &state.show_readiness_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Offline-first project cockpit: identity, definition, changes, "
                                  "recovery evidence, and the next useful action.");
            }
            ImGui::MenuItem("Stats Panel", nullptr, &state.show_stats_panel);
            ImGui::MenuItem("Log Explorer", nullptr, &state.show_log_explorer_panel);
            ImGui::MenuItem("DTCs Panel", nullptr, &state.show_dtcs_panel);
            ImGui::MenuItem("Tune Library", nullptr, &state.show_library_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Browse the user's local .ptm tune library. Reads\n"
                                  "library_index.toml — produced by\n"
                                  "tools/library_inventory/inventory.py. Vendor / stage /\n"
                                  "variant facets; future cross-references against the\n"
                                  "AccessPort's /backupcksum.");
            }
#ifdef ST_HAVE_AP_WORKFLOW
            ImGui::MenuItem("AccessPort Browser", nullptr, &state.show_ap3_browser_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Manage tunes / datalogs / presets on a COBB AccessPort V3\n"
                                  "over USB. Capability A (file vault) only — the .ptm patch\n"
                                  "introspection tier ships off by default. See docs/34.");
            }
#endif
            ImGui::MenuItem("History Panel", nullptr, &state.show_history_panel);
            ImGui::MenuItem("Knock Dashboard", nullptr, &state.show_knock_dashboard_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Per-cylinder knock dashboard. Load a CSV datalog,\n"
                                  "map RPM / load / per-cyl FLKC + FBKC columns,\n"
                                  "compute a windowed snapshot, view strip charts.\n"
                                  "See docs/05-improvements.md §11.");
            }
            ImGui::MenuItem("Adaptive History", nullptr, &state.show_adaptive_history_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Long-cycle LTFT / DAM / idle-adapt drift charts.\n"
                                  "Load a CSV with timestamps + per-signal columns,\n"
                                  "bucket by time (day default), view drift slope.\n"
                                  "See docs/05-improvements.md §11.");
            }
            ImGui::MenuItem("Cold-Start Analysis", nullptr, &state.show_coldstart_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Phase-classify + ECT-bin a cold-start datalog.\n"
                                  "Compares observed lambda to a user-defined target\n"
                                  "curve at each ECT bucket. See docs/05 §11.");
            }
            ImGui::MenuItem("EBCS PID Assistant", nullptr, &state.show_ebcs_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Detect tip-in events in a boost log; characterize each\n"
                                  "step response (rise / overshoot / settling); produce\n"
                                  "heuristic Kp/Ki/Kd gain-adjustment suggestions.\n"
                                  "Advisory only — verify on a dyno. See docs/05 §11.");
            }
            ImGui::MenuItem("Gauge Cluster (live)", nullptr, &state.show_gauge_cluster_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Live gauge cluster with headline values + ImPlot\n"
                                  "mini-line history per channel. v1 ships with a\n"
                                  "synthetic demo data feed so the panel works without\n"
                                  "a connected ECU; real-transport hookup lands as a\n"
                                  "follow-up. See docs/32-live-datalogger.md.");
            }
            ImGui::MenuItem("Compare ROMs", nullptr, &state.show_compare_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Two-pane structured ROM diff. Picks per-table\n"
                                  "cell-level changes against the open project's\n"
                                  "pack. Click 'Open in editor' on a changed table\n"
                                  "to jump to it in the Table viewer. Same engine\n"
                                  "as the `subuwutuner-cli diff` subcommand.\n"
                                  "See docs/33 + analyst Issue #4.");
            }
            ImGui::MenuItem("Audit log", nullptr, &state.show_audit_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Read-only viewer for <project>/audit.log.\n"
                                  "Surfaces every recorded ECU operation with\n"
                                  "per-entry CRC32 verification — bad checksums\n"
                                  "render as red 'BAD' chips so tampering is\n"
                                  "visible instead of silently dropped.\n"
                                  "Toggle scope to 'Per-vehicle history' inside\n"
                                  "the panel to pull the cross-project history\n"
                                  "for the active VehicleProfile's CID.\n"
                                  "See analyst Issue #8.");
            }
            // Quick-launch into the per-vehicle scope, mirrored under
            // both View (Audit log) and the help discoverability path.
            // Single sink ultimately the same panel — but a dedicated
            // entry surfaces the new scope without forcing the user to
            // open the panel and discover the dropdown.
            if (ImGui::MenuItem("Audit (per-vehicle history)", nullptr, nullptr,
                                state.settings.active_vehicle_profile_id.empty() ? false : true)) {
                state.audit_scope = AppState::AuditScope::Vehicle;
                state.audit_pinned_keys.clear();
                state.audit_show_pinned_only = false;
                state.audit_loaded = false;
                state.audit_error_msg.clear();
                state.show_audit_panel = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Open the Audit panel pre-scoped to per-vehicle\n"
                                  "history — aggregates every project's appends for\n"
                                  "this car's CID. Needs an active VehicleProfile.");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Theme")) {
                bool const is_dark = state.settings.theme == Theme::Dark;
                bool const is_light = state.settings.theme == Theme::Light;
                if (ImGui::MenuItem("Dark", nullptr, is_dark) && !is_dark) {
                    state.settings.theme = Theme::Dark;
                    apply_theme(Theme::Dark);
                    save_settings(state.settings);
                }
                if (ImGui::MenuItem("Light", nullptr, is_light) && !is_light) {
                    state.settings.theme = Theme::Light;
                    apply_theme(Theme::Light);
                    save_settings(state.settings);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("\xEE\x9C\xAC  Reset window layout")) {
                request_layout_reset();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Discard the current dock arrangement and rebuild the\n"
                                  "default layout (Tables left, central tab strip with\n"
                                  "Table + datalog panels, Stats/History/DTCs right).\n"
                                  "Useful if a panel got stuck floating off-screen.");
            }
            ImGui::Separator();
            // Custom features designer — Phase 5 user-facing surface.
            // Promoted out of Debug to top-level so users actually
            // discover it; the audit flagged that hiding it next to
            // the ImGui demo made it read as a developer escape hatch.
            ImGui::MenuItem("Custom Features Designer", nullptr, &state.show_features_designer);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Phase 5 node-graph editor for authoring custom\n"
                                  "features (rev limiters, flat-foot shift, etc.).\n"
                                  "Graph + IR + SH-2A codegen are implemented;\n"
                                  "wire-up to flash lands in Phase 5 patch insertion.");
            }
            ImGui::Separator();
            // Dev-only escape hatch. Tucked one level deeper so the
            // ImGui example isn't a peer of the user-facing view modes.
            if (ImGui::BeginMenu("Debug")) {
                ImGui::MenuItem("ImGui demo window", nullptr, &state.show_imgui_demo);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("SubuwuTuner %.*s", static_cast<int>(st::Version::string().size()),
                        st::Version::string().data());
            ImGui::Separator();
            // E721 Search  E92E Keyboard  E946 Info
            if (ImGui::MenuItem("\xEE\x9C\xA1  Command Palette\xE2\x80\xA6", "Ctrl+K")) {
                open_command_palette(state);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Search every menu action, panel toggle, recent\n"
                                  "project, and table in the loaded pack from one\n"
                                  "input. Highest-leverage shortcut in the app.");
            }
            if (ImGui::MenuItem("\xEE\xA4\xAE  Keyboard Shortcuts\xE2\x80\xA6")) {
                state.show_shortcuts_modal = true;
            }
            // E946 Info — the in-app help-topic browser. Reads project
            // docs/ markdown at runtime (resolve_docs_dir) so the same
            // content drives both the docs/ tree and the app.
            if (ImGui::MenuItem("\xEE\xA5\x86  Topics & Glossary\xE2\x80\xA6")) {
                state.show_help_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Browse project documentation (overview,\n"
                                  "glossary, roadmap) without leaving the app.\n"
                                  "Reads from docs/ alongside the binary.\n"
                                  "Analyst Issues #12 + #22.");
            }
            // E700 GlobalNavButton — reuse as a "guided welcome"
            // affordance until a more on-brand icon is needed.
            if (ImGui::MenuItem("\xEE\x9C\xA0  Welcome wizard\xE2\x80\xA6")) {
                state.show_first_run_wizard = true;
                state.first_run_step = 0;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Re-run the first-launch wizard (jurisdiction,\n"
                                  "units, theme, demo project). Settings update\n"
                                  "on Finish; Skip leaves current settings alone.");
            }
            if (ImGui::MenuItem("\xEE\xA5\x86  About SubuwuTuner\xE2\x80\xA6")) {
                state.show_about_modal = true;
            }
            ImGui::Separator();
            text_subtle("Getting started");
            ImGui::BulletText(
                "File \xE2\x86\x92 Open Project\xE2\x80\xA6 (Ctrl+O) to pick a .stune directory.");
            ImGui::BulletText("Or pass one on the command line: subuwutuner-gui my.stune");
            ImGui::BulletText("No project of your own? Open the bundled fixtures/demo.stune/ to "
                              "explore the UI.");
            ImGui::Separator();
            text_subtle("Editing");
            ImGui::BulletText("Click cells to select; Shift-click to extend.");
            ImGui::BulletText("Arrow keys move the cursor; Shift+arrows extend.");
            ImGui::BulletText(
                "F2 or double-click a cell to type a new value.  Enter commits, Esc cancels.");
            ImGui::BulletText(
                "Ctrl+Enter while editing fills every selected cell with the typed value.");
            ImGui::BulletText(
                "Ctrl+C / Ctrl+V copy and paste the selection as tab-separated values.");
            ImGui::BulletText("Right-click any cell for Copy / Paste / Reset to Source.");
            ImGui::BulletText(
                "Toolbar buttons (+5%%, -5%%, Smooth, Interpolate) act on the selection.");
            ImGui::BulletText("Ctrl+Z / Ctrl+Shift+Z to undo / redo.  Ctrl+S to save.");
            ImGui::Separator();
            text_subtle("Navigation");
            ImGui::BulletText("Ctrl+K opens the command palette — search every action + table.");
            ImGui::BulletText("Ctrl+F focuses the table-filter box.  Esc clears it.");
            ImGui::BulletText("Filter matches both the table's name and its snake_case id.");
            ImGui::BulletText(
                "Ctrl+1 / Ctrl+2 / Ctrl+3 switch Tune / Datalog / Features workspaces.");
            ImGui::Separator();
            text_subtle("Viewing");
            ImGui::BulletText("Switch View: Grid \xE2\x86\x94 Heatmap to inspect a map two ways.");
            ImGui::BulletText("For 3D tables, pick a Z slice above the grid.");
            ImGui::Separator();
            text_subtle("Documentation");
            ImGui::BulletText("Repo:    https://github.com/BuffJesus/SubuwuTuner");
            ImGui::BulletText(
                "Design:  docs/00-overview.md \xE2\x80\xA6 docs/16-custom-features.md");
            ImGui::BulletText("License: Apache 2.0 (see LICENSE in the repo root)");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

} // namespace st::ui
