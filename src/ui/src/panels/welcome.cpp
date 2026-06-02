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

        // Snapshot indices to act on — modifying recents inside the
        // iteration (via try_open_project) would invalidate iterators.
        std::optional<std::size_t> clicked_idx;
        std::size_t shown = 0;
        for (std::size_t i = 0; i < state.recents.size(); ++i) {
            auto const &e = state.recents[i];
            auto const basename =
                e.path.filename().empty() ? e.path.string() : e.path.filename().string();
            if (!recents_filter.empty() &&
                !icontains(basename, recents_filter) &&
                !icontains(e.path.string(), recents_filter)) {
                continue;
            }
            ++shown;
            std::error_code ec;
            bool const exists = std::filesystem::exists(e.path, ec);

            ImGui::PushID(static_cast<int>(i));
            // Each row is a button with two-line content (basename on
            // top, dimmed full path beneath). Dead entries are
            // disabled — visible so the user knows the project moved
            // rather than silently dropped.
            float const button_left_x = ImGui::GetCursorPosX();
            ImGui::BeginDisabled(!exists);
            if (ImGui::Button(basename.c_str(), ImVec2(kRowW, 0.0f))) {
                clicked_idx = i;
            }
            ImGui::EndDisabled();

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
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PopID();
        }
        if (!recents_filter.empty()) {
            text_subtle("Showing %zu of %zu recents.", shown, state.recents.size());
        }
        ImGui::EndGroup();

        if (clicked_idx.has_value()) {
            // Capture by value: try_open_project mutates recents.
            auto const path = state.recents[*clicked_idx].path;
            request_action(state, ConfirmAction::OpenRecent, path);
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
        ImGui::TextUnformatted("What's new");
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

    // Footer: version + a small Help shortcut. Subtle enough to not
    // compete with the CTAs above, present enough to be discoverable.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXL));
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SubuwuTuner %.*s",
                      static_cast<int>(st::Version::string().size()), st::Version::string().data());
        float const text_w = ImGui::CalcTextSize(buf).x + ImGui::CalcTextSize(" \xC2\xB7 ").x +
                             ImGui::CalcTextSize("Keyboard shortcuts").x;
        center_cursor_x(text_w);
        text_subtle("%s", buf);
        ImGui::SameLine();
        text_subtle(" \xC2\xB7 ");
        ImGui::SameLine();
        // Render as a button styled to look like a link — TextDisabled
        // color, no frame. Reduces visual weight while keeping it
        // clickable and tab-reachable.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
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
