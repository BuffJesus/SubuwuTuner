// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Theme palette + font load. Fonts sit next to theme because font
// substitution + size choice runs against the theme metric.

#ifndef ST_UI_THEME_HPP
#define ST_UI_THEME_HPP

#include "persistence.hpp" // Theme

#include <imgui.h>

namespace st::ui {

// Font set loaded once at startup. Passed by const-ref into the table
// view (mono for grids, ui for chrome). Not owned by AppState — fonts
// outlive every frame and have no per-session state.
struct Fonts {
    ImFont *ui = nullptr;   // Sans for UI chrome (menus, labels, panels)
    ImFont *mono = nullptr; // Monospace for grids, hex, log output
};

// Accent triple — the three shades used everywhere the GUI emphasises
// a primary action (modal "Create" / "Apply" buttons, ButtonActive,
// HeaderActive, SliderGrabActive, etc.). Per-theme so contrast with
// the surrounding palette stays right.
struct AccentTriple {
    ImVec4 base;
    ImVec4 hover;
    ImVec4 active;
};

AccentTriple accent_for(Theme t) noexcept;
Theme current_theme() noexcept;
bool theme_is_light() noexcept;

Fonts load_fonts();
void apply_theme(Theme t);

} // namespace st::ui

#endif
