// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Core UI state types. AppState is the gravity well — every render
// function and every action takes `AppState &state`. The small types
// here (Selection, TableViewMode, WorkspaceMode, ConfirmAction,
// ToastKind, Toast, AdapterPickerState) are AppState's fields or its
// transient action payloads.

#ifndef ST_UI_APP_STATE_HPP
#define ST_UI_APP_STATE_HPP

#include "persistence.hpp" // RecentEntry, Settings

#include "st/autotune.hpp"
#include "st/defs/pack_registry.hpp"
#include "st/edit.hpp"
#include "st/feature.hpp"
#include "st/log/adaptive_history.hpp"
#include "st/log/coldstart.hpp"
#include "st/log/ebcs.hpp"
#include "st/log/knock_dashboard.hpp"
#include "st/project.hpp"

#include <imgui.h> // ImVec2 / ImVec4 in features-designer + settings fields

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace st::ui {

// Anchor + cursor selection model. Click sets both; shift-click moves
// only the cursor — the cell rect runs between anchor and cursor
// inclusively. `enabled` distinguishes "nothing selected" from
// "single cell at (0,0)".
struct Selection {
    bool enabled{false};
    std::size_t r_anchor{0};
    std::size_t c_anchor{0};
    std::size_t r_cursor{0};
    std::size_t c_cursor{0};

    [[nodiscard]] bool contains(std::size_t r, std::size_t c) const noexcept {
        if (!enabled) {
            return false;
        }
        auto const rmin = std::min(r_anchor, r_cursor);
        auto const rmax = std::max(r_anchor, r_cursor);
        auto const cmin = std::min(c_anchor, c_cursor);
        auto const cmax = std::max(c_anchor, c_cursor);
        return r >= rmin && r <= rmax && c >= cmin && c <= cmax;
    }

    [[nodiscard]] std::size_t rows() const noexcept {
        return enabled ? (std::max(r_anchor, r_cursor) - std::min(r_anchor, r_cursor) + 1) : 0;
    }
    [[nodiscard]] std::size_t cols() const noexcept {
        return enabled ? (std::max(c_anchor, c_cursor) - std::min(c_anchor, c_cursor) + 1) : 0;
    }

    [[nodiscard]] st::edit::Rect as_rect() const noexcept {
        return st::edit::Rect{std::min(r_anchor, r_cursor), std::max(r_anchor, r_cursor),
                              std::min(c_anchor, c_cursor), std::max(c_anchor, c_cursor)};
    }

    void click(std::size_t r, std::size_t c, bool shift) noexcept {
        if (shift && enabled) {
            r_cursor = r;
            c_cursor = c;
        } else {
            r_anchor = r_cursor = r;
            c_anchor = c_cursor = c;
            enabled = true;
        }
    }

    void reset() noexcept {
        enabled = false;
    }
};

enum class TableViewMode {
    Grid,
    Heatmap,
};

// Coarse workspace presets driven by the left-rail icons. Selecting a
// preset toggles a coherent set of show_*_panel flags so the user can
// flip between "tune-the-map" and "look-at-a-datalog" contexts without
// hunting through the View menu.
enum class WorkspaceMode : std::uint8_t {
    Tune,     // Tables + Table + Stats + History
    Datalog,  // Knock + Adaptive + Cold-Start + EBCS
    Features, // Custom Features Designer
};

// What the user was trying to do when the unsaved-changes modal fired.
// Captured so the modal's Save/Discard handlers know what to do next.
enum class ConfirmAction {
    None,
    OpenDialog,
    OpenRecent,
    NewProject,
    Close,
    Quit,
};

// Transient on-screen feedback — replaces the older "set status_msg
// and let it linger forever" pattern for non-modal success / info
// callouts. Toasts stack bottom-right above the status bar, auto-
// expire after kToastLifetime, fade in the last kToastFadeWindow.
enum class ToastKind : std::uint8_t { Info, Success, Warn, Danger };

struct Toast {
    std::string text;
    ToastKind kind;
    std::chrono::steady_clock::time_point expires_at;
};

// Adapter picker — shared between Read-ROM and (future) Write-ROM flows.
// The struct lives here because it sits inside AppState (read_rom_adapter);
// the rendering / spec-construction helpers live in widgets/adapter_picker.hpp.
struct AdapterPickerState {
    int kind_idx{1};               // 0=J2534, 1=OBDX, 2=Native, 3=Trace (test)
    char device_path[256]{"COM5"}; // OBDX/Native; J2534/Trace hide this field
    char dll_path[1024]{};         // J2534 vendor DLL; others hide this
    char trace_path[1024]{};       // Trace (test) file path; others hide
};

struct AppState {
    std::optional<st::Project> project;
    // Transient status line shown in the status bar's middle cluster.
    // Set whenever an action wants to surface a confirmation or a non-
    // fatal error ("Saved.", "Open dialog error: …"). Auto-clears
    // after ~5 s so stale messages don't sit indefinitely — the
    // tick_status_msg() pass in the render loop manages this; call
    // sites just write the string.
    std::string status_msg;
    // Change-detection shadow + first-seen timestamp for the auto-
    // clear pass. Touched only by tick_status_msg(). Internal — no
    // call site reads or writes these directly.
    std::string status_msg_prev;
    double status_msg_seen_at{0.0};
    std::string selected_table_id;
    std::optional<st::Definition::TableData> current_table_data;
    Selection selection;
    TableViewMode view_mode{TableViewMode::Grid};
    std::size_t selected_z{0};
    bool show_imgui_demo{false};
    bool show_shortcuts_modal{false};
    bool show_about_modal{false};

    // ISO 8601 UTC timestamp of the most recent save_project() success in
    // this session. Drives the "Saved 3 minutes ago" status-bar reading;
    // empty until the first save, which falls back to a plain "Clean"
    // chip (the project on disk is still saved — it just wasn't saved
    // by this session).
    std::optional<std::string> last_save_iso;

    // Path to the bundled fixtures/demo.stune project, resolved at
    // startup from argv[0] (best-effort, depends on the install
    // layout — dev tree, packaged, etc). Welcome panel renders a
    // "Try the demo project" button when this is set; absent in
    // installs that didn't ship the demo.
    std::optional<std::filesystem::path> demo_project_path;

    // Stacked transient feedback (Saved., Loaded foo.stune, Apply
    // failed: …). Rendered by render_toasts at the bottom-right
    // above the status bar; auto-expire. Bounded so a burst of
    // notifications doesn't tile the screen.
    std::vector<Toast> toasts;
    // Phase 5 custom-features designer. Hidden behind View → Debug.
    // Graph data model lives in st::feature; the wiring fields below
    // are transient editor state (only meaningful while the user is
    // mid-drag on a pin) and reset on completion or escape.
    bool show_features_designer{false};
    st::feature::Graph features_graph;
    bool features_wiring_active{false};
    st::feature::NodeId features_wiring_from_node{0};
    st::feature::PinId features_wiring_from_pin{0};
    // Set true when Esc / right-click cancels a wire; blocks the
    // pin-drag handler from spawning a fresh wire while the mouse
    // is still held over the source pin. Cleared when the mouse
    // button is released.
    bool features_wiring_blocked{false};
    // Last connect-attempt error. Surfaced under the canvas when a
    // wire is rejected (type mismatch, fan-in, etc.); cleared on the
    // next successful connect or when a drag starts.
    std::string features_wire_error;
    // Currently-selected nodes on the designer canvas. Plain click
    // replaces with {id}; Shift+click toggles a node in the set;
    // box-select via left-drag from empty canvas replaces with every
    // node whose body intersects the rubber-band rectangle.
    std::vector<st::feature::NodeId> features_selected_nodes;
    // Rubber-band selection state. While `band_active` is true the
    // user is dragging out a rectangle from `band_start` (screen
    // coords) — selection is recomputed each frame from the
    // rectangle until release.
    bool features_band_active{false};
    ImVec2 features_band_start{0.0f, 0.0f};
    // Transient buffer for the pin-default-value editor opened from
    // the pin context menu.
    float features_pin_edit_buf{0.0f};
    // Currently-selected edge on the designer canvas. Mutually
    // exclusive with the selected node — selecting either clears
    // the other.
    std::optional<st::feature::Edge> features_selected_edge;
    // Edge captured by the most recent right-click, used only as a
    // reference for the edge context-menu popup.
    std::optional<st::feature::Edge> features_context_edge;
    // Canvas pan + zoom. `view_offset` translates graph-space
    // coords to screen-space (post-canvas-origin), `view_scale`
    // multiplies them.
    ImVec2 features_view_offset{0.0f, 0.0f};
    float features_view_scale{1.0f};
    // Loaded once at startup, persisted on every successful open. See
    // recents_config_path() for the on-disk location.
    std::vector<RecentEntry> recents;
    Settings settings;

    // Sidebar filter. Substring-matched (case-insensitive) against table
    // name + id. `focus_table_filter` is the Ctrl+F handoff: set by the
    // main-loop shortcut, consumed by the sidebar's next render.
    char table_filter[128]{};
    bool focus_table_filter{false};

    // Command palette (Ctrl+K). Fuzzy/substring search across every
    // menu action, every table in the loaded pack, every panel toggle,
    // every recent project. show_command_palette is the open flag;
    // `command_palette_filter` is the live search buffer;
    // `command_palette_selected` is the highlighted row in the result
    // list (arrow keys move, Enter executes).
    bool show_command_palette{false};
    char command_palette_filter[128]{};
    int command_palette_selected{0};
    bool command_palette_focus_input{false};

    // Workspace preset currently highlighted in the left rail. Default
    // Tune so first-run users land on the editor; clicking a rail icon
    // calls apply_workspace_mode() which flips the relevant show_*
    // flags.
    WorkspaceMode workspace_mode{WorkspaceMode::Tune};

    // DTC-panel filter buffer. Same shape as table_filter; matched
    // against DTC code (P0401) or name.
    char dtc_filter[128]{};

    // Visibility toggles. The Sidebar (Tables) and Table panels are the
    // tuning workspace's primary surface — default on, hidden only by
    // workspace switches (Features mode hides both; Datalog hides Table
    // because its central area is the datalog panel tab strip).
    // Secondary panels (Stats, History, DTCs, datalog quartet, designer)
    // are as-needed and are exposed via View menu checkboxes + the
    // standard dock tab-close X.
    bool show_tables_panel{true};
    bool show_table_view_panel{true};
    bool show_stats_panel{true};
    bool show_dtcs_panel{true};
    bool show_history_panel{true};
    // Per-cylinder knock dashboard — v1.x feature, docs/05 §11.
    bool show_knock_dashboard_panel{false};
    char knock_log_path[1024]{};
    std::string knock_load_error;
    char knock_rpm_col[64]{"rpm"};
    char knock_load_col[64]{"load"};
    char knock_flkc_cols[6][64]{"flkc1", "flkc2", "flkc3", "flkc4", "flkc5", "flkc6"};
    char knock_fbkc_cols[6][64]{"fbkc1", "fbkc2", "fbkc3", "fbkc4", "fbkc5", "fbkc6"};
    int knock_cylinder_count{4};
    float knock_window_seconds{10.0f};
    float knock_sample_rate_hz{20.0f};
    float knock_min_rpm{1500.0f};
    float knock_min_load{1.5f};
    bool knock_gate_enabled{true};
    std::optional<st::log::knock::KnockSnapshot> knock_snapshot;
    std::string knock_compute_msg;
    // Adaptive-learning history panel — docs/05 §11 play 1.
    bool show_adaptive_history_panel{false};
    char ah_log_path[1024]{};
    std::string ah_load_error;
    char ah_ts_col[64]{"ts"};
    char ah_ltft_col[64]{"ltft"};
    char ah_dam_col[64]{"dam"};
    char ah_iac_col[64]{"iac"};
    float ah_bucket_seconds{86400.0f};
    int ah_ts_unit{0}; // 0=s,1=ms,2=us,3=rows
    int ah_min_samples_per_bucket{0};
    std::optional<st::log::adaptive::HistorySnapshot> ah_snapshot;
    std::string ah_compute_msg;
    // Cold-start analysis panel — docs/05 §11 play 3.
    bool show_coldstart_panel{false};
    char cs_log_path[1024]{};
    std::string cs_load_error;
    char cs_ts_col[64]{"ts"};
    char cs_ect_col[64]{"ect"};
    char cs_iat_col[64]{"iat"};
    char cs_rpm_col[64]{"rpm"};
    char cs_obs_col[64]{"obs"};
    char cs_cmd_col[64]{"cmd"};
    float cs_cold_threshold_c{55.0f};
    float cs_ect_bin_width_c{5.0f};
    int cs_min_samples_per_bin{2};
    int cs_ts_unit{0};
    char cs_target_curve[256]{"0:0.82,20:0.88,40:0.95,55:1.00"};
    std::optional<st::log::coldstart::ColdStartSnapshot> cs_snapshot;
    std::vector<std::pair<double, double>> cs_target_curve_parsed;
    std::string cs_compute_msg;
    // EBCS PID assistant panel — docs/05 §11 play 4.
    bool show_ebcs_panel{false};
    char ebcs_log_path[1024]{};
    std::string ebcs_load_error;
    char ebcs_ts_col[64]{"ts"};
    char ebcs_target_col[64]{"target_boost"};
    char ebcs_actual_col[64]{"actual_boost"};
    char ebcs_wgdc_col[64]{"wgdc"};
    char ebcs_throttle_col[64]{"throttle"};
    char ebcs_rpm_col[64]{"rpm"};
    float ebcs_throttle_step_pct{30.0f};
    float ebcs_target_step{2.0f};
    float ebcs_max_event_duration{6.0f};
    float ebcs_overshoot_warn_pct{15.0f};
    int ebcs_ts_unit{0};
    std::optional<st::log::ebcs::BoostSnapshot> ebcs_snapshot;
    std::string ebcs_compute_msg;

    // History-panel filter buffer. Substring-matched (case-insensitive)
    // against the edit's table_id.
    char history_filter[128]{};

    // Inline cell-value editor state.
    bool editing_cell{false};
    bool editor_just_opened{false};
    char edit_buf[64]{};

    // Unsaved-changes tracking.
    bool dirty{false};
    ConfirmAction next_action{ConfirmAction::None};
    std::filesystem::path next_recent{};
    bool show_unsaved_modal{false};
    bool user_confirmed_quit{false};

    // Flash-flow modal state.
    bool show_flash_modal{false};
    bool flash_confirm_checked{false};
    char flash_reason[512]{};

    // CSV import preview modal.
    bool show_csv_import_modal{false};
    st::EditCsvParseResult csv_import_parsed;
    std::string csv_import_table_id;
    std::filesystem::path csv_import_source_path;
    std::optional<st::Definition::TableData> csv_import_before_values;
    std::string csv_import_apply_error;

    // New-project modal.
    bool show_new_project_modal{false};
    char np_source_path[1024]{};
    char np_def_path[1024]{};
    char np_dir_path[1024]{};
    char np_display_name[256]{};
    enum class NpMatchStatus { None, Match, NoMatch, LoadFailed };
    NpMatchStatus np_match_status{NpMatchStatus::None};
    std::string np_match_message;
    std::string np_cached_source_path;
    std::string np_cached_def_path;
    std::string np_create_error;

    // MAF autotune modal.
    bool show_maf_autotune_modal{false};
    char maf_at_table_id[128]{};
    char maf_at_log_path[1024]{};
    float maf_at_gain{0.5f};
    float maf_at_max_delta_pct{0.08f};
    int maf_at_min_samples{50};
    bool maf_at_apply_smooth{true};
    bool maf_at_require_open_loop{false};
    std::optional<st::autotune::MafTuneResult> maf_at_result;
    std::vector<st::autotune::LintViolation> maf_at_lints;
    std::optional<st::Definition::TableData> maf_at_table_data;
    std::string maf_at_status_msg;

    // Knock-pull autotune modal.
    bool show_kp_autotune_modal{false};
    char kp_at_table_id[128]{};
    char kp_at_log_path[1024]{};
    int kp_at_rpm_axis_kind{0};
    float kp_at_trigger_degrees{1.5f};
    float kp_at_pull_step_degrees{0.75f};
    int kp_at_min_samples{30};
    bool kp_at_enable_add_back{false};
    float kp_at_add_step_degrees{0.5f};
    int kp_at_add_back_min_clean{50};
    float kp_at_clean_threshold{0.05f};
    std::optional<st::autotune::KnockPullResult> kp_at_result;
    std::vector<st::autotune::LintViolation> kp_at_lints;
    std::optional<st::Definition::TableData> kp_at_table_data;
    std::vector<double> kp_at_rpm_axis_values;
    std::vector<double> kp_at_load_axis_values;
    std::string kp_at_status_msg;

    // Read-ROM-from-car modal.
    bool show_read_rom_modal{false};
    enum class ReadRomState : std::uint8_t {
        Idle,
        Running,
        Done,
        Failed,
        Cancelled,
    };
    ReadRomState read_rom_state{ReadRomState::Idle};
    AdapterPickerState read_rom_adapter{};
    char read_rom_base_addr_hex[32]{"0x0"};
    char read_rom_size_hex[32]{"0x200000"};
    int read_rom_max_chunk{0x100};
    int read_rom_protocol{0};
    bool read_rom_verbose{true};
    bool read_rom_authenticate{true};
    int read_rom_sa_variant_idx{0};
    std::shared_ptr<std::atomic<std::uint32_t>> read_rom_bytes_done;
    std::shared_ptr<std::atomic<std::uint32_t>> read_rom_total_bytes;
    std::shared_ptr<std::atomic<bool>> read_rom_cancel;
    std::vector<std::uint8_t> read_rom_bytes_result;
    std::string read_rom_error_msg;
    std::string read_rom_error_msg_prev;
    std::thread read_rom_worker;
    std::chrono::steady_clock::time_point read_rom_start_time;

    // Method bodies defined out-of-class in main.cpp (and eventually in
    // app_state.cpp). The bodies call file-local helpers like
    // enqueue_toast / push_recent / save_recents that live next to the
    // definitions, so member functions are defined where those helpers
    // are visible.
    void try_open_project(std::filesystem::path const &path);
    void select_table(std::string const &id);
    void close_project();

    // Settings modal (Tools -> Settings...).
    bool show_settings_modal{false};
    char settings_def_root_input[1024]{};
    char settings_rom_dump_root_input[1024]{};
    bool settings_loaded_once{false};
    std::string settings_status_msg;
    ImVec4 settings_status_color{1.0f, 1.0f, 1.0f, 1.0f};

    // Pack-registry browser modal (Tools -> Browse Definitions).
    bool show_def_registry_modal{false};
    char def_registry_root_input[1024]{"definitions"};
    bool def_registry_scanned{false};
    std::vector<std::string> def_registry_warnings;
    struct DefRegistryRow {
        std::string id;
        std::string platform;
        std::string source_label;
        std::string display_name;
    };
    std::vector<DefRegistryRow> def_registry_rows;
    char def_registry_filter[128]{};
};

} // namespace st::ui

#endif
