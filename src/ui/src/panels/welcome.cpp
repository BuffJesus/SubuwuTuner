// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Welcome panel — what the user sees before any project is loaded. Goal
// is welcoming, not utilitarian: clean type hierarchy, one obvious next
// action, no jargon above the fold. Recents block + "What's new" +
// rotating tip + footer-link to keyboard shortcuts.

#include "panels/panels.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "modals/modals.hpp" // pack_supports_fa24_swap
#include "persistence.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/core/version.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace st::ui {

namespace {

// Parse the [Unreleased] section out of CHANGELOG.md. Each bullet
// becomes a WhatsNewItem tagged with the surrounding `### Section`
// heading. Stops at the next `## ` heading (Pre-Unreleased,
// Released, etc.). Best-effort — malformed files just produce
// fewer items.
void parse_unreleased(std::string_view body,
                      std::vector<AppState::WhatsNewItem> &out) {
    out.clear();
    bool in_unreleased = false;
    std::string current_section = "Changes";
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t line_end = body.find('\n', i);
        if (line_end == std::string_view::npos) {
            line_end = body.size();
        }
        std::string_view line{body.data() + i, line_end - i};
        i = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("## ")) {
            in_unreleased = (line.find("[Unreleased]") != std::string_view::npos);
            current_section = "Changes";
            continue;
        }
        if (!in_unreleased) {
            continue;
        }
        if (line.starts_with("### ")) {
            current_section = std::string{line.substr(4)};
            continue;
        }
        if (line.starts_with("- ") || line.starts_with("* ")) {
            AppState::WhatsNewItem item;
            item.section = current_section;
            item.body = std::string{line.substr(2)};
            // Strip markdown back-tick code spans for readability —
            // text_subtle doesn't render markdown, just plain text.
            std::string clean;
            clean.reserve(item.body.size());
            bool in_code = false;
            for (char c : item.body) {
                if (c == '`') {
                    in_code = !in_code;
                    continue;
                }
                clean.push_back(c);
            }
            item.body = std::move(clean);
            out.push_back(std::move(item));
        }
    }
}

void load_whats_new(AppState &state) {
    state.whats_new.clear();
    if (!state.changelog_path.has_value()) {
        state.whats_new_loaded = true;
        return;
    }
    std::ifstream in{*state.changelog_path, std::ios::binary};
    if (!in) {
        state.whats_new_loaded = true;
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    parse_unreleased(ss.str(), state.whats_new);
    state.whats_new_loaded = true;
}

} // namespace

void render_welcome_panel(AppState &state) {
    // F1 on the Welcome panel should land on docs/00-overview — the
    // sidebar's first entry, and the right starting point for someone
    // who's never opened the tool before. The HelpContext value
    // already exists in the enum; just hadn't been wired.
    track_help_context(state, AppState::HelpContext::Welcome);
    ImVec2 const avail = ImGui::GetContentRegionAvail();
    // Balance content vertically. On short panels (default window) the
    // welcome cluster sits in the upper third — first-run feel,
    // recents have room without scrolling. On tall panels (maximized,
    // multi-monitor) the cluster shifts toward vertical-center so it
    // doesn't float in a huge empty space.
    //
    // Heuristic: take whichever is larger between a 10%-of-panel
    // anchor and a "would-be centered if content is ~320px tall"
    // calculation scaled at 40% of the remainder. This keeps the
    // default-window layout unchanged but pushes content meaningfully
    // lower on a 1300+px panel.
    bool const has_recents = !state.recents.empty();
    float const min_pad = avail.y * (has_recents ? 0.10f : 0.22f);
    float const center_bias = (avail.y - 320.0f) * 0.40f;
    float const top_pad = std::max(min_pad, center_bias);
    ImGui::Dummy(ImVec2(0.0f, top_pad));

    text_centered("SubuwuTuner", 2.4f);

    // Thin brand-purple accent rule under the title. Visual weight is
    // small but consistent across themes — uses the same accent the
    // status-bar profile chip and tab-selected overline use, so the
    // first thing the user sees on launch matches the active-element
    // language elsewhere in the app.
    {
        constexpr float kRuleW = 64.0f;
        constexpr float kRuleH = 2.0f;
        center_cursor_x(kRuleW);
        ImVec2 const p = ImGui::GetCursorScreenPos();
        auto *const dl = ImGui::GetWindowDrawList();
        // Read from current_theme() not state.settings.theme — the
        // global is what apply_theme() actually wrote, so this matches
        // the ImGui style colors the surrounding widgets are using.
        auto const [accent, accent_hover, accent_active] = accent_for(current_theme());
        (void)accent_hover;
        (void)accent_active;
        ImU32 const col = ImGui::GetColorU32(accent);
        // Rounded pill, not a hard line — softer against the centered
        // title without bleeding into a "load progress" affordance.
        dl->AddRectFilled(p, ImVec2(p.x + kRuleW, p.y + kRuleH), col, kRuleH * 0.5f);
        ImGui::Dummy(ImVec2(kRuleW, kRuleH + 10.0f));
    }

    text_centered_subtle("Open a Subaru ECU tune to read, edit, and flash.");
    glossary_tooltip_for(state, "ECU");
    ImGui::Dummy(ImVec2(0.0f, 28.0f));

    constexpr float kBtnW = 240.0f;
    constexpr float kBtnH = 38.0f;
    center_cursor_x(kBtnW);
    if (ImGui::Button("Open Project…", ImVec2(kBtnW, kBtnH))) {
        request_action(state, ConfirmAction::OpenDialog);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a .stune project directory.  (Ctrl+O)");
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_centered_subtle("Ctrl+O");

    // New-project CTA. Slightly smaller than Open Project so the
    // welcome panel keeps its primary-action / secondary-action
    // hierarchy — opening an existing project is the more common
    // first move.
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    constexpr float kSecondaryW = 200.0f;
    center_cursor_x(kSecondaryW);
    if (ImGui::Button("New project…", ImVec2(kSecondaryW, 30.0f))) {
        // Welcome panel only renders when no project is loaded, so the
        // dirty-state guard inside request_action is a no-op here —
        // but routing through it keeps every entry point consistent
        // with the File menu's New Project… item.
        request_action(state, ConfirmAction::NewProject);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a source ROM and definition pack, then\n"
                          "create a new .stune project directory.");
    }

    // "Try the demo project" CTA — third tier, only renders when
    // the resolver actually located fixtures/demo.stune at startup
    // (dev tree or install layout that ships fixtures alongside the
    // binary). One click → open the bundled project, no file dialog,
    // no required ROM dump. Highest leverage on a fresh first-run
    // experience; quietly absent when not available so packaged
    // installs without the demo don't show a broken button.
    if (state.demo_project_path.has_value()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        constexpr float kTertiaryW = 200.0f;
        center_cursor_x(kTertiaryW);
        if (ImGui::Button("Try the demo project", ImVec2(kTertiaryW, 28.0f))) {
            request_action(state, ConfirmAction::OpenRecent,
                           *state.demo_project_path);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open the bundled fixtures/demo.stune project.\n%s",
                              state.demo_project_path->string().c_str());
        }
    }

    // Welcome-wizard discovery button. Always visible on the welcome
    // panel — returning users may want to re-run setup. Mirrors the
    // Help → Welcome wizard menu item but more discoverable.
    {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        constexpr float kWizW = 200.0f;
        center_cursor_x(kWizW);
        if (ImGui::Button("Run welcome wizard\xE2\x80\xA6", ImVec2(kWizW, 28.0f))) {
            state.show_first_run_wizard = true;
            state.first_run_step = 0;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Five-step setup: jurisdiction profile, units,\n"
                              "theme, demo project. Re-openable any time via\n"
                              "Help → Welcome wizard.");
        }
    }

    // Common workflows discovery card. Surfaces opinionated multi-
    // table recipes (FA24 swap today; future Stage1→2 step, E85
    // conversion) without burying them in Tools → Common Workflows
    // for users who haven't found the menu yet. The card itself is
    // pre-project-only here (welcome panel doesn't render once a
    // project is loaded) — clicking pre-project shows a guidance
    // toast, and the actionable surface for users WITH a project
    // loaded is the Tools menubar entry.
    //
    // Justification per analyst handoff 2026-06-07-fa24-swap-uiux-plan:
    // a user who just bolted in an FA24 lands on Welcome first and
    // shouldn't have to know the menubar exists to find the workflow.
    {
        ImGui::Dummy(ImVec2(0.0f, kSpaceL));
        constexpr float kCardW = 480.0f;
        center_cursor_x(kCardW);
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Common workflows");
        {
            ImVec2 const p = ImGui::GetCursorScreenPos();
            auto *const dl = ImGui::GetWindowDrawList();
            ImU32 const col = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + kCardW, p.y + 2.0f), col);
            ImGui::Dummy(ImVec2(kCardW, 4.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

        bool const fa24_ok = pack_supports_fa24_swap(state);
        ImGui::BeginDisabled(!fa24_ok);
        if (ImGui::Button("\xEE\xA2\xA8  FA24 swap (VA WRX)\xE2\x80\xA6",
                          ImVec2(kCardW, 32.0f))) {
            state.show_fa24_swap_modal = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (fa24_ok) {
                ImGui::SetTooltip(
                    "Guided 3-step recipe for the FA20→FA24 engine swap.\n"
                    "Applies Engine Displacement + HPFP-Timing + AVCS-Reference +\n"
                    "Injector-Mult edits atomically. Reversible via the status-bar\n"
                    "badge after Apply.");
            } else if (!state.project.has_value()) {
                ImGui::SetTooltip(
                    "Open a project first (Open Project or Recents above).\n"
                    "The workflow needs a loaded calibration pack to know\n"
                    "which tables to edit.");
            } else {
                ImGui::SetTooltip(
                    "This pack doesn't declare FA24-swap support.\n"
                    "Requires LF79101P / LF79103P / LF9L000E coverage.");
            }
        }

        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

        // TGV+EGR delete card. Off-road / track-only by analyst Task E
        // spec — gated at modal entry with explicit jurisdiction
        // confirmation. Surfaced here so users who own track cars find
        // it without menu-spelunking; the disabled state on packs that
        // don't declare the workflow tells daily-driver users why it's
        // not for them.
        bool const tgv_egr_ok = pack_supports_tgv_egr_delete(state);
        ImGui::BeginDisabled(!tgv_egr_ok);
        if (ImGui::Button("\xEE\xAB\x80  TGV + EGR delete (off-road only)\xE2\x80\xA6",
                          ImVec2(kCardW, 32.0f))) {
            state.show_tgv_egr_delete_modal = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (tgv_egr_ok) {
                ImGui::SetTooltip(
                    "Guided 3-step delete: zeros 5 EGR + TGV cal tables and\n"
                    "disables the P0400 DTC. Per analyst Tasks A-F (2026-06-09).\n"
                    "Jurisdiction-confirmation chip inside the modal; not for\n"
                    "daily-drivers on public roads. Reversible via the\n"
                    "status-bar badge after Apply.");
            } else if (!state.project.has_value()) {
                ImGui::SetTooltip(
                    "Open a project first. The workflow needs a loaded\n"
                    "calibration pack to know which tables to edit.");
            } else {
                ImGui::SetTooltip(
                    "This pack doesn't declare TGV+EGR-delete support.\n"
                    "Supported on 8 SH-2A packs (LF79103P + LF79101P inherited\n"
                    "+ 6 promoted siblings).");
            }
        }
        ImGui::EndGroup();
    }

    // Hardware-detection hint. Surfaces the AccessPort panel when an
    // AP3 is enumerated on USB so a first-time user discovers the
    // file-vault capability without menu-spelunking. Cheap polling
    // (~1 Hz) — render every frame, the detector caches internally.
    if (ap3_browser_should_hint(state)) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        constexpr float kHintW = 480.0f;
        center_cursor_x(kHintW);
        if (ImGui::Button("\xEE\xA2\x8C  AccessPort detected — open the AccessPort panel\xE2\x80\xA6",
                          ImVec2(kHintW, 32.0f))) {
            state.show_ap3_browser_panel = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Browse the AP's file vault (tunes / datalogs / presets /\n"
                "boot logo), pull historic logs, push staged tunes, and\n"
                "back up the whole device. See docs/34.");
        }
    }

    // Import .ptm hint. Always available — the cipher gate is checked
    // inside the modal; users without ST_ENABLE_COBB_AP_CIPHER=ON see
    // a clear PolicyDenied error there rather than failing silently.
    // Always-on (rather than first-run-only) because the import flow
    // is the headline path for tuners coming from COBB.
    {
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        constexpr float kHintW = 480.0f;
        center_cursor_x(kHintW);
        if (ImGui::Button("\xEE\x86\x97  Import .ptm tune file as project\xE2\x80\xA6",
                          ImVec2(kHintW, 32.0f))) {
            state.show_ptm_import_modal = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Decode a COBB AccessPort .ptm file into a SubuwuTuner project.\n"
                "Picks the .ptm, base ROM, and definition pack via NFD; runs the\n"
                "4-layer cipher chain (XTEA + base64 + AES + bzip2) and writes a\n"
                "Project::open-compatible skeleton. Loads the new project\n"
                "automatically. Requires ST_ENABLE_COBB_AP_CIPHER=ON at build.");
        }
    }

    // First-run-only pack hint. Answers "what do I need to start?"
    // without forcing the user to open the New Project modal first
    // to find out. Hidden once the user has any recents — they've
    // clearly figured out the flow by then.
    if (!has_recents) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceL));
        text_centered_subtle("First time? You'll need an ECU ROM dump + a definition pack.");
        if (!state.demo_project_path.has_value()) {
            // Only mention the demo as a follow-up hint when the
            // button itself isn't already on screen — otherwise the
            // text duplicates the CTA right above it.
            text_centered_subtle("The repo ships fixtures/demo.stune/ as a ready-to-open project.");
        } else {
            text_centered_subtle("Or click \"Try the demo project\" above to explore the UI now.");
        }
    }

    // Recents block. Empty list → render nothing here; first-run users
    // see the original clean welcome.
    if (has_recents) {
        // Lazily refresh the per-recent pack-lint cache so the chip
        // under each row stays in sync with the on-disk snapshot
        // without disk-thrashing every frame. A size mismatch =
        // recents was mutated (push/pin toggle, push_recent eviction);
        // refill the cache from disk on that frame only.
        if (state.recents_pack_lint.size() != state.recents.size()) {
            state.recents_pack_lint.assign(state.recents.size(), std::nullopt);
            for (std::size_t i = 0; i < state.recents.size(); ++i) {
                std::error_code ec;
                if (std::filesystem::exists(state.recents[i].path, ec)) {
                    state.recents_pack_lint[i] = load_pack_lint(state.recents[i].path);
                }
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 28.0f));
        // Centered "Recent projects" rule. We draw it inside a fixed-
        // width region so multiple windows / wide screens don't make
        // the list stretch oddly across the viewport.
        constexpr float kRowW = 480.0f;
        center_cursor_x(kRowW);
        ImGui::BeginGroup();
        // Heading: regular (not TextDisabled) so it reads as a
        // section break against the dimmed path text beneath each
        // row. The hand-drawn separator below is bounded to kRowW —
        // ImGui::Separator() ignores group width and would span the
        // whole panel, which looked broken on a maximized window.
        ImGui::TextUnformatted("Recent projects");
        {
            ImVec2 const p = ImGui::GetCursorScreenPos();
            auto *const dl = ImGui::GetWindowDrawList();
            ImU32 const col = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + kRowW, p.y + 2.0f), col);
            ImGui::Dummy(ImVec2(kRowW, 4.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

        // Filter input — only shown when there are more than a handful
        // of recents (else it's just noise). Substring-matched against
        // basename + full path, case-insensitive.
        constexpr std::size_t kRecentsFilterThreshold = 4;
        if (state.recents.size() >= kRecentsFilterThreshold) {
            ImGui::SetNextItemWidth(kRowW);
            ImGui::InputTextWithHint("##recents_filter",
                                     "Filter recents…",
                                     state.recents_filter,
                                     sizeof state.recents_filter);
            ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        }
        std::string_view const recents_filter{state.recents_filter};

        // Build visible-indices list for keyboard nav (filter-aware).
        // Pinned entries surface first (stable order — pin/unpin
        // doesn't shuffle within their group); unpinned entries
        // follow in LRU order. Matches the on-disk LIFO trim in
        // push_recent so the index never reorders without a deliberate
        // user gesture.
        std::vector<std::size_t> visible;
        visible.reserve(state.recents.size());
        auto const matches_filter = [&](std::size_t i) {
            auto const &e = state.recents[i];
            auto const basename =
                e.path.filename().empty() ? e.path.string() : e.path.filename().string();
            return recents_filter.empty() ||
                   icontains(basename, recents_filter) ||
                   icontains(e.path.string(), recents_filter);
        };
        for (std::size_t i = 0; i < state.recents.size(); ++i) {
            if (state.recents[i].pinned && matches_filter(i)) {
                visible.push_back(i);
            }
        }
        for (std::size_t i = 0; i < state.recents.size(); ++i) {
            if (!state.recents[i].pinned && matches_filter(i)) {
                visible.push_back(i);
            }
        }
        // Up/Down arrows walk the visible list when no input is active.
        // Enter opens the selected entry.
        if (!visible.empty() && !ImGui::GetIO().WantTextInput) {
            int const n = static_cast<int>(visible.size());
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                state.recents_selected_idx =
                    state.recents_selected_idx < 0 ? 0
                                                   : (state.recents_selected_idx + 1) % n;
            } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                state.recents_selected_idx =
                    state.recents_selected_idx < 0 ? n - 1
                                                   : (state.recents_selected_idx - 1 + n) % n;
            }
        }
        // Snapshot indices to act on — modifying recents inside the
        // iteration (via try_open_project) would invalidate iterators.
        std::optional<std::size_t> clicked_idx;
        if (state.recents_selected_idx >= 0 &&
            state.recents_selected_idx < static_cast<int>(visible.size()) &&
            !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            auto const sel = visible[static_cast<std::size_t>(state.recents_selected_idx)];
            // Gate kb-Enter on path existence — firing try_open_project on
            // a moved/deleted project's path is at best a quick toast and
            // at worst a long filesystem stall (UNC / disconnected drive).
            // Mouse clicks hit the same gate via BeginDisabled(!exists)
            // below; the row's per-frame exists() result drives both.
            std::error_code kb_ec;
            if (std::filesystem::exists(state.recents[sel].path, kb_ec)) {
                clicked_idx = sel;
            }
        }
        std::size_t shown = 0;
        // Walk the precomputed `visible` index permutation rather than
        // state.recents directly: pinned-first ordering must match the
        // keyboard-nav `visible` list exactly, otherwise arrow keys
        // would highlight a row that doesn't correspond to what got
        // rendered.
        std::optional<std::size_t> pin_toggle_idx; // captured for post-loop mutation
        // Click on a chip → re-validate that recent's pack. Captured
        // here for post-loop dispatch (project open + save_pack_lint
        // would invalidate the iteration if applied mid-loop).
        std::optional<std::size_t> revalidate_idx;
        // Click on the ✕ next to a missing recent → drop it from the
        // list. Captured here for post-loop dispatch so recents.erase
        // can't invalidate the visible-index walk.
        std::optional<std::size_t> remove_idx;
        for (std::size_t vidx = 0; vidx < visible.size(); ++vidx) {
            std::size_t const i = visible[vidx];
            auto const &e = state.recents[i];
            auto const basename =
                e.path.filename().empty() ? e.path.string() : e.path.filename().string();
            ++shown;
            std::error_code ec;
            bool const exists = std::filesystem::exists(e.path, ec);

            ImGui::PushID(static_cast<int>(i));
            // Highlight the row that arrow-key navigation last selected
            // so the user can see where Enter will land.
            bool const kb_selected =
                state.recents_selected_idx >= 0 &&
                state.recents_selected_idx < static_cast<int>(visible.size()) &&
                visible[static_cast<std::size_t>(state.recents_selected_idx)] == i;
            if (kb_selected) {
                push_primary_button_colors();
            }
            // Each row is a button with two-line content (basename on
            // top, dimmed full path beneath). Dead entries are
            // disabled — visible so the user knows the project moved
            // rather than silently dropped. Pin toggle sits on the
            // right edge — small width, share the row's vertical
            // budget so the layout stays one-line.
            float const button_left_x = ImGui::GetCursorPosX();
            constexpr float kPinW = 28.0f;
            constexpr float kRemoveW = 28.0f;
            // When the recent's path is missing, the row reserves room
            // for a trailing ✕ "remove from recents" button so the user
            // has a one-click escape from a dead entry. Without this the
            // entry is uncloseable except by editing recents.toml by hand.
            float const body_w = exists ? (kRowW - kPinW - 4.0f)
                                        : (kRowW - kPinW - kRemoveW - 8.0f);
            ImGui::BeginDisabled(!exists);
            // Decorate the visible label with a ★ glyph for pinned
            // entries so the user spots them at a glance. The button
            // id stays the path-derived ImGui::PushID so click handlers
            // don't conflict. Pinned star is colored with the brand
            // accent so it pops against the row's normal text — a
            // bare glyph reads as ornamental noise; tinted reads as
            // status.
            std::string display_label = e.pinned ? (std::string{"\xE2\x98\x85  "} + basename)
                                                 : basename;
            if (e.pinned) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      accent_for(current_theme()).base);
            }
            if (ImGui::Button(display_label.c_str(), ImVec2(body_w, 0.0f))) {
                clicked_idx = i;
            }
            ImGui::EndDisabled();
            if (e.pinned) {
                ImGui::PopStyleColor();
            }
            if (kb_selected) {
                pop_primary_button_colors();
            }
            // Row tooltip MUST be queried while the row button is still
            // the most-recent ImGui item — before the SameLine + pin
            // button shift `IsItemHovered` onto the pin glyph. Doing it
            // after `pop_primary_button_colors()` is safe; the pop only
            // restores style state.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (exists) {
                    ImGui::SetTooltip("%s\nOpened %s", e.path.string().c_str(),
                                      e.opened_at.c_str());
                } else {
                    ImGui::SetTooltip("%s\n\nPath no longer exists — the project may "
                                      "have moved.\nOpen Project… to locate it manually.",
                                      e.path.string().c_str());
                }
            }
            ImGui::SameLine(0.0f, 4.0f);
            // Pin/unpin toggle. ★ when pinned, ☆ when not. Always
            // enabled — the user might want to pin a project whose
            // file moved (un-pin to clean up the list).
            char const *pin_glyph = e.pinned ? "\xE2\x98\x85" : "\xE2\x98\x86";
            if (ImGui::Button(pin_glyph, ImVec2(kPinW, 0.0f))) {
                pin_toggle_idx = i;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(e.pinned ? "Unpin: this entry will roll off when\nolder than 8 unpinned recents."
                                           : "Pin: keep this entry at the top of\nthe recents list across sessions.");
            }
            // ✕ "remove from recents" affordance for dead entries. Only
            // rendered when the path is missing — for live entries the
            // user opens the project to interact with it, and a remove
            // button next to live rows is one click away from accidental
            // recents-list churn.
            if (!exists) {
                ImGui::SameLine(0.0f, 4.0f);
                if (ImGui::Button("\xE2\x9C\x95", ImVec2(kRemoveW, 0.0f))) {
                    remove_idx = i;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Remove from recents.\nThe project files (if any) stay on disk —\nthis only clears the entry from this list.");
                }
            }
            // Subtitle: dimmed full path + relative time, aligned under
            // the row. Center inside the button's kRowW span so the
            // subtitle shares the button's centerline; GetContentRegionAvail
            // here measures from the button's left edge to the panel's
            // right edge, which would land the subtitle off-center to the
            // right.
            std::string subtitle;
            if (exists) {
                subtitle = e.path.string();
                auto const rel = format_relative_time(e.opened_at);
                if (!rel.empty()) {
                    subtitle += "  ·  ";
                    subtitle += rel;
                }
            } else {
                subtitle = e.path.string() + "  (missing)";
            }
            float const text_w = ImGui::CalcTextSize(subtitle.c_str()).x;
            if (text_w < kRowW) {
                ImGui::SetCursorPosX(button_left_x + (kRowW - text_w) * 0.5f);
            } else {
                ImGui::SetCursorPosX(button_left_x);
            }
            text_subtle("%s", subtitle.c_str());
            // Pack-lint chip — pulls from the lazily-populated cache so
            // the welcome panel can answer "is this project's pack
            // well-formed?" without the user opening it first. Cache
            // misses cost one small TOML read per recent, only on the
            // first render after a recents change.
            if (exists && i < state.recents_pack_lint.size()) {
                auto const &snap = state.recents_pack_lint[i];
                if (snap.has_value()) {
                    char chip[160];
                    if (snap->status == 0) {
                        std::snprintf(chip, sizeof chip,
                                      "\xE2\x9C\x93  Pack OK  \xC2\xB7  %s",
                                      format_relative_time(snap->last_validated_at).c_str());
                    } else {
                        std::snprintf(chip, sizeof chip,
                                      "\xE2\x9C\x95  Pack: %d issue%s  \xC2\xB7  %s",
                                      snap->status,
                                      snap->status == 1 ? "" : "s",
                                      format_relative_time(snap->last_validated_at).c_str());
                    }
                    float const chip_w = ImGui::CalcTextSize(chip).x;
                    if (chip_w < kRowW) {
                        ImGui::SetCursorPosX(button_left_x + (kRowW - chip_w) * 0.5f);
                    } else {
                        ImGui::SetCursorPosX(button_left_x);
                    }
                    if (snap->status == 0) {
                        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
                    }
                    // Clickable chip — Selectable with PushStyleColor
                    // sets the text color, and dimmed Header /
                    // HeaderHovered colors keep the chip from looking
                    // like a sidebar selection cell. Sized exactly to
                    // the text so the click target hugs the visible
                    // chip glyph.
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                          ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                          ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
                    if (ImGui::Selectable(chip, false,
                                          ImGuiSelectableFlags_AllowOverlap,
                                          ImVec2(chip_w, 0.0f))) {
                        revalidate_idx = i;
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Click to re-run pack validation against\n"
                                          "this project's loaded pack. Updates the\n"
                                          "chip + persists to <project>/pack-lint.toml.");
                    }
                    ImGui::PopStyleColor();
                }
            }
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PopID();
        }
        if (!recents_filter.empty()) {
            text_subtle("Showing %zu of %zu recents.", shown, state.recents.size());
        }
        ImGui::EndGroup();

        if (pin_toggle_idx.has_value() && *pin_toggle_idx < state.recents.size()) {
            state.recents[*pin_toggle_idx].pinned =
                !state.recents[*pin_toggle_idx].pinned;
            save_recents(state.recents);
        }
        if (clicked_idx.has_value()) {
            // Capture by value: try_open_project mutates recents.
            auto const path = state.recents[*clicked_idx].path;
            // Belt-and-suspenders: re-check existence at dispatch time
            // even though BeginDisabled gated the per-row click and the
            // kb-Enter handler gated itself. ImGui disabled-button
            // semantics have wobbled across versions for keyboard
            // activation; a stale "exists" snapshot also vanishes
            // between exists() and dispatch on rare race timing. Either
            // way we never want to call try_open_project on a path that
            // demonstrably isn't there — Project::open's failure path
            // emits a toast but a long-stalling fs call (UNC,
            // disconnected drive) would freeze the UI in the meantime.
            std::error_code dispatch_ec;
            if (std::filesystem::exists(path, dispatch_ec)) {
                request_action(state, ConfirmAction::OpenRecent, path);
            } else {
                enqueue_toast(state, ToastKind::Warn,
                              std::string{"Project moved or deleted: "} +
                                  path.string() +
                                  "\nUse the ✕ button to remove it from recents.");
            }
        }
        if (remove_idx.has_value() && *remove_idx < state.recents.size()) {
            // Erase the dead entry and persist immediately. The pack-lint
            // parallel cache resizes itself on the next frame via the
            // recents_pack_lint.size() != state.recents.size() guard
            // higher up in this function, so we don't have to keep them
            // in sync here.
            auto const removed_basename = state.recents[*remove_idx].path.filename().string();
            state.recents.erase(state.recents.begin() +
                                static_cast<std::ptrdiff_t>(*remove_idx));
            save_recents(state.recents);
            // Keep keyboard nav from pointing at a now-out-of-range row
            // after the erase — easiest correct behavior is to reset
            // the cursor, which the next Down/Up key press snaps back
            // to the new index 0.
            state.recents_selected_idx = -1;
            enqueue_toast(state, ToastKind::Success,
                          std::string{"Removed '"} + removed_basename + "' from recents.");
        }
        if (revalidate_idx.has_value() && *revalidate_idx < state.recents.size()) {
            // Welcome-chip click → re-validate the project's pack
            // directly, without opening the project (heavy + would
            // swap the user's view). We need only the Definition;
            // open it via st::Project to honor the project.toml's
            // def_path resolution + drop the rest of the load.
            auto const path = state.recents[*revalidate_idx].path;
            auto proj = st::Project::open(path);
            if (proj.has_value()) {
                auto const v = proj->definition().validate();
                PackLintSnapshot snap;
                snap.pack_id = proj->definition().pack().id;
                snap.last_validated_at = iso8601_utc_now();
                if (v.has_value()) {
                    snap.status = 0;
                } else {
                    auto const msg = v.error().to_string();
                    int violations = msg.empty() ? 0 : 1;
                    for (char c : msg) {
                        if (c == '\n') ++violations;
                    }
                    snap.status = violations;
                    snap.message = msg;
                }
                save_pack_lint(proj->dir(), snap);
                // Refresh the parallel cache slot so the chip updates
                // this frame without waiting for a recents-size churn.
                state.recents_pack_lint[*revalidate_idx] = snap;
                // Explicit confirmation — without this, a silent
                // re-validate looks like the click did nothing,
                // especially when the pack was already OK and the
                // chip text doesn't visibly change.
                if (snap.status == 0) {
                    enqueue_toast(state, ToastKind::Success,
                                  "Pack '" + snap.pack_id + "' is valid.");
                } else {
                    enqueue_toast(state, ToastKind::Warn,
                                  "Pack '" + snap.pack_id + "' has " +
                                      std::to_string(snap.status) +
                                      (snap.status == 1 ? " issue."
                                                        : " issues."));
                }
            } else {
                // Project couldn't be opened (deleted, perm denied)
                // — surface a visible failure rather than no-op.
                enqueue_toast(state, ToastKind::Warn,
                              "Couldn't open the project to revalidate. "
                              "The directory may have moved.");
            }
        }
    }

    if (!state.status_msg.empty()) {
        ImGui::Dummy(ImVec2(0.0f, has_recents ? 16.0f : 32.0f));
        text_centered_subtle(state.status_msg.c_str());
    }

    // What's new — parsed from CHANGELOG.md's [Unreleased] section so
    // the welcome panel always shows the actual unreleased changelog
    // without anyone hand-curating the list. Falls back gracefully
    // when CHANGELOG.md isn't alongside the binary (packaged install
    // without docs, dev build run from an odd dir).
    if (!state.whats_new_loaded) {
        load_whats_new(state);
    }
    if (!state.whats_new.empty()) {
        ImGui::Dummy(ImVec2(0.0f, has_recents ? 22.0f : 28.0f));
        constexpr float kRowW = 480.0f;
        center_cursor_x(kRowW);
        ImGui::BeginGroup();
        // Renamed from "What's new" — the parsed source is CHANGELOG's
        // [Unreleased] section, so this is literally what's new in the
        // build currently running, not a forward-looking roadmap.
        // Spelling that out keeps the user from confusing it with
        // marketing copy.
        ImGui::TextUnformatted("New in this build");
        {
            ImVec2 const p = ImGui::GetCursorScreenPos();
            auto *const dl = ImGui::GetWindowDrawList();
            ImU32 const col = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + kRowW, p.y + 2.0f), col);
            ImGui::Dummy(ImVec2(kRowW, 4.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        // Cap to first N items so the welcome panel doesn't grow
        // unbounded when [Unreleased] balloons. CHANGELOG.md itself
        // remains the full reference.
        constexpr std::size_t kMaxWhatsNew = 6;
        std::string last_section;
        for (std::size_t k = 0; k < std::min(state.whats_new.size(), kMaxWhatsNew); ++k) {
            auto const &item = state.whats_new[k];
            if (item.section != last_section) {
                if (k > 0) {
                    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
                }
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      accent_for(current_theme()).base);
                ImGui::Text("%s", item.section.c_str());
                ImGui::PopStyleColor();
                last_section = item.section;
            }
            text_subtle("\xE2\x80\xA2  %s", item.body.c_str());
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }
        ImGui::EndGroup();
    }

    // Tip-of-the-launch — one rotating hint surfacing a non-obvious
    // affordance the user might not have discovered yet. Picked once
    // per process launch (static-init seeded from system_clock), so
    // a single welcome-panel visit shows the same tip but quitting
    // + relaunching cycles through them. Subtle text, no chrome —
    // sits between "What's new" and the footer.
    {
        static constexpr std::array<char const *, 10> kTips = {
            "Press Ctrl+K for the command palette — search every action, panel, and table.",
            "Press Ctrl+F to filter the table list — handy on packs with hundreds of tables.",
            "Right-click any table cell for Copy / Paste / Reset to Source.",
            "Toolbar buttons (+5%, -5%, Smooth, Interpolate) act on the current selection.",
            "Hover any table name in the sidebar for its address + dimensions.",
            "S badge = engine-safety-critical. E badge = emissions-relevant.",
            "Ctrl+Enter while editing a cell fills every selected cell with the typed value.",
            "Pass a .stune directory on the command line to skip the welcome screen.",
            "View \xE2\x86\x92 Theme to flip between Dark and Light. Same brand purple in both.",
            "View \xE2\x86\x92 Stats Panel for min / mean / max + a value histogram of the current table.",
        };
        // Mutable so the ↻ button below can advance it. Seeded once per
        // process from the wall clock so two cold starts in a row don't
        // show the same tip; clicking the cycle button walks forward
        // through the array in order from that seed.
        static std::size_t tip_idx = []() {
            auto const now = std::chrono::system_clock::now().time_since_epoch().count();
            return std::hash<long long>{}(now) % kTips.size();
        }();

        ImGui::Dummy(ImVec2(0.0f, has_recents ? 18.0f : kSpaceXL));
        constexpr float kTipW = 480.0f;
        center_cursor_x(kTipW);
        ImGui::BeginGroup();
        // "Tip:" prefix in brand-accent text, the tip itself in subtle —
        // the eye picks up the cue without the line shouting compared
        // to "What's new" above.
        auto const [accent, accent_hover, accent_active] = accent_for(current_theme());
        (void)accent_hover;
        (void)accent_active;
        ImGui::TextColored(accent, "Tip:");
        ImGui::SameLine();
        text_subtle("%s", kTips[tip_idx]);
        ImGui::SameLine();
        // Cycle button — frameless link-styled glyph, same affordance
        // language as the welcome footer's "Keyboard shortcuts" link.
        // Hand cursor on hover signals clickability since the visual
        // is text-only. PushID keeps the button's ImGui ID stable
        // across tip rotations (the visible label is constant).
        ImGui::PushID("tip_cycle");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
        // U+21BB ↻ — clockwise open-circle arrow. Compact, unambiguous,
        // matches the "another" affordance vocabulary users already know
        // from web UIs.
        if (ImGui::Button("\xE2\x86\xBB")) {
            tip_idx = (tip_idx + 1) % kTips.size();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Another tip");
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        ImGui::EndGroup();
    }

    // Footer: version + small Help shortcuts. Subtle enough to not
    // compete with the CTAs above, present enough to be discoverable.
    // Command-palette link is the highest-leverage navigation in the
    // app, so it gets a persistent footer slot — relying on the
    // rotating tip alone meant users had to get lucky to discover
    // Ctrl+K on their first visit.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXL));
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SubuwuTuner %.*s",
                      static_cast<int>(st::Version::string().size()), st::Version::string().data());
        float const sep_w = ImGui::CalcTextSize(" \xC2\xB7 ").x;
        float const text_w = ImGui::CalcTextSize(buf).x + sep_w +
                             ImGui::CalcTextSize("Command palette (Ctrl+K)").x +
                             sep_w + ImGui::CalcTextSize("Keyboard shortcuts").x;
        center_cursor_x(text_w);
        text_subtle("%s", buf);
        ImGui::SameLine();
        text_subtle(" \xC2\xB7 ");
        ImGui::SameLine();
        // Link-styled buttons — TextDisabled color, no frame. Reduces
        // visual weight while keeping them clickable and tab-reachable.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Button("Command palette (Ctrl+K)")) {
            open_command_palette(state);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Search every action, panel, and table in one input.");
        }
        ImGui::SameLine();
        text_subtle(" \xC2\xB7 ");
        ImGui::SameLine();
        if (ImGui::Button("Keyboard shortcuts")) {
            state.show_shortcuts_modal = true;
        }
        // It looks like text (no frame, no border) — signal that it
        // actually clicks by switching to the hand cursor on hover.
        // Same affordance the status-bar profile chip uses.
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }
}

} // namespace st::ui
