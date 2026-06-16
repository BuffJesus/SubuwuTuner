// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// About modal — name + version + brand rule, project facts table,
// bundled-library list. First move out of the main.cpp split: small,
// self-contained, no companion structs to ferry along.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/core/version.hpp"

#include <imgui.h>

#include <cstdio>

namespace st::ui {

void render_about_modal(AppState &state) {
    if (state.show_about_modal) {
        ImGui::OpenPopup("\xEE\xA5\x86  About SubuwuTuner##about_modal");
        state.show_about_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(440.0f, 0.0f), ImVec2(680.0f, 600.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA5\x86  About SubuwuTuner##about_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Title cluster — name + version + brand accent rule, mirrors the
    // welcome panel's typography so the "this is SubuwuTuner" voice is
    // consistent wherever it appears.
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_centered("SubuwuTuner", 1.6f);
    {
        constexpr float kRuleW = 48.0f;
        constexpr float kRuleH = 2.0f;
        center_cursor_x(kRuleW);
        ImVec2 const p = ImGui::GetCursorScreenPos();
        auto const a = accent_for(current_theme());
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImVec2(p.x + kRuleW, p.y + kRuleH),
            ImGui::GetColorU32(a.base), kRuleH * 0.5f);
        ImGui::Dummy(ImVec2(kRuleW, kRuleH + 4.0f));
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "v%.*s",
                      static_cast<int>(st::Version::string().size()),
                      st::Version::string().data());
        text_centered_subtle(buf);
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_centered_subtle(
        "A free, open-source Subaru ECU tuning suite in modern C++23.");

    ImGui::Dummy(ImVec2(0.0f, kSpaceL));
    ImGui::Separator();
    ImGui::Spacing();

    // Project facts — license, repo, build info. Two-column layout
    // (label / value) gives the eye a stable left rail to scan.
    if (ImGui::BeginTable("##about_facts", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        auto const row = [](char const *label, char const *value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            text_subtle("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value);
        };

        row("License",   "Apache-2.0 (see LICENSE)");
        row("Source",    "https://github.com/BuffJesus/SubuwuTuner");
        row("Built",     __DATE__ " " __TIME__);
        // Compiler name + version, formatted to match the GCC/MSVC
        // shape so the modal's value column stays a stable width.
        // __clang_version__ expands to a verbose string like
        // "19.1.0 (https://github.com/llvm/llvm-project ...)" that
        // would wrap awkwardly; major.minor.patch keeps parity.
#if defined(__clang__)
        {
            char clang[32];
            std::snprintf(clang, sizeof clang, "Clang %d.%d.%d",
                          __clang_major__, __clang_minor__, __clang_patchlevel__);
            row("Compiler", clang);
        }
#elif defined(__GNUC__)
        {
            char gcc[32];
            std::snprintf(gcc, sizeof gcc, "GCC %d.%d.%d",
                          __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
            row("Compiler", gcc);
        }
#elif defined(_MSC_VER)
        {
            char msvc[32];
            std::snprintf(msvc, sizeof msvc, "MSVC %d", _MSC_VER);
            row("Compiler", msvc);
        }
#else
        row("Compiler",  "unknown");
#endif
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_subtle("Bundled libraries");
    ImGui::BulletText("Dear ImGui — Omar Cornut + contributors (MIT)");
    ImGui::BulletText("ImPlot — Evan Pezent + contributors (MIT)");
    ImGui::BulletText("GLFW — Marcus Geelnard + Camilla Lowy (zlib/libpng)");
    ImGui::BulletText("nativefiledialog-extended — btzy + Frogtoss Games (zlib)");
    ImGui::BulletText("tomlplusplus — Mark Gillard (MIT)");
    ImGui::BulletText("Catch2 — Catch Org + contributors (BSL-1.0)");
    ImGui::BulletText("tl::expected — Sy Brand (CC0-1.0; Result<T> fallback)");
    ImGui::BulletText("libusb-1.0 — libusb contributors (LGPL-2.1 + linking exception)");
#ifdef ST_ETS_HAVE_CIPHER
    ImGui::BulletText("tiny-AES-c — kokke (public domain; .ptm cipher)");
    ImGui::BulletText("bzip2 1.0.8 — Julian Seward (BSD-style; .ptm cipher layer 4)");
#endif
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Full text of each license: THIRD_PARTY_NOTICES.md");

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Spacing();

    bool const close_clicked = ImGui::Button("Close", ImVec2(120.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dismiss.  (Esc)");
    }
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
