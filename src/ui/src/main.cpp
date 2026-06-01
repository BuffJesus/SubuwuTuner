// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// subuwutuner-gui — Dear ImGui front-end.
//
// Opens a .stune project passed as argv[1], renders a dockable sidebar of
// tables, and shows the selected table as a read-only grid. Editing,
// project-management menus, and file-open dialogs land in follow-ups; this
// pass lays the polish foundation: docking + viewports, tuned palette,
// system-font probing.

#include "st/autotune.hpp"
#include "st/core/version.hpp"
#include "st/edit.hpp"
#include "st/feature.hpp"
#include "st/flash.hpp"
#include "st/log/adaptive_history.hpp"
#include "st/log/coldstart.hpp"
#include "st/log/ebcs.hpp"
#include "st/log/knock_dashboard.hpp"
#include "st/policy.hpp"
#include "st/config.hpp"
#include "st/defs/pack_registry.hpp"
#include "st/project.hpp"
#include "st/transport/factory.hpp"
#include "st/transport/mock.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/uds_trace.hpp"

#include "icon_data.hpp"

// Shared UI headers — checkpoint 1 of the main.cpp split. Struct/enum
// definitions live here; function implementations are still in this
// file until the per-modal / per-panel moves land.
#include "app_state.hpp"
#include "persistence.hpp"
#include "theme.hpp"
#include "actions.hpp"
#include "project_io.hpp"
#include "widgets/widgets.hpp"
#include "widgets/adapter_picker.hpp"
#include "modals/modals.hpp"
#include "panels/panels.hpp"

// ImGui + backends.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // DockBuilder*
#include <implot.h>
#include <ios>
#include <limits>
#include <memory>
#include <nfd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Everything below is wrapped in `namespace st::ui { ... }` for
// checkpoint 1 of the split. Header decls (in app_state.hpp etc.) are
// at st::ui scope; main.cpp's function bodies sit directly in st::ui
// so they satisfy those decls with external linkage. The only inner
// anonymous namespace kept is the one around g_current_theme (a true
// file-local cache variable). Per-file anon namespaces come back when
// each modal / panel moves to its own .cpp.

namespace st::ui {

// Widget primitives moved to widgets/widgets.cpp (decls in widgets/widgets.hpp).
// Toast helpers moved to panels/toasts.cpp (decls in panels/panels.hpp).

// ---------------------------------------------------------------------
// Recents — one-line-per-entry config persisted between cold starts.
// Lives next to other user-config in the OS-conventional location:
//   Windows: %LOCALAPPDATA%\SubuwuTuner\recents.txt
//   Mac:     ~/Library/Application Support/SubuwuTuner/recents.txt
//   Linux:   $XDG_CONFIG_HOME/subuwutuner/recents.txt
//            (fallback: $HOME/.config/subuwutuner/recents.txt)
//
// Format: one entry per line, "<ISO-8601 UTC>\t<absolute path>".
// Cap at 8 entries; most recent first. A malformed line is silently
// skipped — recents are a convenience, not a source of truth.
// ---------------------------------------------------------------------


// Selection, TableViewMode, WorkspaceMode, ConfirmAction, ToastKind,
// Toast, AdapterPickerState moved to app_state.hpp.

// Adapter picker helpers (render_adapter_picker / adapter_picker_to_spec /
// adapter_is_trace_mode) stay file-local for now — their bodies use ImGui +
// transport-factory details. Header is widgets/adapter_picker.hpp.

// AppState moved to app_state.hpp.
//
// The three methods that have non-trivial bodies (try_open_project,
// AppState methods moved to app_state.cpp.
// project-IO dialogs (open/save/csv) moved to project_io.cpp.
// actions (apply_parsed_csv_edits, apply_history_step, paste, reset,
// undo/redo, execute/request_action, copy_rect_to_clipboard, parse_tsv)
// moved to actions.cpp.


// render_unsaved_modal moved to modals/unsaved.cpp (along with
// modal_save_label, modal_discard_label, modal_subtitle as file-local).
// render_csv_import_modal moved to modals/csv_import.cpp.
// render_maf_autotune_modal moved to modals/autotune_maf.cpp (along
// with run_maf_autotune_preview + apply_maf_autotune_proposal helpers).
// render_kp_autotune_modal moved to modals/autotune_knock.cpp (along
// with run_knock_pull_preview + apply_knock_pull_proposal helpers).
// render_flash_modal moved to modals/flash.cpp (along with PendingFlash
// + build_pending_flash as file-local).
// render_shortcuts_modal moved to modals/shortcuts.cpp (along with
// ShortcutRow, ShortcutGroup, shortcuts_reference as file-local).
// render_about_modal moved to modals/about.cpp.
// render_new_project_modal moved to modals/new_project.cpp.

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}


// Case-insensitive substring search. Used by the sidebar table filter
// to match against both human-readable name and the snake_case id.
// Empty needle matches everything (so an empty filter shows the full
// list). ASCII-only — calibration table names use ASCII identifiers.


// ===========================================================================
// Read-ROM-from-car modal
// ===========================================================================
//
// Renders the Tools → Read ROM from Car flow. State machine drives the body:
// Idle (form) → Running (progress) → Done (save dialog) / Failed (error) /
// Cancelled (status note). All long-running work runs on `state.read_rom_worker`;
// the modal just polls atomics + status enum on every frame.
//
// ----- v1.1 Write-ROM plan (NOT IMPLEMENTED — design notes only) -----
//
// The flash side of this same UX is meaningfully more dangerous than the
// read side, and the existing render_flash_modal already covers most of
// it for file-based flashing. The "Write ROM to Car" GUI button would be
// a thin wrapper that:
//
//   1. Routes through the same render_flash_modal policy gate (engine-
//      safety hard-stop + emissions confirmation + tuner-reason field).
//   2. Adds a "Pick adapter" sub-form identical to this read modal's
//      (transport kind + device/DLL path) — could literally share the
//      ImGui code via a render_adapter_picker(state) helper.
//   3. Adds a brick-protection confirmation (docs/05 §4): battery > 12.0V
//      via a UDS PID poll, ignition state check, etc.
//   4. Background-threads Flasher::execute(plan), with the same atomic
//      progress + cancel pattern this read modal uses. Flasher::execute
//      already emits a structured FlashReport per step — surface that
//      as a per-sector ledger in the modal body.
//   5. Verify pass (the existing post-erase-write loop in execute()
//      already does this). Failure path: keep partial-flash report
//      visible + offer "Resume from journal" if applicable.
//
// Scope to defer:
//   - Resume-from-journal UI is its own modal/dialog.
//   - Vehicle-state precondition checks (battery, ignition, transmission
//      in N/P) need new transport calls + a "preflight checks passed"
//      panel. Hardware-gated — wire after OBDX arrives.
// render_settings_modal moved to modals/settings.cpp.
// render_def_registry_modal moved to modals/def_registry.cpp.
// render_read_rom_modal moved to modals/read_rom.cpp.
// open_command_palette + render_command_palette moved to panels/command_palette.cpp
// (PaletteCommand / PaletteCommandKind / build_palette_commands /
// dispatch_palette_command are file-local there).

// render_menubar moved to panels/menubar.cpp.
// render_sidebar moved to panels/sidebar.cpp.
// render_workspace_rail + render_dockspace_host + apply_workspace_mode
// + build_workspace_layout moved to panels/workspace.cpp (g_request_dock_reset
// is now file-local there; request_layout_reset is exposed via panels.hpp).


// render_welcome_panel moved to panels/welcome.cpp.
// render_stats_panel moved to panels/stats.cpp.
// render_knock_dashboard_panel moved to panels/knock_dashboard.cpp.
// render_adaptive_history_panel moved to panels/adaptive_history.cpp.
// render_coldstart_panel moved to panels/coldstart.cpp.
// render_ebcs_panel moved to panels/ebcs.cpp.
// render_dtcs_panel moved to panels/dtcs.cpp.
// render_history_panel moved to panels/history.cpp.
// render_features_designer moved to panels/features_designer.cpp.
// render_table_view moved to panels/table_view.cpp (along with
// GridStats, compute_stats, heatmap_color, text_right_aligned,
// render_table_heatmap, render_table_grid as file-local helpers).


// Status-bar TTL pass. Called once per frame before render_status_bar
// to fade out stale transient messages. Detects "new message" via a
// shadow string + per-message timestamp; after ~5s of the same
// content, clears it. Call sites just write to state.status_msg — no
// per-callsite TTL bookkeeping. Edge cases:
//  - Two consecutive identical messages (e.g. "Saved." twice) only
//    reset the timer on STRING change. That's the right trade-off:
//    rapid duplicate writes are usually the same event, and the
//    alternative (timer reset on every frame the string is set) would
//    pin the message forever.
//  - Time source is ImGui::GetTime() (wall-clock seconds since app
//    start, monotonic). No allocations, no calls outside ImGui.

// Render-frame callable shared between the main loop and GLFW's window-refresh
// callback. Windows enters a modal resize loop while the user drags a border;
// the main thread is blocked inside glfwPollEvents for the entire drag, so the
// only way to keep painting is to render from inside the WM_PAINT-driven
// refresh callback that GLFW dispatches during the modal loop.
//
// Lifted to outer st::ui scope so main() (at global scope) can address it
// after the anon namespace closes.
std::function<void()> g_render_frame;

} // namespace st::ui

int main(int argc, char *argv[]) {
    using namespace st::ui;
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == 0) {
        std::fprintf(stderr, "subuwutuner-gui: glfwInit failed\n");
        return 1;
    }

    // Stick to OpenGL 3.0 core-ish — the lowest target ImGui supports cleanly
    // across Win/Mac/Linux without needing extension-loader gymnastics.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow *window = glfwCreateWindow(1400, 880, "SubuwuTuner", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "subuwutuner-gui: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Set the window title-bar icon from the compiled-in RGBA blob
    // (icon_data.hpp, regenerated by scripts/embed_icon.py from
    // assets/icon.png). GLFW takes ownership of nothing — pixels stay
    // in our static .rodata segment. The Windows Explorer / taskbar
    // EXE icon is set separately via subuwutuner.rc + windres in
    // src/ui/CMakeLists.txt; both originate from the same PNG.
    {
        GLFWimage icon_image{};
        icon_image.width = st::ui::icon::kWidth;
        icon_image.height = st::ui::icon::kHeight;
        icon_image.pixels = const_cast<unsigned char *>(st::ui::icon::kRgba);
        glfwSetWindowIcon(window, 1, &icon_image);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto &io = ImGui::GetIO();
    // Deliberately NOT setting ImGuiConfigFlags_NavEnableKeyboard. With
    // it on, ImGui auto-focuses the first focusable widget in each
    // window and draws a permanent nav-focus rectangle around it —
    // which reads as "one cell is highlighted and never unhighlights"
    // in the data grid. Tab-cycling through hundreds of cells is not
    // a workflow this app targets, and every actual keyboard nav we
    // care about (arrow keys in the grid, Ctrl+F focus on the filter,
    // Enter/Esc in modals, Ctrl+{O,S,Z,Y,Q}) is handled explicitly via
    // IsKeyPressed and works without the nav system being on.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Force a tab bar on every docked window — even alone-in-node ones.
    // Without this, ImGui auto-hides the tab on single-window nodes and
    // there's no visible drag handle, so the panel can't be undocked or
    // moved by dragging once it's settled.
    io.ConfigDockingAlwaysTabBar = true;

    Fonts const fonts = load_fonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    state.recents = load_recents();
    state.settings = load_settings();
    // Best-effort lookup of the bundled fixtures/demo.stune. Welcome
    // panel renders a "Try the demo project" button when set; absent
    // in installs that didn't ship the demo (the call returns
    // nullopt and the button just doesn't render).
    state.demo_project_path = resolve_demo_project_path(argc >= 1 ? argv[0] : nullptr);
    // Apply the persisted theme before any user-visible frame renders.
    apply_theme(state.settings.theme);
    std::string_view const arg1 = (argc >= 2) ? argv[1] : "";
    if (arg1 == "-h" || arg1 == "--help" || arg1 == "/?") {
        std::fputs("Usage: subuwutuner-gui [PROJECT.stune]\n"
                   "  Open the optional .stune project on launch. Without an "
                   "argument, the GUI starts on the welcome panel.\n",
                   stderr);
        return 0;
    }
    if (!arg1.empty()) {
        state.try_open_project(argv[1]);
    } else {
        state.status_msg = "Open a .stune project: File → Open (Ctrl+O).";
    }

    auto render_frame = [&]() {
        // The user clicked the window's close button (or any other
        // path that toggled GLFW's close flag). Route it through the
        // same confirm-action machinery as Ctrl+Q / File → Quit so an
        // accidental click on the X doesn't lose unsaved edits. The
        // GLFW flag is reset so request_action's confirm path can
        // decide whether to actually quit.
        if (glfwWindowShouldClose(window) != 0) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            request_action(state, ConfirmAction::Quit);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Global keyboard shortcuts. IsKeyChordPressed is mod-aware, so
        // Ctrl+Shift+Z and Ctrl+Z don't collide.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) {
            request_action(state, ConfirmAction::Quit);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
            request_action(state, ConfirmAction::OpenDialog);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            save_project(state);
        }
        // Ctrl+F focuses the sidebar's table filter. Only meaningful
        // when a project is open; harmless otherwise (the next
        // render_sidebar call will reset the flag without acting on
        // it).
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
            state.focus_table_filter = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
            do_redo(state);
        } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            do_undo(state);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            do_redo(state);
        }
        // Ctrl+K opens the command palette. Highest-leverage discoverability
        // affordance — every menu item, every panel toggle, every recent
        // project, every table in the loaded pack is searchable from one
        // input. open_command_palette resets buffer + selection so each
        // open lands on a clean state.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K)) {
            open_command_palette(state);
        }

        tick_status_msg(state);
        render_menubar(state);
        // Workspace rail before the dockspace — its window position is
        // viewport-anchored and doesn't depend on dockspace layout, but
        // rendering it first keeps Z order intuitive when both happen
        // to overlap during the GLFW window-refresh callback path.
        render_workspace_rail(state);
        render_dockspace_host(state);
        render_sidebar(state);
        render_table_view(state, fonts);
        render_stats_panel(state);
        render_dtcs_panel(state);
        render_history_panel(state);
        render_knock_dashboard_panel(state);
        render_adaptive_history_panel(state);
        render_coldstart_panel(state);
        render_ebcs_panel(state);
        render_features_designer(state);
        render_status_bar(state);
        // Toasts last so they layer over panels + the status bar's
        // window (each toast is its own undecorated viewport-anchored
        // window). Render order doesn't matter for modals — those
        // dim the background regardless.
        render_toasts(state);
        render_unsaved_modal(state);
        render_csv_import_modal(state);
        render_new_project_modal(state);
        render_maf_autotune_modal(state);
        render_kp_autotune_modal(state);
        render_flash_modal(state);
        render_read_rom_modal(state);
        render_def_registry_modal(state);
        render_settings_modal(state);
        render_shortcuts_modal(state);
        render_about_modal(state);
        // Command palette rendered last so it stacks above every other
        // modal — Ctrl+K is meant as a global escape hatch.
        render_command_palette(state);

        if (state.show_imgui_demo) {
            ImGui::ShowDemoWindow(&state.show_imgui_demo);
        }

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport: render OS-level windows that ImGui has spawned for
        // panels the user tore off. The GL context juggling is the standard
        // pattern from the ImGui examples.
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            GLFWwindow *prev_ctx = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(prev_ctx);
        }

        // OS window title reflects current state: project name + dirty
        // marker so taskbar / alt-tab show what's open and whether
        // there are unsaved changes. Static cache avoids the glfw call
        // every frame.
        {
            static std::string last_title;
            std::string desired;
            if (state.project.has_value()) {
                if (state.dirty) {
                    desired = "\xE2\x97\x8F ";
                }
                desired += state.project->display_name();
                desired += " \xE2\x80\x94 SubuwuTuner";
            } else {
                desired = "SubuwuTuner";
            }
            if (desired != last_title) {
                glfwSetWindowTitle(window, desired.c_str());
                last_title = std::move(desired);
            }
        }

        glfwSwapBuffers(window);
    };

    g_render_frame = render_frame;
    glfwSetWindowRefreshCallback(window, [](GLFWwindow * /*w*/) {
        if (g_render_frame) {
            g_render_frame();
        }
    });

    while (!state.user_confirmed_quit) {
        glfwPollEvents();
        render_frame();
    }
    g_render_frame = nullptr;

    // If a Read-ROM-from-Car worker is still running (user closed the GUI
    // mid-read instead of clicking Cancel), signal it to abort and join
    // before the AppState destructor runs. Without this the std::thread
    // destructor would terminate() on a joinable thread.
    if (state.read_rom_worker.joinable()) {
        if (state.read_rom_cancel)
            state.read_rom_cancel->store(true, std::memory_order_release);
        state.read_rom_worker.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
