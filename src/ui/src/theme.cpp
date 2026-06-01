// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Theme palettes + font load. The brand-purple accent triple flows
// from accent_for() into ImGui's button/tab/header colors via
// apply_theme(). g_current_theme is a file-local cache so widget
// helpers (chip palette etc.) can pick the right contrast variant
// without threading Theme through every call site — ImGui is single-
// threaded so no race.

#include "theme.hpp"

#include "persistence.hpp"

#include <imgui.h>

#include <filesystem>
#include <initializer_list>
#include <system_error>

namespace st::ui {

AccentTriple accent_for(Theme t) noexcept {
    constexpr ImVec4 accent_purple(0.55f, 0.35f, 0.85f, 1.00f);
    if (t == Theme::Light) {
        return {accent_purple, ImVec4(0.48f, 0.28f, 0.78f, 1.00f),
                ImVec4(0.40f, 0.20f, 0.70f, 1.00f)};
    }
    return {accent_purple, ImVec4(0.62f, 0.45f, 0.90f, 1.00f), ImVec4(0.70f, 0.55f, 0.95f, 1.00f)};
}

// File-local current-theme cache, updated by apply_theme() at the top
// of the function so any helper that needs theme-aware colors can read
// it without threading Theme through every call site. Chip palettes
// (chip_fg_warn etc) and badge tints use this; widget-internal ImGui
// style colors are still the source of truth for native widgets.
namespace {
Theme g_current_theme{Theme::Dark};
}

Theme current_theme() noexcept {
    return g_current_theme;
}

// True when the active theme renders against a light surface — useful
// for picking foreground colors that need to flip dark to stay legible.
bool theme_is_light() noexcept {
    return g_current_theme == Theme::Light;
}

namespace {

// ImGui's default glyph range is Basic Latin + Latin-1. The workspace
// rail uses ▦ (U+25A6 Geometric Shapes), ◈ (U+25C8 Geometric Shapes),
// ⚡ (U+26A1 Misc Symbols), and various typography callers reach into
// General Punctuation (·, …, → etc.) + Arrows. Without these ranges
// in the loaded atlas, the glyph slots resolve to '?' tofu. Build a
// merged range table once and reuse for every font load below.
ImWchar const *extended_glyph_ranges() {
    static ImWchar const ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
        0x2010, 0x2027, // General Punctuation (–, —, …, ·)
        0x2190, 0x21FF, // Arrows (→ ↔ ↻ ↩)
        0x2500, 0x257F, // Box Drawing
        0x2580, 0x259F, // Block Elements
        0x25A0, 0x25FF, // Geometric Shapes (▦ ◈ ●)
        0x2600, 0x26FF, // Miscellaneous Symbols (⚡ ⚠ ✓ ✗)
        0x2700, 0x27BF, // Dingbats (✔ ✘)
        0,
    };
    return ranges;
}

// Probe a few candidate paths and load the first one that exists. Returns
// nullptr if none was loadable, in which case ImGui's default font is used.
ImFont *load_first_existing(std::initializer_list<char const *> candidates, float size_px) {
    auto &io = ImGui::GetIO();
    for (auto const *path : candidates) {
        if (path == nullptr) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (auto *f = io.Fonts->AddFontFromFileTTF(path, size_px, nullptr,
                                                   extended_glyph_ranges());
            f != nullptr) {
            return f;
        }
    }
    return nullptr;
}

} // namespace

// Merge a system icon font into the main UI font atlas so callers can
// reach into the Windows icon set with private-use codepoints inline
// (workspace rail uses MDL2_EDIT / _LINE_CHART / _LIGHTNING etc.).
// MergeMode merges glyphs from this font into the previously-loaded
// font's atlas — they render as one continuous font from ImGui's
// perspective. Falls back silently when the system font isn't present
// (Mac/Linux today); cross-platform icon coverage is a follow-up that
// would bundle Lucide or FontAwesome under assets/fonts/.
void load_icon_font_merged(float size_px) {
    static ImWchar const icon_ranges[] = {
        0xE700, 0xF8FF, // MDL2 + Fluent icon glyphs (Private Use Area)
        0,
    };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    // Glyphs sit slightly tall against the body font baseline by default;
    // nudge them up so the centerline matches.
    cfg.GlyphMinAdvanceX = size_px; // make each icon square-cell
    cfg.GlyphOffset.y = 1.0f;

    auto &io = ImGui::GetIO();
    char const *const candidates[] = {
        "C:/Windows/Fonts/SegoeIcons.ttf", // Win 11 — Segoe Fluent Icons
        "C:/Windows/Fonts/segmdl2.ttf",    // Win 10 — Segoe MDL2 Assets
    };
    for (auto const *path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            continue;
        if (io.Fonts->AddFontFromFileTTF(path, size_px, &cfg, icon_ranges) != nullptr)
            return;
    }
}

Fonts load_fonts() {
    Fonts f;
    // UI font — sans for menus, panels, labels. Tries Inter from a bundled
    // assets/ dir first (drop in to get the polished look), then a sane
    // system font per platform, finally falls back to ImGui's default.
    f.ui = load_first_existing(
        {
            "assets/fonts/Inter-Regular.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        },
        15.0f);
    // Merge MDL2 / Fluent icons into the UI font atlas so panels can
    // use icon codepoints inline. Merged-mode add must follow the
    // primary AddFontFromFileTTF call.
    load_icon_font_merged(15.0f);

    // Mono — for grids, hex dumps, log output where alignment matters.
    f.mono = load_first_existing(
        {
            "assets/fonts/JetBrainsMono-Regular.ttf",
            "C:/Windows/Fonts/consola.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        },
        14.0f);

    if (f.ui == nullptr) {
        ImGui::GetIO().Fonts->AddFontDefault();
    }
    return f;
}

namespace {

// Custom dark palette tuned for long tuning sessions: high contrast for the
// numerical grids, low chroma so the chrome reads as neutral. Accent colour
// is the brand purple matching the welcome-panel accent rule + status-bar
// profile chip + tab-selected overline.
//
// Shape + sizing settings — identical between themes. Only the palette
// differs; pulling these out keeps apply_theme() short and avoids
// re-stating the layout twice.
void apply_style_shape(ImGuiStyle &s) {
    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(8.0f, 5.0f);
    s.CellPadding = ImVec2(6.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    s.IndentSpacing = 20.0f;
    s.ScrollbarSize = 14.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.DockingSeparatorSize = 1.0f;
}

void apply_palette_dark(ImGuiStyle &s) {
    auto const [accent, accent_hover, accent_active] = accent_for(Theme::Dark);

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.16f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.45f, 0.71f, 0.45f);

    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.30f, 0.35f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.34f, 0.39f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.38f, 0.42f, 0.49f, 1.00f);

    c[ImGuiCol_CheckMark] = accent_active;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_active;

    c[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accent_hover;
    c[ImGuiCol_SeparatorActive] = accent_active;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ResizeGripHovered] = accent_hover;
    c[ImGuiCol_ResizeGripActive] = accent_active;

    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered] = accent_hover;
    c[ImGuiCol_TabSelected] = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);

    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    c[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = accent_hover;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accent_hover;

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    c[ImGuiCol_NavCursor] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

void apply_palette_light(ImGuiStyle &s) {
    // Same purple accent as the dark theme — keeps brand identity
    // consistent across both modes. Hover/active go DARKER (toward
    // saturated indigo) on the light background so they don't wash
    // out, opposite of the dark-theme convention.
    auto const [accent, accent_hover, accent_active] = accent_for(Theme::Light);

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.93f, 0.94f, 0.96f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text] = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.42f, 0.62f, 0.83f, 0.45f);

    c[ImGuiCol_FrameBg] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.78f, 0.81f, 0.86f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.74f, 0.76f, 0.80f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.66f, 0.69f, 0.73f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.58f, 0.62f, 0.67f, 1.00f);

    c[ImGuiCol_CheckMark] = accent_active;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_active;

    c[ImGuiCol_Button] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accent_hover;
    c[ImGuiCol_SeparatorActive] = accent_active;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_ResizeGripHovered] = accent_hover;
    c[ImGuiCol_ResizeGripActive] = accent_active;

    c[ImGuiCol_Tab] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);

    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);

    c[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.42f, 0.46f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = accent_hover;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accent_hover;

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.84f, 0.87f, 0.92f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.00f, 0.00f, 0.00f, 0.04f);

    c[ImGuiCol_NavCursor] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.20f, 0.20f, 0.20f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

} // namespace

// NavCursor was previously `accent`, which made it render as a bright
// outline around whichever widget last received focus. Combined with
// viewports/docking, this manifested as "one cell is highlighted and
// never unhighlights" in the data grid — even after disabling
// NavEnableKeyboard, the cursor color was still being applied wherever
// the nav system happened to land. We don't ship any explicit
// nav-focus indicator, so both palettes zero out NavCursor (above).
//
// With viewports enabled, OS-level windows render with their own
// alpha; force fully-opaque WindowBg so detached panels don't show
// through to the desktop.
void apply_theme(Theme t) {
    // Cache for chip helpers (chip_fg_warn etc) that need theme-aware
    // colors without threading Theme through every call site. ImGui is
    // single-threaded so no race.
    g_current_theme = t;

    auto &s = ImGui::GetStyle();
    apply_style_shape(s);
    if (t == Theme::Light)
        apply_palette_light(s);
    else
        apply_palette_dark(s);
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        s.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
}

} // namespace st::ui
