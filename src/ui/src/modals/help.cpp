// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// In-app help modal (analyst Issues #12 + #22). Two-pane sidebar/content
// layout over the markdown topics under docs/. Topic files are read
// once per session via resolve_docs_dir() and cached in
// state.help_topics; the modal renders the active topic's body inside
// a scrollable child window.
//
// Markdown rendering is intentionally minimal — headings render as
// distinct text styles (## bold, ### subtle) and pipe tables (the
// glossary's `| term | definition |` shape) render as ImGui tables so
// the glossary stays scannable. Anything else falls through as plain
// text. The point is to surface project docs without leaving the
// app, not to ship a full markdown engine.
//
// Uses the same plain ImGui::Begin + dim-overlay pattern the first-run
// wizard fix landed on (the BeginPopupModal Z-order pathology
// documented in 48d16e5 still applies for the same reasons).

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "persistence.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {

namespace {

// Markdown topic-files the help modal surfaces. Order = sidebar order.
// More can be added without code changes — the loader reads any file
// matching <docs_dir>/<filename>. Missing files are silently skipped
// (a partial install or a wall-cleaned tree is not a runtime error).
struct TopicSource {
    char const *filename;
    char const *fallback_title;
};
constexpr std::array<TopicSource, 3> kTopicSources = {{
    {"00-overview.md", "Overview"},
    {"10-glossary.md", "Glossary"},
    {"04-roadmap.md", "Roadmap"},
}};

std::string read_file_to_string(std::filesystem::path const &p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Pull the first markdown H1 (`# Title`) from a topic body. Falls back
// to the source's hardcoded label when no heading is present.
std::string title_from(std::string_view body, char const *fallback) {
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t line_end = body.find('\n', i);
        if (line_end == std::string_view::npos) {
            line_end = body.size();
        }
        std::string_view line{body.data() + i, line_end - i};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("# ")) {
            return std::string{line.substr(2)};
        }
        i = line_end + 1;
    }
    return fallback;
}

// Crude trim — strips leading/trailing whitespace from a string_view.
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// Parse the glossary markdown table into (term, definition) pairs. The
// glossary uses a single `| Term | Meaning |` table; rows before the
// separator and rows with the bold-markdown wrap (**term**) get
// normalized. Header + separator rows are dropped.
void parse_glossary(std::string_view body,
                    std::vector<std::pair<std::string, std::string>> &out) {
    out.clear();
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
        if (line.size() < 3 || line.front() != '|') {
            continue;
        }
        // Separator row (---|---|---).
        bool sep_only = true;
        for (char c : line) {
            if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') {
                sep_only = false;
                break;
            }
        }
        if (sep_only) {
            continue;
        }
        // Split on '|' — first and last cells are empty (border pipes).
        std::vector<std::string_view> cells;
        std::size_t start = 1;
        for (std::size_t j = 1; j < line.size(); ++j) {
            if (line[j] == '|') {
                cells.push_back(line.substr(start, j - start));
                start = j + 1;
            }
        }
        if (cells.size() < 2) {
            continue;
        }
        auto term = std::string{trim(cells[0])};
        auto defn = std::string{trim(cells[1])};
        // Strip ** markdown bold wrap on the term — glossary uses it.
        if (term.size() >= 4 && term.starts_with("**") && term.ends_with("**")) {
            term = term.substr(2, term.size() - 4);
        }
        // Drop the header row by checking for a "term"/"meaning"
        // lowercase match (case-insensitive); the actual data rows
        // start their term with a capital letter or acronym.
        std::string lower = term;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "term" || lower == "meaning") {
            continue;
        }
        if (term.empty() || defn.empty()) {
            continue;
        }
        out.emplace_back(std::move(term), std::move(defn));
    }
}

void load_help_topics(AppState &state) {
    state.help_topics.clear();
    state.help_error_msg.clear();
    state.glossary_terms.clear();
    state.glossary_loaded = false;
    if (!state.docs_dir.has_value()) {
        state.help_error_msg = "docs/ directory not found alongside the binary. "
                               "In dev tree, run from the build dir so docs/ "
                               "resolves up from <build>/bin.";
        state.help_loaded = true;
        state.glossary_loaded = true; // mark settled-empty so tooltip path stops retrying
        return;
    }
    for (auto const &src : kTopicSources) {
        auto const path = *state.docs_dir / src.filename;
        auto body = read_file_to_string(path);
        if (body.empty()) {
            continue;
        }
        AppState::HelpTopic t;
        t.id = src.filename;
        t.title = title_from(body, src.fallback_title);
        t.body = std::move(body);
        if (t.id == "10-glossary.md") {
            parse_glossary(t.body, state.glossary_terms);
            state.glossary_loaded = true;
        }
        state.help_topics.push_back(std::move(t));
    }
    // Ensure the flag flips even if the glossary file was missing —
    // the lazy-loader in widgets.cpp checks this to decide whether
    // to re-attempt the file read on every hover.
    state.glossary_loaded = true;
    if (state.help_active_topic >= static_cast<int>(state.help_topics.size())) {
        state.help_active_topic = 0;
    }
    state.help_loaded = true;
}

// Render a body of markdown text. Minimal: H1 / H2 / H3, bullet rows,
// pipe tables, plain text. No inline emphasis parsing — leaves **bold**
// literal because the glossary uses it as a term-emphasis cue and the
// raw text reads fine in the help pane.
void render_markdown(std::string_view body) {
    std::vector<std::string_view> table_rows;
    auto flush_table = [&]() {
        if (table_rows.empty()) {
            return;
        }
        // Find max column count across rows.
        auto split_cells = [](std::string_view line) {
            std::vector<std::string_view> cells;
            if (line.empty() || line.front() != '|') {
                return cells;
            }
            std::size_t start = 1;
            for (std::size_t i = 1; i < line.size(); ++i) {
                if (line[i] == '|') {
                    cells.push_back(line.substr(start, i - start));
                    start = i + 1;
                }
            }
            return cells;
        };
        int max_cols = 0;
        for (auto const &row : table_rows) {
            int const c = static_cast<int>(split_cells(row).size());
            if (c > max_cols) {
                max_cols = c;
            }
        }
        if (max_cols == 0) {
            table_rows.clear();
            return;
        }
        if (ImGui::BeginTable("##md_table", max_cols,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg)) {
            for (std::size_t r = 0; r < table_rows.size(); ++r) {
                auto const cells = split_cells(table_rows[r]);
                // Skip the separator row (---|---|...).
                bool sep_only = true;
                for (char c : table_rows[r]) {
                    if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') {
                        sep_only = false;
                        break;
                    }
                }
                if (sep_only) {
                    continue;
                }
                bool const is_header = (r == 0); // first non-sep row reads as header
                ImGui::TableNextRow(is_header ? ImGuiTableRowFlags_Headers : 0);
                for (int c = 0; c < max_cols; ++c) {
                    ImGui::TableNextColumn();
                    if (c < static_cast<int>(cells.size())) {
                        auto cell = trim(cells[static_cast<std::size_t>(c)]);
                        // Strip ** bold wrap for cleaner rendering.
                        std::string text{cell};
                        if (text.size() >= 4 && text.starts_with("**") && text.ends_with("**")) {
                            text = text.substr(2, text.size() - 4);
                        }
                        ImGui::TextWrapped("%s", text.c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
        table_rows.clear();
    };

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

        // Accumulate consecutive pipe-table rows.
        if (!line.empty() && line.front() == '|') {
            table_rows.push_back(line);
            continue;
        }
        // Non-table line — flush any pending table first.
        flush_table();

        if (line.empty()) {
            ImGui::Spacing();
            continue;
        }
        if (line.starts_with("# ")) {
            text_centered(std::string{line.substr(2)}.c_str(), 1.4f);
            ImGui::Separator();
            continue;
        }
        if (line.starts_with("## ")) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PushFont(ImGui::GetFont());
            ImGui::PushStyleColor(ImGuiCol_Text, accent_for(current_theme()).base);
            ImGui::TextUnformatted(line.data() + 3, line.data() + line.size());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Separator();
            continue;
        }
        if (line.starts_with("### ")) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
            ImGui::TextUnformatted(line.data() + 4, line.data() + line.size());
            continue;
        }
        if (line.starts_with("- ") || line.starts_with("* ")) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%.*s", static_cast<int>(line.size()) - 2, line.data() + 2);
            continue;
        }
        ImGui::TextWrapped("%.*s", static_cast<int>(line.size()), line.data());
    }
    flush_table();
}

bool topic_matches_filter(AppState::HelpTopic const &t, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    auto contains_ci = [](std::string_view hay, std::string_view needle) {
        auto lo = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        if (needle.size() > hay.size()) {
            return false;
        }
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                if (lo(hay[i + j]) != lo(needle[j])) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                return true;
            }
        }
        return false;
    };
    return contains_ci(t.title, filter) || contains_ci(t.body, filter);
}

} // namespace

void render_help_modal(AppState &state) {
    if (!state.show_help_modal) {
        return;
    }
    if (!state.help_loaded) {
        load_help_topics(state);
    }

    ImGuiViewport *vp = ImGui::GetMainViewport();

    // Dim overlay — same pattern as the wizard fix. Click outside
    // window doesn't dismiss (regular ImGui::Begin can't intercept
    // outside-clicks the way BeginPopupModal would); the X button +
    // Esc handle dismissal.
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.40f);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##help_dim_overlay", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoNavFocus |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Centered help window.
    ImVec2 const center = vp->GetCenter();
    ImVec2 const size{std::min(vp->Size.x - 80.0f, 1000.0f),
                      std::min(vp->Size.y - 80.0f, 720.0f)};
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowViewport(vp->ID);

    bool open = true;
    if (!ImGui::Begin("\xEE\xA4\xA8  SubuwuTuner — Help##help_modal", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.show_help_modal = false;
        ImGui::End();
        return;
    }

    if (!state.help_error_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.help_error_msg.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
            state.show_help_modal = false;
        }
        ImGui::End();
        return;
    }

    if (state.help_topics.empty()) {
        ImGui::TextUnformatted("No help topics loaded.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
            state.show_help_modal = false;
        }
        ImGui::End();
        return;
    }

    // Filter input — searches title + body for the active topic list.
    // Ctrl+F (when the help modal is the focused window) focuses this
    // input — standard "find in document" gesture so the user doesn't
    // have to click into the search box.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputTextWithHint("##help_filter", "Search topics… (Ctrl+F)",
                             state.help_filter, sizeof state.help_filter);
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(110.0f, 0.0f))) {
        state.help_loaded = false;
        load_help_topics(state);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-read docs from disk. Useful when iterating on\n"
                          "doc content during dev.");
    }
    ImGui::Separator();

    // Two-pane: sidebar + content.
    std::string_view const filter{state.help_filter};
    if (ImGui::BeginChild("##help_sidebar", ImVec2(240.0f, -1.0f), true)) {
        for (std::size_t i = 0; i < state.help_topics.size(); ++i) {
            auto const &t = state.help_topics[i];
            if (!topic_matches_filter(t, filter)) {
                continue;
            }
            bool const selected = (state.help_active_topic == static_cast<int>(i));
            if (ImGui::Selectable(t.title.c_str(), selected)) {
                state.help_active_topic = static_cast<int>(i);
            }
            text_subtle("%s", t.id.c_str());
            ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##help_content", ImVec2(0.0f, -1.0f), true)) {
        auto const idx = state.help_active_topic;
        if (idx >= 0 && idx < static_cast<int>(state.help_topics.size())) {
            render_markdown(state.help_topics[static_cast<std::size_t>(idx)].body);
        } else {
            text_subtle("Pick a topic from the sidebar.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace st::ui
