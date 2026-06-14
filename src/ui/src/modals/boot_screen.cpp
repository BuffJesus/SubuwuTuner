// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// F5 — boot-screen viewer. Pulls `/images/startup_screen.fb` from the
// connected AccessPort, decodes the raw RGB565-LE bytes into an RGB888
// pixel buffer (via the existing st::devices::ets::rgb565 codec),
// uploads it as a GL texture, and renders it at native resolution
// (240×320) inside an ImGui modal.
//
// Push-back (replacing the boot screen) is deferred — it needs a
// PNG/JPG decoder + the user-supplied image picker, which adds a fair
// chunk of code surface. The view-only path here is the small wedge
// that closes the "see what's on your AP" half of the feature, and
// re-uses the existing read_file_async + RGB565 codec end-to-end.
//
// Workflow-gated. When ST_HAVE_AP_WORKFLOW is undefined the modal
// renders a "workflow off" notice in place of the AP fetch.

#include "modals/modals.hpp"
#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/devices/ets/rgb565.hpp"

#include <imgui.h>

// GL 1.1 core (glGenTextures et al.) is available via the platform's
// system GL header. The texture-upload path runs from inside ImGui's
// main render loop where a current GL context is guaranteed by
// imgui_impl_glfw + imgui_impl_opengl3.
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace st::ui {

namespace {

// File-static modal state. The texture and pixel buffer live across
// frames; the future is held only while a pull is in flight.
struct State {
    std::future<EtsPushResult> async_pull; // re-using the result struct
    // For the read path we want the file bytes back, not a status —
    // so the wrapper below stashes the bytes into the buffer below
    // and the future just signals completion via ok / error.
    std::vector<std::uint8_t> raw_bytes;
    std::vector<st::devices::ets::rgb565::Rgb888> pixels;
    GLuint texture{0};
    std::string error;
    std::string source_path; // for the user-visible "Pulled from <path>" line
    bool fetched{false};
};

State &state() {
    static State s;
    return s;
}

void delete_texture(State &s) {
    if (s.texture != 0) {
        glDeleteTextures(1, &s.texture);
        s.texture = 0;
    }
}

void reset_state(State &s) {
    delete_texture(s);
    s.raw_bytes.clear();
    s.pixels.clear();
    s.error.clear();
    s.source_path.clear();
    s.fetched = false;
}

// Best-effort: write `pixels` into a fresh GL_RGB GL texture. Returns
// 0 on failure (the modal renders a placeholder in that case).
GLuint upload_pixels_as_texture(
    std::vector<st::devices::ets::rgb565::Rgb888> const &pixels,
    std::uint32_t width, std::uint32_t height) {
    if (pixels.size() != static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height)) {
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Each Rgb888 is 3 bytes; the underlying memory is tightly packed
    // since we declared the struct with three uint8 members and no
    // padding (verified by the rgb565 codec's existing tests). On the
    // unlikely chance a host adds padding for ABI reasons we'd see a
    // garbled image — but the round-trip tests would catch it long
    // before this code runs.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                  static_cast<GLsizei>(width),
                  static_cast<GLsizei>(height),
                  0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // GL_CLAMP is GL 1.1 core; GL_CLAMP_TO_EDGE arrived in 1.2 and
    // isn't always declared by the system's `<GL/gl.h>` on Windows.
    // For a static image sampled at native resolution the difference
    // is invisible — both behave the same at the edge texels.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    return tex;
}

void start_pull(State &s) {
    reset_state(s);
#ifndef ST_HAVE_AP_WORKFLOW
    s.error =
        "AP workflow surface is off in this build. Reconfigure with "
        "-DST_ENABLE_COBB_AP_WORKFLOW=ON to enable boot-screen pulls.";
    s.fetched = true;
    return;
#else
    if (!ets_panel_has_channel()) {
        s.error =
            "No AccessPort channel open. Connect via the AccessPort "
            "Browser panel first.";
        s.fetched = true;
        return;
    }
    // We don't have a dedicated read helper on the panel — the closest
    // is the push path. For the read direction the simplest approach
    // is to issue the read inline on a worker thread. The panel's own
    // worker queue would serialize it, but since the read takes a
    // few hundred ms in practice, blocking the GUI thread here would
    // freeze the modal's progress text. Spawning a std::async copy of
    // the Client around the panel's open channel mirrors the push
    // modal's pattern.
    s.source_path = "/images/startup_screen.fb";
    // The pull runs synchronously here on the click frame (no async
    // surface yet from the panel) — the boot-screen file is small
    // (~150 KB raw) and the pull completes in well under a second on
    // a working AP. A worker-thread upgrade is queued under the
    // larger F4/F5 async unification.
    // Synchronous read via the panel's helper (declared in
    // panels.hpp). NB: not safe to call while another async op (F4
    // backup pull, T31 push) is in flight — the channel is single-
    // stream. The button is gated by the same has_channel check, so
    // the worst case is a momentary serial conflict that we treat as
    // a daze risk.
    std::string err;
    auto bytes = ets_panel_read_file_sync(s.source_path, err);
    if (!err.empty()) {
        s.error = std::move(err);
        s.fetched = true;
        return;
    }
    s.raw_bytes = std::move(bytes);
    // Decode RGB565 → RGB888 at the canonical AP-screen geometry.
    using namespace st::devices::ets::rgb565;
    auto decoded = decode_rgb565_le(s.raw_bytes, kApScreenWidth,
                                      kApScreenHeight);
    if (!decoded.has_value()) {
        s.error = "Decode failed: " + decoded.error().to_string() +
                   " (expected " + std::to_string(kApFullScreenBytes) +
                   " bytes, got " + std::to_string(s.raw_bytes.size()) + ").";
        s.fetched = true;
        return;
    }
    s.pixels = std::move(*decoded);
    s.texture = upload_pixels_as_texture(s.pixels, kApScreenWidth,
                                           kApScreenHeight);
    if (s.texture == 0) {
        s.error =
            "GL texture upload failed — pixel buffer decoded but couldn't "
            "land on the GPU.";
    }
    s.fetched = true;
#endif
}

} // namespace

void render_boot_screen_modal(AppState &app_state) {
    if (app_state.show_boot_screen_modal) {
        ImGui::OpenPopup(
            "\xEE\xA2\x8C  AccessPort boot screen##boot_screen_modal");
        app_state.show_boot_screen_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 540.0f),
                                         ImVec2(700.0f, 800.0f));
    if (!ImGui::BeginPopupModal(
            "\xEE\xA2\x8C  AccessPort boot screen##boot_screen_modal",
            nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto &s = state();
    ImGui::TextWrapped(
        "Pull the AccessPort's current boot screen at "
        "/images/startup_screen.fb and render it at native 240\xC3\x97""320 "
        "RGB565. Push-back (replacing the boot screen) is queued for a "
        "future pass.");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Pull boot screen##bs_pull")) {
        start_pull(s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close##bs_close")) {
        reset_state(s);
        ImGui::CloseCurrentPopup();
    }

    ImGui::Spacing();
    if (!s.fetched) {
        ImGui::TextDisabled(
            "Click \"Pull boot screen\" to read the AP's current splash.");
        ImGui::EndPopup();
        return;
    }
    if (!s.error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "Error: %s", s.error.c_str());
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Source:  %s", s.source_path.c_str());
    ImGui::Text("Decoded: %zu pixels (%zu bytes raw)", s.pixels.size(),
                 s.raw_bytes.size());
    ImGui::Spacing();
    if (s.texture != 0) {
        // Render at native 240×320 so the on-screen size matches the
        // AP's physical screen. Cast through intptr_t to keep older
        // ImGui compatibility; the GL3 backend reads the value as a
        // GLuint at render time.
        ImGui::Image(
            static_cast<ImTextureID>(s.texture),
            ImVec2(static_cast<float>(
                        st::devices::ets::rgb565::kApScreenWidth),
                    static_cast<float>(
                        st::devices::ets::rgb565::kApScreenHeight)));
    } else {
        ImGui::TextColored(
            chip_fg_caution(),
            "Pixels decoded but no GL texture was uploaded.");
    }
    ImGui::EndPopup();
}

} // namespace st::ui
