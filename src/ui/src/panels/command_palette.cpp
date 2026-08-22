// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Command palette (Ctrl+K) — fuzzy/substring search across every menu
// action, every panel toggle, every recent project, and every table in
// the loaded pack. One affordance that scales as features land:
// every new entry becomes discoverable without growing the menu
// surface.
//
// Implementation choices:
//   * Enum + payload, not std::function — avoids capturing AppState by
//     reference into command objects that are rebuilt every frame
//     (cheap allocations would dominate the cost on a 100+ table pack).
//   * Substring match via the existing icontains() helper. Fuzzy
//     subsequence matching is a v1.x polish — substring is the floor.
//   * Result list reuses ImGui::Selectable so arrow-key nav comes for
//     free if/when we flip on NavEnableKeyboard later; for now arrow
//     keys are driven explicitly by IsKeyPressed below.

#include "st/policy.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "modals/modals.hpp" // pack_supports_fa24_swap
#include "panels/panels.hpp"
#include "project_io.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {
namespace {

enum class PaletteCommandKind : std::uint8_t {
    NewProject,
    OpenProject,
    SaveProject,
    CloseProject,
    ImportCsv,
    ExportCsv,
    ExportCsvDiff,
    Quit,
    Undo,
    Redo,
    Copy,
    Paste,
    ResetToSource,
    AutotuneMaf,
    AutotuneKp,
    ReadRom,
    BrowseDefs,
    Settings,
    ViewGrid,
    ViewHeatmap,
    ToggleStats,
    ToggleDtcs,
    ToggleHistory,
    ToggleKnock,
    ToggleAdaptive,
    ToggleColdstart,
    ToggleEbcs,
    ToggleFeatures,
    ToggleGaugeCluster,
    ToggleLogExplorer,
    ToggleCompare,
    ToggleAudit,
    ToggleAp3Browser,
    ToggleLibrary,
    ResetLayout,
    ThemeDark,
    ThemeLight,
    Shortcuts,
    About,
    HelpTopics,
    WelcomeWizard,
    Flash,
    OpenRecent,      // payload = absolute path
    OpenTable,       // payload = table id
    SwitchActiveRom, // payload = "working" / "source" / additional id
    FA24Swap,        // open the FA24 swap workflow modal
};

struct PaletteCommand {
    PaletteCommandKind kind;
    std::string label;
    std::string hint;     // dimmed subtitle; usually a shortcut or category
    char const *category; // group label for the chip on the right
    std::string payload;  // path for OpenRecent, id for OpenTable; empty otherwise
};

// Build the command list. Recomputed every frame the palette is open —
// the palette's contents depend on AppState (has_project, recents,
// loaded pack tables) and rebuilding is cheaper than maintaining a
// reactive cache. Static actions go first so the palette is useful on
// the empty welcome screen; tables + recents tail because they're
// project-dependent and grow large.
std::vector<PaletteCommand> build_palette_commands(AppState const &state) {
    std::vector<PaletteCommand> out;
    out.reserve(64);
    bool const has_project = state.project.has_value();
    bool const has_selection = state.selection.enabled;
    // Undo/redo reflect the active slot's per-ROM history
    // (Issue #10 phase 3). Source: empty (no edits), additional or
    // working: the slot's own history.
    bool const can_undo = has_project && state.project->active_history().can_undo();
    bool const can_redo = has_project && state.project->active_history().can_redo();
    bool const can_csv = has_project && !state.selected_table_id.empty();

    auto push = [&](PaletteCommandKind k, char const *label, char const *hint, char const *category,
                    std::string payload = {}) {
        out.push_back({k, label, hint, category, std::move(payload)});
    };

    // File — always available except the project-dependent ones.
    push(PaletteCommandKind::NewProject, "New project…", "", "File");
    push(PaletteCommandKind::OpenProject, "Open project…", "Ctrl+O", "File");
    if (has_project) {
        push(PaletteCommandKind::SaveProject, "Save project", "Ctrl+S", "File");
        push(PaletteCommandKind::CloseProject, "Close project", "", "File");
        if (can_csv) {
            push(PaletteCommandKind::ImportCsv, "Import CSV into current table…", "", "File");
            push(PaletteCommandKind::ExportCsv, "Export current table as CSV…", "", "File");
            push(PaletteCommandKind::ExportCsvDiff, "Export current table edits as CSV…", "",
                 "File");
        }
    }
    push(PaletteCommandKind::Quit, "Quit", "Ctrl+Q", "File");

    // Edit — undo/redo conditional, but list them even when unavailable
    // so the user discovers the shortcut. dispatch_palette_command short-
    // circuits when the action isn't legal.
    if (has_project) {
        push(PaletteCommandKind::Undo, "Undo", can_undo ? "Ctrl+Z" : "Ctrl+Z (nothing to undo)",
             "Edit");
        push(PaletteCommandKind::Redo, "Redo",
             can_redo ? "Ctrl+Shift+Z" : "Ctrl+Shift+Z (nothing to redo)", "Edit");
        if (has_selection) {
            push(PaletteCommandKind::Copy, "Copy selection", "Ctrl+C", "Edit");
            push(PaletteCommandKind::Paste, "Paste at cursor", "Ctrl+V", "Edit");
            push(PaletteCommandKind::ResetToSource, "Reset selection to source", "", "Edit");
        }
        push(PaletteCommandKind::AutotuneMaf, "Auto-Tune: MAF…", "From a CSV datalog", "Edit");
        push(PaletteCommandKind::AutotuneKp, "Auto-Tune: Knock Pull…", "From a CSV datalog",
             "Edit");
    }

    // Tools — always available; their modals handle the no-project case.
    push(PaletteCommandKind::ReadRom, "Read ROM from car…", "Dump current cal", "Tools");
    push(PaletteCommandKind::BrowseDefs, "Browse definitions…", "Pack registry", "Tools");
    push(PaletteCommandKind::Settings, "Settings…", "", "Tools");
    // Pack-declared workflows — only surface when the loaded pack
    // qualifies, so the palette doesn't list a half-dozen disabled
    // workflows on every demo project. Future workflows (Stage1→2
    // step, E85 conversion) follow the same shape.
    if (has_project && pack_supports_fa24_swap(state)) {
        push(PaletteCommandKind::FA24Swap, "FA24 swap (VA WRX)\xE2\x80\xA6", "Guided 3-step recipe",
             "Workflows");
    }

    // View — table view modes only meaningful with a project loaded.
    if (has_project) {
        push(PaletteCommandKind::ViewGrid, "Table view: Grid", "", "View");
        push(PaletteCommandKind::ViewHeatmap, "Table view: Heatmap", "", "View");
        // Active-ROM switches — only the inactive slots are listed so
        // the palette doesn't offer "switch to <current>" no-ops.
        bool const is_working_active = state.viewing_working_rom();
        bool const is_source_active = state.active_rom_id == "source";
        if (!is_working_active) {
            push(PaletteCommandKind::SwitchActiveRom, "View: Switch to Working ROM", "Active ROM",
                 "View", "working");
        }
        if (!is_source_active) {
            push(PaletteCommandKind::SwitchActiveRom, "View: Switch to Source ROM (read-only)",
                 "Active ROM", "View", "source");
        }
        for (auto const &r : state.project->additional_roms()) {
            if (state.active_rom_id == r.id) {
                continue;
            }
            std::string const display = r.display_name.empty() ? r.id : r.display_name;
            std::string label = "View: Switch to " + display;
            push(PaletteCommandKind::SwitchActiveRom, label.c_str(), "Active ROM", "View", r.id);
        }
    }
    // Panel toggles — useful even without a project (panels render their
    // own empty states). Label says "Show" / "Hide" based on current state
    // so the user always sees the action they're about to take.
    auto const toggle_label = [](bool on, char const *name) {
        return std::string{on ? "Hide " : "Show "} + name;
    };
    push(PaletteCommandKind::ToggleStats,
         toggle_label(state.show_stats_panel, "Stats panel").c_str(), "", "View");
    push(PaletteCommandKind::ToggleDtcs, toggle_label(state.show_dtcs_panel, "DTCs panel").c_str(),
         "", "View");
    push(PaletteCommandKind::ToggleHistory,
         toggle_label(state.show_history_panel, "History panel").c_str(), "", "View");
    push(PaletteCommandKind::ToggleKnock,
         toggle_label(state.show_knock_dashboard_panel, "Knock Dashboard").c_str(), "Datalog",
         "View");
    push(PaletteCommandKind::ToggleAdaptive,
         toggle_label(state.show_adaptive_history_panel, "Adaptive History").c_str(), "Datalog",
         "View");
    push(PaletteCommandKind::ToggleColdstart,
         toggle_label(state.show_coldstart_panel, "Cold-Start Analysis").c_str(), "Datalog",
         "View");
    push(PaletteCommandKind::ToggleEbcs,
         toggle_label(state.show_ebcs_panel, "EBCS PID Assistant").c_str(), "Datalog", "View");
    push(PaletteCommandKind::ToggleFeatures,
         toggle_label(state.show_features_designer, "Custom Features Designer").c_str(), "Features",
         "View");
    push(PaletteCommandKind::ToggleGaugeCluster,
         toggle_label(state.show_gauge_cluster_panel, "Gauge Cluster").c_str(), "Datalog", "View");
    push(PaletteCommandKind::ToggleLogExplorer,
         toggle_label(state.show_log_explorer_panel, "Log Explorer").c_str(), "Datalog", "View");
    push(PaletteCommandKind::ToggleCompare,
         toggle_label(state.show_compare_panel, "Compare ROMs").c_str(), "Diff", "View");
    push(PaletteCommandKind::ToggleAudit, toggle_label(state.show_audit_panel, "Audit log").c_str(),
         "Project", "View");
#ifdef ST_HAVE_AP_WORKFLOW
    push(PaletteCommandKind::ToggleAp3Browser,
         toggle_label(state.show_ap3_browser_panel, "AccessPort Browser").c_str(), "Hardware",
         "View");
#endif
    push(PaletteCommandKind::ToggleLibrary,
         toggle_label(state.show_library_panel, "Tune Library").c_str(), "Library", "View");
    push(PaletteCommandKind::ResetLayout, "Reset window layout",
         "Rebuild the default dock arrangement", "View");
    // Theme — show the inactive one. No point listing "Theme: Dark" when
    // Dark is already on; the action would be a no-op.
    if (state.settings.theme == Theme::Dark) {
        push(PaletteCommandKind::ThemeLight, "Switch to Light theme", "", "View");
    } else {
        push(PaletteCommandKind::ThemeDark, "Switch to Dark theme", "", "View");
    }

    // Tools — surfaces that aren't panels but still high-traffic.
    if (has_project) {
        push(PaletteCommandKind::Flash, "Flash…", "Preview the flash plan + policy gate", "Tools");
    }

    // Help.
    push(PaletteCommandKind::Shortcuts, "Keyboard shortcuts…", "", "Help");
    push(PaletteCommandKind::HelpTopics, "Topics & Glossary…", "Open the in-app help browser",
         "Help");
    push(PaletteCommandKind::WelcomeWizard, "Run welcome wizard…",
         "Re-pick jurisdiction / units / theme / demo project", "Help");
    push(PaletteCommandKind::About, "About SubuwuTuner…", "", "Help");

    // Recents — surface every entry; let the matcher narrow them down.
    // Use basename as the label and full path as the hint so a short
    // search like "demo" finds fixtures/demo.stune.
    for (auto const &r : state.recents) {
        auto base = r.path.filename().string();
        if (base.empty())
            base = r.path.string();
        push(PaletteCommandKind::OpenRecent, ("Open: " + base).c_str(), r.path.string().c_str(),
             "Recent", r.path.string());
    }

    // Tables — every table in the loaded pack. Label uses the human
    // name; hint carries the snake_case id + 0x address + category.
    if (has_project) {
        auto const &def = state.project->definition();
        for (auto const &t : def.tables()) {
            char hint_buf[160];
            char const *cat = t.category.empty() ? "Other" : t.category.c_str();
            std::snprintf(hint_buf, sizeof hint_buf, "%s · 0x%08zX · %s", t.id.c_str(), t.address,
                          cat);
            char const *label = t.name.empty() ? t.id.c_str() : t.name.c_str();
            push(PaletteCommandKind::OpenTable, ("Go to: " + std::string{label}).c_str(), hint_buf,
                 "Table", t.id);
        }
    }

    return out;
}

// request_layout_reset() is declared in panels.hpp; the palette
// dispatcher and the View menu's "Reset window layout" entry call it.

void dispatch_palette_command(AppState &state, PaletteCommand const &cmd) {
    switch (cmd.kind) {
    case PaletteCommandKind::NewProject:
        request_action(state, ConfirmAction::NewProject);
        break;
    case PaletteCommandKind::OpenProject:
        request_action(state, ConfirmAction::OpenDialog);
        break;
    case PaletteCommandKind::SaveProject:
        if (state.project.has_value())
            save_project(state);
        break;
    case PaletteCommandKind::CloseProject:
        if (state.project.has_value())
            request_action(state, ConfirmAction::Close);
        break;
    case PaletteCommandKind::ImportCsv:
        if (state.project.has_value() && !state.selected_table_id.empty())
            import_csv_into_current_table_dialog(state);
        break;
    case PaletteCommandKind::ExportCsv:
        if (state.project.has_value() && !state.selected_table_id.empty())
            export_current_table_csv_dialog(state, /*diff_only=*/false);
        break;
    case PaletteCommandKind::ExportCsvDiff:
        if (state.project.has_value() && !state.selected_table_id.empty())
            export_current_table_csv_dialog(state, /*diff_only=*/true);
        break;
    case PaletteCommandKind::Quit:
        request_action(state, ConfirmAction::Quit);
        break;
    case PaletteCommandKind::Undo:
        do_undo(state);
        break;
    case PaletteCommandKind::Redo:
        do_redo(state);
        break;
    case PaletteCommandKind::Copy:
        if (state.selection.enabled)
            do_copy_selection(state);
        break;
    case PaletteCommandKind::Paste:
        if (state.selection.enabled)
            paste_clipboard_at_cursor(state);
        break;
    case PaletteCommandKind::ResetToSource:
        if (state.selection.enabled)
            reset_selection_to_source(state);
        break;
    case PaletteCommandKind::AutotuneMaf:
        if (state.project.has_value()) {
            // Mirror the menubar's MAF launcher (main.cpp ~4985). Keep
            // these two callsites in sync if the init shape grows; a
            // future open_autotune_maf() helper would dedupe both.
            std::snprintf(state.maf_at_table_id, sizeof state.maf_at_table_id, "%s",
                          state.selected_table_id.c_str());
            state.maf_at_status_msg.clear();
            state.maf_at_result.reset();
            state.maf_at_lints.clear();
            state.maf_at_table_data.reset();
            state.show_maf_autotune_modal = true;
        }
        break;
    case PaletteCommandKind::AutotuneKp:
        if (state.project.has_value()) {
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
        break;
    case PaletteCommandKind::ReadRom:
        state.read_rom_state = AppState::ReadRomState::Idle;
        state.read_rom_error_msg.clear();
        state.read_rom_bytes_result.clear();
        state.show_read_rom_modal = true;
        break;
    case PaletteCommandKind::BrowseDefs:
        state.show_def_registry_modal = true;
        break;
    case PaletteCommandKind::Settings:
        state.show_settings_modal = true;
        break;
    case PaletteCommandKind::FA24Swap:
        state.show_fa24_swap_modal = true;
        break;
    case PaletteCommandKind::ViewGrid:
        state.view_mode = TableViewMode::Grid;
        break;
    case PaletteCommandKind::ViewHeatmap:
        state.view_mode = TableViewMode::Heatmap;
        break;
    case PaletteCommandKind::ToggleStats:
        state.show_stats_panel = !state.show_stats_panel;
        break;
    case PaletteCommandKind::ToggleDtcs:
        state.show_dtcs_panel = !state.show_dtcs_panel;
        break;
    case PaletteCommandKind::ToggleHistory:
        state.show_history_panel = !state.show_history_panel;
        break;
    case PaletteCommandKind::ToggleKnock:
        state.show_knock_dashboard_panel = !state.show_knock_dashboard_panel;
        break;
    case PaletteCommandKind::ToggleAdaptive:
        state.show_adaptive_history_panel = !state.show_adaptive_history_panel;
        break;
    case PaletteCommandKind::ToggleColdstart:
        state.show_coldstart_panel = !state.show_coldstart_panel;
        break;
    case PaletteCommandKind::ToggleEbcs:
        state.show_ebcs_panel = !state.show_ebcs_panel;
        break;
    case PaletteCommandKind::ToggleFeatures:
        state.show_features_designer = !state.show_features_designer;
        break;
    case PaletteCommandKind::ToggleGaugeCluster:
        state.show_gauge_cluster_panel = !state.show_gauge_cluster_panel;
        break;
    case PaletteCommandKind::ToggleLogExplorer:
        state.show_log_explorer_panel = !state.show_log_explorer_panel;
        break;
    case PaletteCommandKind::ToggleCompare:
        state.show_compare_panel = !state.show_compare_panel;
        break;
    case PaletteCommandKind::ToggleAudit:
        state.show_audit_panel = !state.show_audit_panel;
        break;
    case PaletteCommandKind::ToggleAp3Browser:
        state.show_ap3_browser_panel = !state.show_ap3_browser_panel;
        break;
    case PaletteCommandKind::ToggleLibrary:
        state.show_library_panel = !state.show_library_panel;
        break;
    case PaletteCommandKind::ResetLayout:
        request_layout_reset();
        break;
    case PaletteCommandKind::ThemeDark:
        state.settings.theme = Theme::Dark;
        apply_theme(Theme::Dark);
        save_settings(state.settings);
        break;
    case PaletteCommandKind::ThemeLight:
        state.settings.theme = Theme::Light;
        apply_theme(Theme::Light);
        save_settings(state.settings);
        break;
    case PaletteCommandKind::Shortcuts:
        state.show_shortcuts_modal = true;
        break;
    case PaletteCommandKind::About:
        state.show_about_modal = true;
        break;
    case PaletteCommandKind::HelpTopics:
        state.show_help_modal = true;
        break;
    case PaletteCommandKind::WelcomeWizard:
        state.show_first_run_wizard = true;
        state.first_run_step = 0;
        break;
    case PaletteCommandKind::Flash:
        state.show_flash_modal = true;
        state.flash_confirm_checked = false;
        state.flash_reason[0] = '\0';
        break;
    case PaletteCommandKind::OpenRecent:
        request_action(state, ConfirmAction::OpenRecent, std::filesystem::path{cmd.payload});
        break;
    case PaletteCommandKind::OpenTable:
        if (state.project.has_value()) {
            state.select_table(cmd.payload);
        }
        break;
    case PaletteCommandKind::SwitchActiveRom:
        set_active_view_rom(state, cmd.payload);
        break;
    }
}

} // namespace

void open_command_palette(AppState &state) {
    state.show_command_palette = true;
    state.command_palette_filter[0] = '\0';
    state.command_palette_selected = 0;
    state.command_palette_focus_input = true;
}

// The modal itself. Centered, 560×420 max, transparent backdrop. Search
// input takes focus on open; arrow keys move the highlighted row; Enter
// executes; Esc closes. List virtualization isn't needed for the row
// counts we expect (welcome state ~20 commands; max ~200 with a 100-
// table pack + recents), so a straight ImGui::Selectable loop is fine.
void render_command_palette(AppState &state) {
    if (!state.show_command_palette) {
        return;
    }

    auto const *vp = ImGui::GetMainViewport();
    constexpr float kPaletteW = 560.0f;
    constexpr float kPaletteH = 420.0f;
    ImVec2 const center{vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                        vp->WorkPos.y + vp->WorkSize.y * 0.30f};
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(kPaletteW, kPaletteH), ImGuiCond_Always);

    if (!ImGui::IsPopupOpen("##command_palette")) {
        ImGui::OpenPopup("##command_palette");
    }

    ImGuiWindowFlags const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal("##command_palette", nullptr, flags)) {
        return;
    }

    // Esc closes — handled here as well as the input-bound EscapeClearsAll
    // so an empty input still dismisses on first press.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
        state.show_command_palette = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    // Search input. Auto-focus on open; subsequent frames leave focus
    // to whatever the user does (arrow-keys jump out, click jumps back).
    if (state.command_palette_focus_input) {
        ImGui::SetKeyboardFocusHere();
        state.command_palette_focus_input = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    bool const filter_changed = ImGui::InputTextWithHint(
        "##command_palette_filter", "Search commands, tables, recents… (Esc to close)",
        state.command_palette_filter, sizeof state.command_palette_filter);
    if (filter_changed) {
        // Any input change resets the selection so the user always sees
        // the top match highlighted as they type.
        state.command_palette_selected = 0;
    }
    ImGui::Separator();

    // Build + filter. Substring match against label OR hint (so a search
    // for "0x66000" finds tables by address; a search for "Ctrl+S"
    // surfaces Save Project).
    auto const all = build_palette_commands(state);
    std::string_view const needle{state.command_palette_filter};
    std::vector<std::size_t> matches;
    matches.reserve(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (icontains(all[i].label, needle) || icontains(all[i].hint, needle) ||
            icontains(all[i].category, needle)) {
            matches.push_back(i);
        }
    }

    if (matches.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceL));
        text_centered_subtle(needle.empty() ? "No commands available."
                                            : "No matches. Try a shorter query.");
        ImGui::EndPopup();
        return;
    }

    // Clamp the selected index to valid matches. Up/Down arrows nav.
    if (state.command_palette_selected < 0)
        state.command_palette_selected = 0;
    if (state.command_palette_selected >= static_cast<int>(matches.size()))
        state.command_palette_selected = static_cast<int>(matches.size()) - 1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, /*repeat=*/true)) {
        state.command_palette_selected =
            (state.command_palette_selected + 1) % static_cast<int>(matches.size());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, /*repeat=*/true)) {
        state.command_palette_selected =
            (state.command_palette_selected - 1 + static_cast<int>(matches.size())) %
            static_cast<int>(matches.size());
    }

    // Enter executes whatever's highlighted. Capture and dispatch after
    // closing so the dispatch's side effects (open another modal, change
    // the dock layout) don't interleave with this popup's End.
    std::optional<std::size_t> to_execute;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false)) {
        to_execute = matches[static_cast<std::size_t>(state.command_palette_selected)];
    }

    // Result list. Reserve room for the footer hint by sizing the child
    // explicitly. ScrollY only — horizontal overflow truncates with an
    // ellipsis via ImGui's default text clipping. Child flags / window
    // flags both zero: we drive selection by index from arrow-key
    // pressed events above rather than ImGui's nav system.
    float const footer_h = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##palette_results", ImVec2(0.0f, -footer_h - kSpaceS), 0, 0)) {
        for (std::size_t i = 0; i < matches.size(); ++i) {
            auto const &cmd = all[matches[i]];
            bool const is_sel = (static_cast<int>(i) == state.command_palette_selected);
            ImGui::PushID(static_cast<int>(i));
            // Single-line selectable; subtitle and category chip drawn
            // manually so they don't compete for the click target.
            char row[256];
            std::snprintf(row, sizeof row, "%s", cmd.label.c_str());
            if (ImGui::Selectable(row, is_sel,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                to_execute = matches[i];
            }
            if (is_sel) {
                ImGui::SetScrollHereY(0.5f);
            }
            // Category chip, right-aligned. Reuses the muted chip palette
            // so it sits quietly beside the row text.
            if (cmd.category != nullptr && cmd.category[0] != '\0') {
                float const chip_w = ImGui::CalcTextSize(cmd.category).x + 16.0f;
                float const right_x = ImGui::GetWindowContentRegionMax().x - chip_w -
                                      ImGui::GetStyle().FramePadding.x;
                ImGui::SameLine();
                ImGui::SetCursorPosX(right_x);
                chip(cmd.category, chip_fg_muted(), chip_bg_muted());
            }
            // Subtitle row: hint text underneath at small size. Skipped
            // when the hint is empty so single-line commands stay tight.
            if (!cmd.hint.empty()) {
                text_subtle("  %s", cmd.hint.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Footer hint — keyboard affordances. Same dimmed text as the rest
    // of the palette's chrome.
    text_subtle("\xE2\x86\x91 \xE2\x86\x93 to move  \xC2\xB7  Enter to run  \xC2\xB7  "
                "Esc to close  \xC2\xB7  %zu of %zu",
                matches.size(), all.size());

    if (to_execute.has_value()) {
        auto const cmd = all[*to_execute]; // copy before close (dispatch may mutate state)
        state.show_command_palette = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        dispatch_palette_command(state, cmd);
        return;
    }

    ImGui::EndPopup();
}

} // namespace st::ui
