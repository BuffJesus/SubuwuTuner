// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Widget primitives — centering / centered-text helpers, the empty-state
// composer, primary-action button-color stack, the chip palette + chip()
// renderer + preview pill, the case-insensitive substring helper, and
// the autotune lint-kind → fix-phrase mapping. Used everywhere; pulled
// here so future panel additions don't have to walk back to main.cpp
// for the shared draw primitives.

#include "widgets/widgets.hpp"

#include "app_state.hpp"
#include "theme.hpp"

#include "st/autotune.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {

// Map an autotune lint-kind enum to plain-language descriptive text
// for end-user display. The library-side `lint_kind_name` returns
// kebab-cased technical terms ("non-monotonic", "step discontinuity")
// useful for debug/CLI output but jargon-y for tuners. This wrapper
// produces a short fix-oriented phrase that explains the problem
// without requiring familiarity with the underlying enum.
char const *pretty_lint_kind(st::autotune::LintViolationKind kind) noexcept {
    using K = st::autotune::LintViolationKind;
    switch (kind) {
    case K::NonMonotonic:
        return "neighbor cells out of expected order";
    case K::StepDiscontinuity:
        return "neighbor-cell jump exceeds smoothness threshold";
    }
    return "lint violation";
}

// Case-insensitive substring search. Used by the sidebar table filter
// to match against both human-readable name and the snake_case id.
// Empty needle matches everything (so an empty filter shows the full
// list). ASCII-only — calibration table names use ASCII identifiers.
bool icontains(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty())
        return true;
    if (hay.size() < needle.size())
        return false;
    auto const eq = [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (!eq(static_cast<unsigned char>(hay[i + j]),
                    static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

// Center the current cursor's X for a piece of content of width `w` within
// the panel's current content region.
void center_cursor_x(float w) {
    float const avail = ImGui::GetContentRegionAvail().x;
    if (w < avail) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
    }
}

// Centered single-line text. Scale > 1.0 temporarily enlarges the font.
void text_centered(char const *text, float scale) {
    if (scale != 1.0f) {
        ImGui::SetWindowFontScale(scale);
    }
    center_cursor_x(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
    if (scale != 1.0f) {
        ImGui::SetWindowFontScale(1.0f);
    }
}

void text_centered_disabled(char const *text) {
    center_cursor_x(ImGui::CalcTextSize(text).x);
    ImGui::TextDisabled("%s", text);
}

// `TextDisabled` is the ImGui-canonical "this is inactive" color
// (~0.5 alpha, distinctly grey). It's the right choice for a
// disabled menu item or a button that won't fire. It's the WRONG
// choice for a subtitle or a piece of metadata — both want a
// muted-but-not-disabled look so the eye can still distinguish
// "kind of dim" (subtitle) from "actually inactive" (disabled).
// `text_subtle` fills that gap at ~0.65 alpha of the normal text
// color, regardless of theme. printf-style format string mirrors
// ImGui::Text / ImGui::TextDisabled so call sites can move over
// 1:1.
void text_subtle(char const *fmt, ...) {
    auto const c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.x, c.y, c.z, c.w * 0.65f));
    va_list args;
    va_start(args, fmt);
    // `fmt` is the format-attribute-checked parameter of this function;
    // the static check at the call site has already validated it. Apple
    // Clang flags the forward into ImGui::TextV anyway, so suppress
    // -Wformat-nonliteral just for this delegation.
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    ImGui::TextV(fmt, args);
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif
    va_end(args);
    ImGui::PopStyleColor();
}

void text_centered_subtle(char const *text) {
    center_cursor_x(ImGui::CalcTextSize(text).x);
    text_subtle("%s", text);
}

// Centered empty-state for panels that have no data to show. Title is
// regular weight (reads as a heading); hint is subtle (reads as a
// caption). Top padding lifts the cluster off the panel's top edge so
// it doesn't read as a terse one-liner stuck against the title bar.
// Use for "no project loaded", "select a table first", "pack declares
// no DTCs" — wherever a panel needs to acknowledge a void rather than
// look broken.
void render_empty_state(char const *title, char const *hint) {
    // Adaptive top padding — 24px floor for short panels, 10% of
    // available height on tall ones so the cluster doesn't pin to
    // the top edge with a sea of void below. Empirically the 10%
    // anchor lands the cluster at a comfortable upper-third on
    // 600-800px panels without feeling lost in 2000px maximized
    // viewports.
    float const avail_h = ImGui::GetContentRegionAvail().y;
    float const top_pad = std::max(24.0f, avail_h * 0.10f);
    ImGui::Dummy(ImVec2(0.0f, top_pad));
    text_centered(title, 1.0f);
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_centered_subtle(hint);
}

// Push the brand-accent triple onto ImGui's button color stack. Call
// in pairs around exactly one ImGui::Button to render it as the
// primary action — same purple the welcome panel accent rule + the
// status-bar profile chip + tab-selected overline use. Reads from
// current_theme() so dark/light renders the right contrast variant.
void push_primary_button_colors() {
    auto const a = accent_for(current_theme());
    ImGui::PushStyleColor(ImGuiCol_Button, a.base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, a.active);
}

void pop_primary_button_colors() {
    ImGui::PopStyleColor(3);
}

// Small framed "tag" used to highlight a per-table attribute (unit, safety
// flag, …) without it competing with the title. Looks like a button but
// stays purely visual: the return value is ignored and the hover/active
// states match the resting state.
void chip(char const *text, ImVec4 fg, ImVec4 bg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 2.0f));
    (void)ImGui::SmallButton(text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

// Common chip palettes — kept centrally so future flags pick from a small,
// coherent set rather than each call site rolling its own RGB. Each pair
// (fg, bg) is theme-aware: dark variants are pale-fg on dark-tint-bg, light
// variants flip to dark-fg on pale-tint-bg so contrast holds on white. The
// alpha on bg composites against WindowBg, so the chips read as tinted
// surfaces rather than slabs.
//
// Accent uses the brand purple from accent_for() — same purple as
// ButtonActive / HeaderActive / TabSelectedOverline / the welcome-panel
// accent rule, so when an "active" chip lights up it matches the rest of
// the active-element language.
ImVec4 chip_fg_accent() {
    return theme_is_light() ? ImVec4(0.32f, 0.18f, 0.55f, 1.0f)
                            : ImVec4(0.92f, 0.84f, 1.00f, 1.0f);
}
ImVec4 chip_bg_accent() {
    return theme_is_light() ? ImVec4(0.85f, 0.78f, 0.96f, 0.80f)
                            : ImVec4(0.32f, 0.20f, 0.50f, 0.55f);
}
ImVec4 chip_fg_warn() {
    return theme_is_light() ? ImVec4(0.55f, 0.32f, 0.05f, 1.0f)
                            : ImVec4(1.00f, 0.86f, 0.55f, 1.0f);
}
ImVec4 chip_bg_warn() {
    return theme_is_light() ? ImVec4(1.00f, 0.90f, 0.65f, 0.78f)
                            : ImVec4(0.42f, 0.30f, 0.08f, 0.60f);
}
ImVec4 chip_fg_caution() {
    // Pale-yellow on dark, dark-olive on light — same caution band as
    // chip_bg_caution. Prior to the inline → external-linkage flip, the
    // dark branch was a recursive call to chip_fg_caution() itself; -O3
    // elided the UB branch in release inline builds but real recursion
    // surfaced as a stack overflow once chip_fg_caution gained external
    // linkage (cross-TU calls from sidebar S/E badges).
    return theme_is_light() ? ImVec4(0.46f, 0.40f, 0.05f, 1.0f)
                            : ImVec4(1.00f, 0.92f, 0.60f, 1.0f);
}
ImVec4 chip_bg_caution() {
    return theme_is_light() ? ImVec4(1.00f, 0.96f, 0.70f, 0.75f)
                            : ImVec4(0.34f, 0.32f, 0.08f, 0.55f);
}
ImVec4 chip_fg_muted() {
    return theme_is_light() ? ImVec4(0.32f, 0.36f, 0.42f, 1.0f)
                            : ImVec4(0.78f, 0.80f, 0.82f, 1.0f);
}
ImVec4 chip_bg_muted() {
    return theme_is_light() ? ImVec4(0.82f, 0.85f, 0.90f, 0.75f)
                            : ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
}
ImVec4 chip_fg_ok() {
    return theme_is_light() ? ImVec4(0.10f, 0.40f, 0.15f, 1.0f)
                            : ImVec4(0.70f, 0.94f, 0.72f, 1.0f);
}
ImVec4 chip_bg_ok() {
    return theme_is_light() ? ImVec4(0.75f, 0.93f, 0.78f, 0.75f)
                            : ImVec4(0.14f, 0.34f, 0.18f, 0.55f);
}
ImVec4 chip_fg_danger() {
    return theme_is_light() ? ImVec4(0.58f, 0.10f, 0.10f, 1.0f)
                            : ImVec4(1.00f, 0.75f, 0.72f, 1.0f);
}
ImVec4 chip_bg_danger() {
    return theme_is_light() ? ImVec4(1.00f, 0.82f, 0.80f, 0.80f)
                            : ImVec4(0.46f, 0.18f, 0.18f, 0.60f);
}
// Info — neutral / informational accent in the blue band. Distinct
// from chip_accent (brand purple) so info-tier indicators don't read
// as "active selection". Used for non-error/non-warn callouts like
// chunk indicators, "preview" labels, etc.
ImVec4 chip_fg_info() {
    return theme_is_light() ? ImVec4(0.10f, 0.32f, 0.62f, 1.0f)
                            : ImVec4(0.55f, 0.82f, 1.00f, 1.0f);
}
ImVec4 chip_bg_info() {
    return theme_is_light() ? ImVec4(0.78f, 0.88f, 1.00f, 0.75f)
                            : ImVec4(0.14f, 0.26f, 0.42f, 0.55f);
}

// Small "Preview" chip rendered inside a panel header. Replaces the
// older "(Preview)" suffix on menu labels + window titles, which read
// as "this is broken" rather than "API may shift." Info-band palette
// (low-alpha blue) keeps it visually quiet next to the panel title
// while still signaling status at a glance. Call right after the
// panel's title text on the same line.
void preview_pill() { chip("Preview", chip_fg_info(), chip_bg_info()); }

bool typed_phrase_gate(char *buffer, std::size_t buffer_size,
                       char const *required_phrase, char const *id_suffix) {
    // Layout: instruction text with the phrase highlighted in caution,
    // input field, ✓/✗ marker. Caller positions the whole block.
    ImGui::TextUnformatted("Type the phrase to confirm:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
    ImGui::Text("%s", required_phrase);
    ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(-60.0f);
    ImGui::InputText(id_suffix, buffer, buffer_size);

    bool const matches = std::string_view{buffer} == std::string_view{required_phrase};
    ImGui::SameLine();
    if (matches) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
        ImGui::TextUnformatted("\xEE\x9C\xBE"); // E73E CheckMark
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_muted());
        ImGui::TextUnformatted("\xEE\x9C\x91"); // E711 Cancel
        ImGui::PopStyleColor();
    }
    return matches;
}

namespace {

// Strip ASCII whitespace from both ends of a string_view.
std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// Lazy-load the glossary from docs/10-glossary.md into state.
// Idempotent: returns immediately when already loaded. No-op when
// state.docs_dir wasn't resolved (packaged install without docs).
//
// Mirrors the parser in modals/help.cpp::parse_glossary — duplicated
// here because widgets/ can't depend on modals/. If glossary parsing
// ever grows non-trivial, extract to its own translation unit.
void ensure_glossary_loaded(AppState &state) {
    if (state.glossary_loaded) {
        return;
    }
    if (!state.docs_dir.has_value()) {
        // Mark loaded so we don't retry the resolve_docs_dir lookup
        // on every hover; the glossary just stays empty on this
        // install layout.
        state.glossary_loaded = true;
        return;
    }
    auto const path = *state.docs_dir / "10-glossary.md";
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        state.glossary_loaded = true;
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto const body = std::move(ss).str();
    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t line_end = body.find('\n', i);
        if (line_end == std::string::npos) {
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
        std::string term{trim_ws(cells[0])};
        std::string defn{trim_ws(cells[1])};
        if (term.size() >= 4 && term.starts_with("**") && term.ends_with("**")) {
            term = term.substr(2, term.size() - 4);
        }
        std::string lower = term;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "term" || lower == "meaning" || term.empty() || defn.empty()) {
            continue;
        }
        state.glossary_terms.emplace_back(std::move(term), std::move(defn));
    }
    state.glossary_loaded = true;
}

// Case-insensitive equality.
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

void preload_glossary(AppState &state) {
    ensure_glossary_loaded(state);
}

void glossary_tooltip_for(AppState &state, std::string_view term) {
    if (!ImGui::IsItemHovered()) {
        return;
    }
    if (term.empty()) {
        return;
    }
    ensure_glossary_loaded(state);
    for (auto const &[t, d] : state.glossary_terms) {
        if (ieq(t, term)) {
            ImGui::BeginTooltip();
            ImGui::PushStyleColor(ImGuiCol_Text, accent_for(current_theme()).base);
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::PushTextWrapPos(420.0f);
            ImGui::TextUnformatted(d.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
            return;
        }
    }
}

} // namespace st::ui
