// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Toast stack — bottom-right transient notifications above the status
// bar. enqueue_toast is the cross-cutting entry point (actions/modals
// call it); render_toasts owns the per-frame draw + click-to-dismiss.
// tick_status_msg + mirror_status_change live here too because the
// stderr mirror is conceptually part of the same "user-visible status"
// surface.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {

constexpr double kStatusMsgTtlSeconds = 5.0;

// Status-bar text + the Read-ROM-modal error string are both user-visible
// surfaces; mirror every change to stderr so the console window that
// opens alongside the GUI captures every user-visible status/error as a
// copy-pasteable line. Heuristic: classify as `[err]` when the text
// matches an error-flavored regex (Failed / Error / cannot / Invalid /
// missing / Reject — same set other error sites use), otherwise
// `[status]`. Both classifications land on stderr so the console
// captures the full timeline.
//
// Called from tick_status_msg() once per frame — cheap because the
// fprintf only fires when `current` differs from the shadow `prev`.
namespace {
void mirror_status_change(std::string const &current, std::string &prev,
                          char const *surface_label) {
    if (current == prev) {
        return;
    }
    prev = current;
    if (current.empty()) {
        return;
    }
    auto const looks_like_error = [&](std::string const &s) noexcept {
        constexpr std::string_view kErrorMarkers[] = {
            "Failed", "failed", "Error", "error", "cannot", "Cannot",
            "Invalid", "missing", "Reject", "reject",
        };
        for (auto const &m : kErrorMarkers) {
            if (s.find(m) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    char const *severity = looks_like_error(current) ? "err" : "status";
    std::fprintf(stderr, "[%s][%s] %s\n", severity, surface_label, current.c_str());
    std::fflush(stderr);
}
} // namespace

void tick_status_msg(AppState &state) {
    double const now = ImGui::GetTime();
    // Stderr-mirror every surface that carries a user-visible status
    // string, BEFORE the TTL pass — so the console captures the
    // message even if it's about to be auto-cleared this frame.
    // Both `status_msg` (the status bar) and `read_rom_error_msg`
    // (the Tools → Read ROM from Car modal) get the same treatment;
    // adding more surfaces is one line per new shadow field.
    bool const status_changed = state.status_msg != state.status_msg_prev;
    mirror_status_change(state.status_msg, state.status_msg_prev, "status-bar");
    mirror_status_change(state.read_rom_error_msg, state.read_rom_error_msg_prev, "read-rom");
    if (status_changed) {
        state.status_msg_seen_at = now;
        return;
    }
    if (!state.status_msg.empty() && now - state.status_msg_seen_at >= kStatusMsgTtlSeconds) {
        state.status_msg.clear();
        state.status_msg_prev.clear();
    }
}

namespace {

// Toast tuning. Lifetimes vary by kind — successes and info dismiss
// fast (a glance is enough), warn lingers, danger sticks the
// longest because the user usually needs to read it + decide what
// to do. Max stack caps a burst of notifications from tiling the
// screen — oldest drop off first.
constexpr std::chrono::milliseconds kToastFadeWindow{500};
constexpr std::size_t kToastMaxStack = 5;
constexpr float kToastWidth = 320.0f;
constexpr float kToastVerticalGap = 8.0f;

std::chrono::milliseconds toast_lifetime_for(ToastKind k) {
    switch (k) {
    case ToastKind::Danger:
        return std::chrono::milliseconds{8000};
    case ToastKind::Warn:
        return std::chrono::milliseconds{6000};
    case ToastKind::Success:
    case ToastKind::Info:
    default:
        return std::chrono::milliseconds{4000};
    }
}

// Toast colors by kind — composes the existing theme-aware chip
// palette so toasts inherit the same dark/light contrast story.
ImVec4 toast_fg(ToastKind k) {
    switch (k) {
    case ToastKind::Success:
        return chip_fg_ok();
    case ToastKind::Warn:
        return chip_fg_warn();
    case ToastKind::Danger:
        return chip_fg_danger();
    case ToastKind::Info:
    default:
        return chip_fg_info();
    }
}

ImVec4 toast_bg(ToastKind k) {
    switch (k) {
    case ToastKind::Success:
        return chip_bg_ok();
    case ToastKind::Warn:
        return chip_bg_warn();
    case ToastKind::Danger:
        return chip_bg_danger();
    case ToastKind::Info:
    default:
        return chip_bg_info();
    }
}

} // namespace

void enqueue_toast(AppState &state, ToastKind kind, std::string text) {
    state.toasts.push_back({
        std::move(text),
        kind,
        std::chrono::steady_clock::now() + toast_lifetime_for(kind),
    });
    // Drop oldest if we've exceeded the visible cap.
    if (state.toasts.size() > kToastMaxStack) {
        state.toasts.erase(state.toasts.begin());
    }
}

void render_toasts(AppState &state) {
    auto const now = std::chrono::steady_clock::now();

    // GC expired toasts first so we don't waste a frame's worth of
    // window setup on something we're about to drop.
    std::erase_if(state.toasts,
                  [&](Toast const &t) { return t.expires_at <= now; });
    if (state.toasts.empty()) {
        return;
    }

    auto const *const vp = ImGui::GetMainViewport();
    float const right_x = vp->WorkPos.x + vp->WorkSize.x;
    float const bottom_y =
        vp->WorkPos.y + vp->WorkSize.y - static_cast<float>(kStatusBarHeight);

    // Stack newest-at-bottom (closest to the status bar) and grow
    // upward. anchor=(0,1) means SetNextWindowPos specifies the
    // bottom-LEFT corner of the toast.
    float y_cursor = bottom_y - kToastVerticalGap;

    // Click-to-dismiss: collect indices to remove after the loop so
    // we don't invalidate iteration order while rendering. Index
    // is into the original toasts vector (newest at .back()).
    std::vector<std::size_t> dismiss_indices;

    for (std::size_t i = 0; i < state.toasts.size(); ++i) {
        // Iterate from end (newest) backward to front (oldest).
        std::size_t const toast_idx = state.toasts.size() - 1 - i;
        auto const &t = state.toasts[toast_idx];

        // Compute fade alpha — linear ramp during the last
        // kToastFadeWindow before expiry.
        auto const remaining = t.expires_at - now;
        float alpha = 1.0f;
        if (remaining < kToastFadeWindow) {
            float const num = std::chrono::duration<float>(remaining).count();
            float const den = std::chrono::duration<float>(kToastFadeWindow).count();
            alpha = std::clamp(num / den, 0.0f, 1.0f);
        }

        // Estimate height from wrapped-text size so the bottom anchor
        // lands consistently on the very first frame (otherwise
        // ImGui's auto-resize would briefly render at zero size).
        // 24px = 2*WindowPadding.y; the chip-style frame padding is
        // implicit in the WindowPadding chosen by the active style.
        ImVec2 const text_sz = ImGui::CalcTextSize(
            t.text.c_str(), nullptr, /*hide_text_after_hash=*/false,
            kToastWidth - 24.0f);
        float const toast_h = text_sz.y + 20.0f;

        ImGui::SetNextWindowPos(
            ImVec2(right_x - kToastWidth - kToastVerticalGap, y_cursor),
            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(kToastWidth, toast_h), ImGuiCond_Always);

        ImVec4 const bg = toast_bg(t.kind);
        ImVec4 const fg = toast_fg(t.kind);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.x, bg.y, bg.z, bg.w * alpha));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(fg.x, fg.y, fg.z, fg.w * alpha));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        char wid[32];
        std::snprintf(wid, sizeof wid, "##toast_%zu", i);
        ImGui::Begin(wid, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoDocking);
        // Body text wraps to leave room for the × close glyph in the
        // top-right corner. Close glyph is drawn manually so it lives
        // on the same y baseline as the first line of the message
        // regardless of how many lines the message wraps to.
        constexpr float kCloseW = 14.0f;
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - kCloseW);
        ImGui::TextWrapped("%s", t.text.c_str());
        ImGui::PopTextWrapPos();

        // Visible × in the top-right — affordance for click-to-dismiss.
        // Anchored to the toast's window edge so it stays in the same
        // place across single-line and multi-line toasts.
        {
            ImVec2 const win_pos = ImGui::GetWindowPos();
            ImVec2 const win_size = ImGui::GetWindowSize();
            ImVec2 const close_pos(win_pos.x + win_size.x - kCloseW - 4.0f,
                                   win_pos.y + 4.0f);
            // U+2715 ✕ — multiplication X. Lighter weight than 'x'
            // and aligns visually with the row baseline.
            ImU32 const col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(fg.x, fg.y, fg.z, fg.w * alpha));
            ImGui::GetWindowDrawList()->AddText(close_pos, col, "\xE2\x9C\x95");
        }

        // Click-to-dismiss: any click anywhere on the toast window
        // queues it for removal. The user might want to dismiss a
        // sticky danger toast they've already read; this avoids
        // waiting 8s for it to fade. The hover changes the cursor
        // to a pointer to signal interactivity.
        if (ImGui::IsWindowHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                dismiss_indices.push_back(toast_idx);
            }
        }

        ImGui::End();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        y_cursor -= toast_h + kToastVerticalGap;
    }

    // Apply dismissals in reverse so indices stay valid through the
    // erases. (Iterated newest-first above, so dismiss_indices is
    // already in descending order — but sort for safety.)
    if (!dismiss_indices.empty()) {
        std::sort(dismiss_indices.begin(), dismiss_indices.end(), std::greater<>{});
        for (auto idx : dismiss_indices) {
            if (idx < state.toasts.size()) {
                state.toasts.erase(state.toasts.begin() +
                                   static_cast<std::ptrdiff_t>(idx));
            }
        }
    }
}

} // namespace st::ui
