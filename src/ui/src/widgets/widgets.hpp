// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Shared widget primitives: spacing rhythm, text helpers, chip palette,
// empty-state, primary-action button stack, small string helpers.

#ifndef ST_UI_WIDGETS_HPP
#define ST_UI_WIDGETS_HPP

#include "st/autotune.hpp" // LintViolationKind for pretty_lint_kind

#include <imgui.h>

#include <cstddef>
#include <string_view>

namespace st::ui {

// Vertical-rhythm scale. Use these in `ImGui::Dummy({0, kSpaceX})`
// instead of raw pixel values so the rhythm stays consistent and a
// single edit can re-tune the whole app's whitespace.
inline constexpr float kSpaceXS = 4.0f;
inline constexpr float kSpaceS = 6.0f;
inline constexpr float kSpaceM = 10.0f;
inline constexpr float kSpaceL = 16.0f;
inline constexpr float kSpaceXL = 24.0f;

// Centering / centered-text helpers.
void center_cursor_x(float w);
void text_centered(char const *text, float scale = 1.0f);
void text_centered_disabled(char const *text);
void text_centered_subtle(char const *text);

// Clang defines __GNUC__ but rejects the gnu_printf archetype, so Clang uses
// the printf archetype. MinGW GCC needs gnu_printf to check against
// glibc-style conversions rather than the MSVC runtime's.
#if defined(__clang__)
[[gnu::format(printf, 1, 2)]]
#elif defined(__GNUC__)
[[gnu::format(gnu_printf, 1, 2)]]
#endif
void text_subtle(char const *fmt, ...);

// Empty-state — title + dimmed hint, centered in the current window.
void render_empty_state(char const *title, char const *hint);

// Primary-action accent button-color stack. Use in pairs around
// exactly one ImGui::Button() to render it as a modal's primary
// action (brand purple).
void push_primary_button_colors();
void pop_primary_button_colors();

// Chip — small inline pill rendered with foreground + background tint.
void chip(char const *text, ImVec4 fg, ImVec4 bg);

// Theme-aware chip palette. Fg shades pair with their bg counterpart.
ImVec4 chip_fg_accent();
ImVec4 chip_bg_accent();
ImVec4 chip_fg_warn();
ImVec4 chip_bg_warn();
ImVec4 chip_fg_caution();
ImVec4 chip_bg_caution();
ImVec4 chip_fg_muted();
ImVec4 chip_bg_muted();
ImVec4 chip_fg_ok();
ImVec4 chip_bg_ok();
ImVec4 chip_fg_danger();
ImVec4 chip_bg_danger();
ImVec4 chip_fg_info();
ImVec4 chip_bg_info();

// "Preview" pill — used by autotune ledger headings + similar.
void preview_pill();

// Typed-phrase confirmation gate (analyst Issue #14). Renders an
// input field labeled with the required phrase and returns true iff
// the buffer contents match exactly. Modals gate their primary
// destructive action on this so the user has to actively type
// (e.g. "YES FLASH"), not just click. Closes the most common
// click-through-fatigue failure mode for irreversible operations.
//
// Returns true only on exact case-sensitive match. Empty input
// returns false. The widget renders inline (no Begin/End); the
// caller controls placement.
[[nodiscard]] bool typed_phrase_gate(char *buffer, std::size_t buffer_size,
                                     char const *required_phrase,
                                     char const *id_suffix = "##typed_phrase");

// Case-insensitive substring contains. Used by the sidebar + DTC + history
// filters; in widgets/ so future panels can reuse without a copy-paste.
bool icontains(std::string_view hay, std::string_view needle) noexcept;

// Glossary tooltip popover (analyst Issue #22). Call after a text /
// label item that might describe a tuning term. When the user hovers
// the previous item AND the term matches an entry in the project's
// glossary (parsed lazily from <docs_dir>/10-glossary.md), pops a
// markdown-rendered definition as an ImGui tooltip. Silent no-op if
// the term isn't in the glossary or docs aren't located. Defined in
// widgets.cpp; declared here in widgets.hpp because the AppState-
// dependent helper would otherwise pull app_state.hpp into every
// translation unit.
struct AppState;
void glossary_tooltip_for(AppState &state, std::string_view term);

// Eagerly parse the glossary so the first hover doesn't pay a TOML
// read latency. Idempotent and cheap (a few hundred bytes of disk
// I/O); call once at app startup after state.docs_dir is resolved.
// No-op when docs_dir wasn't located.
void preload_glossary(AppState &state);

// Map an autotune lint-kind enum to a fix-oriented phrase for end-user
// display. Library-side `lint_kind_name` returns kebab-cased technical
// terms; this wrapper produces something the tuner can act on.
[[nodiscard]] char const *pretty_lint_kind(st::autotune::LintViolationKind kind) noexcept;

} // namespace st::ui

#endif
