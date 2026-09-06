// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// subuwutuner-gui — Dear ImGui front-end. Bootstrap only:
// GLFW + ImGui + ImPlot context setup, font + theme load, the main
// render loop, and the per-frame keyboard-shortcut router. Everything
// substantive lives in:
//   - panels/         per-panel renderers + chrome (menubar, sidebar,
//                     workspace rail, dockspace, command palette,
//                     status bar, toasts)
//   - modals/         per-modal renderers (about, settings, flash,
//                     read_rom, autotune_*, csv_import, …)
//   - widgets/        text / chip / empty-state / button helpers +
//                     the adapter-picker sub-form
//   - actions.*       undo/redo, copy/paste, the action router,
//                     CSV import
//   - app_state.*     AppState type + try_open_project /
//                     select_table / close_project
//   - project_io.*    Open/Save/CSV file-dialog wrappers
//   - persistence.*   config dir + recents + settings text files
//   - theme.*         brand-purple palette + apply_theme + font load

#include "st/autotune.hpp"
#include "st/config.hpp"
#include "st/core/version.hpp"
#include "st/defs/pack_registry.hpp"
#include "st/edit.hpp"
#include "st/feature.hpp"
#include "st/flash.hpp"
#include "st/log/adaptive_history.hpp"
#include "st/log/coldstart.hpp"
#include "st/log/ebcs.hpp"
#include "st/log/knock_dashboard.hpp"
#include "st/policy.hpp"
#include "st/project.hpp"
#include "st/transport/factory.hpp"
#include "st/transport/mock.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/uds_trace.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "icon_data.hpp"
#include "modals/modals.hpp"
#include "panels/panels.hpp"
#include "persistence.hpp"
#include "project_io.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <nfd.hpp>
#include <string>
#include <string_view>

namespace st::ui {

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

// Render-frame callable shared between the main loop and GLFW's window-refresh
// callback. Windows enters a modal resize loop while the user drags a border;
// the main thread is blocked inside glfwPollEvents for the entire drag, so the
// only way to keep painting is to render from inside the WM_PAINT-driven
// refresh callback that GLFW dispatches during the modal loop.
//
// Lifted to outer st::ui scope so main() (at global scope) can address it
// after the namespace closes.
std::function<void()> g_render_frame;

// Exercise the non-graphical half of application startup. This deliberately
// runs before glfwInit(), making it suitable for CI agents and repair laptops
// with no usable display/GPU. It validates the same persisted settings and
// bundled resources a normal launch consumes, then opens either the supplied
// project or the shipped demo through the real Project loader.
int run_headless_startup_smoke(char const *argv0, char const *project_arg) {
    (void)load_settings();
    (void)load_recents();

    auto const docs = resolve_docs_dir(argv0);
    auto const changelog = resolve_changelog_path(argv0);
    auto const demo = resolve_demo_project_path(argv0);
    if (!docs.has_value() || !changelog.has_value() || !demo.has_value()) {
        std::fprintf(stderr,
                     "subuwutuner-gui: startup smoke failed: bundled docs, "
                     "changelog, or demo project was not found\n");
        return 2;
    }

    std::filesystem::path const project_path =
        (project_arg != nullptr && project_arg[0] != '\0')
            ? std::filesystem::path{project_arg}
            : *demo;
    auto project = st::Project::open(project_path);
    if (!project.has_value()) {
        auto const error = project.error().to_string();
        std::fprintf(stderr, "subuwutuner-gui: startup smoke failed opening %s: %s\n",
                     project_path.string().c_str(), error.c_str());
        return 3;
    }
    if (project->definition().tables().empty()) {
        std::fprintf(stderr,
                     "subuwutuner-gui: startup smoke failed: project has no tables\n");
        return 4;
    }

    std::printf("subuwutuner-gui: startup smoke passed (%s, %zu tables)\n",
                project_path.string().c_str(), project->definition().tables().size());
    return 0;
}

} // namespace st::ui

int main(int argc, char *argv[]) {
    using namespace st::ui;

    // Console-only commands must stay ahead of GLFW: --help should work on a
    // headless machine, and --smoke-test exists specifically to validate an
    // installation without creating a native window or contacting hardware.
    std::string_view const first_arg = (argc >= 2) ? argv[1] : "";
    if (first_arg == "-h" || first_arg == "--help" || first_arg == "/?") {
        std::fputs("Usage: subuwutuner-gui [PROJECT.stune]\n"
                   "       subuwutuner-gui --smoke-test [PROJECT.stune]\n"
                   "  Open the optional .stune project on launch. Without an "
                   "argument, the GUI starts on the welcome panel.\n"
                   "  --smoke-test validates settings, bundled resources, and "
                   "project loading without opening a window or hardware.\n",
                   stdout);
        return 0;
    }
    if (first_arg == "--smoke-test") {
        return run_headless_startup_smoke(argc >= 1 ? argv[0] : nullptr,
                                          argc >= 3 ? argv[2] : nullptr);
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == 0) {
        std::fprintf(stderr, "subuwutuner-gui: glfwInit failed\n");
        return 1;
    }

    // macOS offers either a legacy 2.1 context or a 3.2+ core profile that
    // must also be forward-compatible; a 3.0 request fails in NSGL. The GLSL
    // version moves with the context: a core profile rejects "#version 130".
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    char const *const glsl_version = "#version 150";
#else
    // OpenGL 3.0 is the lowest target ImGui supports on Windows and Linux
    // without an extension loader.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    char const *const glsl_version = "#version 130";
#endif

    GLFWwindow *window = glfwCreateWindow(1400, 880, "SubuwuTuner", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "subuwutuner-gui: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Floor the OS window at a size that keeps every chrome region
    // (menubar, workspace rail, dockspace, status bar) computing
    // non-negative dimensions. Below this we'd be passing negative
    // sizes to ImGui::SetNextWindowSize, which is UB. 640x400 is
    // tight but still enough to surface a panel's title bar +
    // toolbar; the GUI isn't usable that small but won't break either.
    glfwSetWindowSizeLimits(window, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);

    // Set the window title-bar icon from the compiled-in RGBA blob
    // (icon_data.hpp, regenerated by scripts/embed_icon.py from
    // assets/icon.png). GLFW takes ownership of nothing — pixels stay
    // in our static .rodata segment. The Windows Explorer / taskbar
    // EXE icon is set separately via subuwutuner.rc + windres in
    // src/ui/CMakeLists.txt; both originate from the same PNG. Cocoa has
    // no per-window icons and GLFW reports an error for the call, so
    // macOS skips it.
#if !defined(__APPLE__)
    {
        GLFWimage icon_image{};
        icon_image.width = st::ui::icon::kWidth;
        icon_image.height = st::ui::icon::kHeight;
        icon_image.pixels = const_cast<unsigned char *>(st::ui::icon::kRgba);
        glfwSetWindowIcon(window, 1, &icon_image);
    }
#endif

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
    // Multi-viewport intentionally OFF. Tear-off-to-OS-window is a nice-
    // to-have on multi-monitor setups, but ImGui's docking-branch
    // multi-viewport on Windows GLFW has a recurring class of z-order
    // bugs where popups whose computed position extends past the main
    // viewport detach into child OS windows that land behind the parent.
    // Manifestations seen in this codebase:
    //   - Menu dropdowns + BeginCombo dropdowns rendering behind docked
    //     panels (user report 2026-06-06).
    //   - First-run wizard invisible at startup (fix 601713d, hot-
    //     patched per-modal with SetNextWindowViewport — that commit's
    //     deferred follow-up was "do this for every modal", which we're
    //     subsuming by just turning the feature off).
    //   - Floating (undocked) panels can be dragged onto a secondary
    //     monitor as their own OS window, which technically is the
    //     point — but it also means a floating panel can land off-
    //     screen and effectively disappear (the user report's "stretched
    //     past bounds of the main window").
    // Single-window docking still works: ImGuiConfigFlags_DockingEnable
    // alone supports split/tear-off/re-dock within the GLFW window.
    // Re-enable when ImGui's multi-viewport story on Windows GLFW
    // tightens up, or if a user explicitly asks for it.
    // Force a tab bar on every docked window — even alone-in-node ones.
    // Without this, ImGui auto-hides the tab on single-window nodes and
    // there's no visible drag handle, so the panel can't be undocked or
    // moved by dragging once it's settled.
    io.ConfigDockingAlwaysTabBar = true;

    Fonts const fonts = load_fonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    state.recents = load_recents();
    state.settings = load_settings();
    // --reset-config flips first_run_complete on disk and re-launches
    // the wizard on next paint. Honored before any other arg checks.
    bool reset_requested = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--reset-config") {
            reset_requested = true;
            state.settings.first_run_complete = false;
            save_settings(state.settings);
            break;
        }
    }
    // Best-effort lookup of the bundled fixtures/demo.stune. Welcome
    // panel renders a "Try the demo project" button when set; absent
    // in installs that didn't ship the demo (the call returns
    // nullopt and the button just doesn't render).
    state.demo_project_path = resolve_demo_project_path(argc >= 1 ? argv[0] : nullptr);
    state.docs_dir = resolve_docs_dir(argc >= 1 ? argv[0] : nullptr);
    state.changelog_path = resolve_changelog_path(argc >= 1 ? argv[0] : nullptr);
    // Eager glossary parse — cheap TOML/markdown read, lifted out of
    // the first F1 / hover path so the first tooltip doesn't pay disk
    // latency. No-op when docs_dir wasn't located.
    preload_glossary(state);
    // Apply the persisted theme before any user-visible frame renders.
    apply_theme(state.settings.theme);
    // First-run wizard auto-trigger ONLY when --reset-config was just
    // used. On normal launches the wizard is opt-in via Help →
    // Welcome wizard or the welcome panel's "Run welcome wizard"
    // button — the auto-trigger pattern caused an invisible-modal
    // bug on 2026-06-01 night where multi-viewport popup detach
    // hid the wizard behind the main window, blocking input. The
    // viewport-pin fix in first_run.cpp prevents the detach; this
    // change removes the surprise of a forced auto-open for users
    // who upgraded from a build without the feature.
    if (reset_requested) {
        state.show_first_run_wizard = true;
        state.first_run_step = 0;
    }
    std::string_view const arg1 = (argc >= 2) ? argv[1] : "";
    // Skip arg1 if it's a flag we already handled (e.g. --reset-config)
    // so the project-path-from-argv path doesn't try to open a flag
    // string as a directory.
    if (!arg1.empty() && !arg1.starts_with("--")) {
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
        // Skip when the help modal owns Ctrl+F (find-in-doc gesture
        // takes precedence over the sidebar-filter shortcut while the
        // modal is on screen).
        if (!state.show_help_modal && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
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
        // F1 toggles the in-app help / glossary modal. Open when not
        // currently showing, close when already showing — second F1
        // press is a "I'm done with help" acknowledgement, matches
        // how F1 behaves in most IDEs / web apps. F1 isn't a typeable
        // character so consuming it globally doesn't conflict with
        // any field-local handler.
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
            if (!state.show_help_modal) {
                // Per-panel context routing — closes analyst QW-I.
                // Each panel updates state.help_context each render
                // when its window is focused; consult that to pick
                // the topic this F1 should land on.
                using HC = AppState::HelpContext;
                char const *topic_id = nullptr;
                switch (state.help_context) {
                case HC::FlashModal:
                    topic_id = "31-brick-protection-by-isa";
                    break;
                case HC::ReadRomModal:
                    topic_id = "23-security-access";
                    break;
                case HC::Audit:
                    topic_id = "08-testing-strategy";
                    break;
                case HC::Compare:
                    topic_id = "21-stune-format";
                    break;
                case HC::FeaturesDesigner:
                    topic_id = "16-custom-features";
                    break;
                case HC::AdaptiveHistory:
                    topic_id = "20-ai-integration";
                    break;
                case HC::ColdStart:
                case HC::Ebcs:
                case HC::KnockDashboard:
                    topic_id = "05-improvements";
                    break;
                case HC::TableEditor:
                case HC::Sidebar:
                    topic_id = "11-definition-format";
                    break;
                case HC::SettingsModal:
                    topic_id = "25-config-system";
                    break;
                case HC::AccessPortBrowser:
                    topic_id = "34-cobb-ap-as-tune-vault";
                    break;
                case HC::History:
                    topic_id = "21-stune-format";
                    break;
                case HC::Stats:
                    // Stats summarizes table-level numeric properties;
                    // the definition-format doc covers what those
                    // properties mean (units, scaling, axis types).
                    topic_id = "11-definition-format";
                    break;
                case HC::Dtcs:
                    // DTCs panel sits at the emissions/jurisdiction
                    // intersection — disable-DTC routes through the
                    // policy gate before any edit lands.
                    topic_id = "06-legal-ethics";
                    break;
                case HC::GaugeCluster:
                    topic_id = "32-live-datalogger";
                    break;
                case HC::Library:
                    // Library panel covers the user's local .ptm
                    // collection + currently-flashed cross-reference;
                    // docs/34 (AP file vault) is the closest match
                    // until a dedicated library doc lands.
                    topic_id = "34-cobb-ap-as-tune-vault";
                    break;
                case HC::FirstRunWizard:
                case HC::Welcome:
                    topic_id = "00-overview";
                    break;
                default:
                    break;
                }
                if (topic_id != nullptr) {
                    state.help_initial_topic_id = topic_id;
                }
            }
            state.show_help_modal = !state.show_help_modal;
        }
        // Workspace switches — Ctrl+1 = Tune, Ctrl+2 = Datalog, Ctrl+3
        // = Features. Mirrors the left-rail click ordering.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_1)) {
            apply_workspace_mode(state, WorkspaceMode::Tune);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_2)) {
            apply_workspace_mode(state, WorkspaceMode::Datalog);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_3)) {
            apply_workspace_mode(state, WorkspaceMode::Features);
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
        render_readiness_panel(state);
        render_stats_panel(state);
        render_dtcs_panel(state);
        render_history_panel(state);
        render_knock_dashboard_panel(state);
        render_adaptive_history_panel(state);
        render_coldstart_panel(state);
        render_ebcs_panel(state);
        render_gauge_cluster_panel(state);
        render_log_explorer_panel(state);
        render_ap3_browser_panel(state);
        render_library_panel(state);
        render_compare_panel(state);
        render_audit_panel(state);
        render_features_designer(state);
        render_status_bar(state);
        // Toasts last so they layer over panels + the status bar's
        // window (each toast is its own undecorated viewport-anchored
        // window). Render order doesn't matter for modals — those
        // dim the background regardless.
        render_toasts(state);
        render_unsaved_modal(state);
        render_csv_import_modal(state);
        render_ptm_import_modal(state);
        render_ptm_inspect_modal(state);
        render_ptm_export_modal(state);
        render_ptm_save_and_push_modal(state);
        render_boot_screen_modal(state);
        render_datalog_channels_modal(state);
        render_pull_file_modal(state);
        render_ptm_diff_modal(state);
        render_new_project_modal(state);
        render_maf_autotune_modal(state);
        render_kp_autotune_modal(state);
        render_flash_modal(state);
        render_read_rom_modal(state);
        render_def_registry_modal(state);
        render_settings_modal(state);
        render_first_run_modal(state);
        render_fa24_swap_modal(state);
        render_tgv_egr_delete_modal(state);
        render_shortcuts_modal(state);
        render_about_modal(state);
        render_doctor_modal(state);
        render_help_modal(state);
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
