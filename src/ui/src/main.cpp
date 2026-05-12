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

#include "st/core/version.hpp"
#include "st/project.hpp"

// ImGui + backends.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // DockBuilder*

#include <GLFW/glfw3.h>
#include <nfd.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Fonts {
    ImFont *ui   = nullptr; // Sans for UI chrome (menus, labels, panels)
    ImFont *mono = nullptr; // Monospace for grids, hex, log output
};

struct AppState {
    std::optional<st::Project>               project;
    std::string                              status_msg;
    std::string                              selected_table_id;
    std::optional<st::Definition::TableData> current_table_data;
    bool                                     show_imgui_demo{false};

    void try_open_project(std::filesystem::path const &path) {
        auto r = st::Project::open(path);
        if (!r.has_value()) {
            status_msg = "Failed to open " + path.string() + ": " + r.error().to_string();
            project.reset();
            selected_table_id.clear();
            current_table_data.reset();
            return;
        }
        project = std::move(*r);
        status_msg.clear();
        selected_table_id.clear();
        current_table_data.reset();
    }

    void select_table(std::string const &id) {
        selected_table_id = id;
        current_table_data.reset();
        if (!project.has_value()) {
            return;
        }
        auto const *table = project->definition().find_table(id);
        if (table == nullptr) {
            return;
        }
        auto td = project->definition().read_table_values(project->working_rom(), *table);
        if (td.has_value()) {
            current_table_data = std::move(*td);
        }
    }

    void close_project() {
        project.reset();
        selected_table_id.clear();
        current_table_data.reset();
        status_msg.clear();
    }
};

// Native folder picker for a .stune project directory. nfd handles the OS
// dialog; we only have to feed the result back through Project::open.
void open_project_dialog(AppState &state) {
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::PickFolder(out_path);
    if (r == NFD_OKAY) {
        state.try_open_project(std::filesystem::path(out_path.get()));
    } else if (r == NFD_ERROR) {
        state.status_msg = std::string{"Open dialog error: "} + NFD::GetError();
    }
}

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

// Probe a few candidate paths and load the first one that exists. Returns
// nullptr if none was loadable, in which case ImGui's default font is used.
ImFont *load_first_existing(std::initializer_list<char const *> candidates,
                            float                               size_px) {
    auto &io = ImGui::GetIO();
    for (auto const *path : candidates) {
        if (path == nullptr) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (auto *f = io.Fonts->AddFontFromFileTTF(path, size_px); f != nullptr) {
            return f;
        }
    }
    return nullptr;
}

Fonts load_fonts() {
    Fonts f;
    // UI font — sans for menus, panels, labels. Tries Inter from a bundled
    // assets/ dir first (drop in to get the polished look), then a sane
    // system font per platform, finally falls back to ImGui's default.
    f.ui = load_first_existing({
                                   "assets/fonts/Inter-Regular.ttf",
                                   "C:/Windows/Fonts/segoeui.ttf",
                                   "/System/Library/Fonts/Helvetica.ttc",
                                   "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                               },
                               15.0f);

    // Mono — for grids, hex dumps, log output where alignment matters.
    f.mono = load_first_existing({
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

// Custom dark palette tuned for long tuning sessions: high contrast for the
// numerical grids, low chroma so the chrome reads as neutral. Accent colour
// nods to Subaru rally blue without dominating.
void apply_theme() {
    auto &s = ImGui::GetStyle();

    s.WindowPadding        = ImVec2(10.0f, 10.0f);
    s.FramePadding         = ImVec2(8.0f, 5.0f);
    s.CellPadding          = ImVec2(6.0f, 4.0f);
    s.ItemSpacing          = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing     = ImVec2(6.0f, 6.0f);
    s.IndentSpacing        = 20.0f;
    s.ScrollbarSize        = 14.0f;
    s.GrabMinSize          = 12.0f;
    s.WindowBorderSize     = 1.0f;
    s.ChildBorderSize      = 1.0f;
    s.PopupBorderSize      = 1.0f;
    s.FrameBorderSize      = 0.0f;
    s.TabBorderSize        = 0.0f;
    s.WindowRounding       = 4.0f;
    s.ChildRounding        = 4.0f;
    s.FrameRounding        = 3.0f;
    s.PopupRounding        = 4.0f;
    s.GrabRounding         = 3.0f;
    s.TabRounding          = 3.0f;
    s.ScrollbarRounding    = 8.0f;
    s.WindowTitleAlign     = ImVec2(0.0f, 0.5f);
    s.DockingSeparatorSize = 1.0f;

    constexpr ImVec4 accent       (0.21f, 0.46f, 0.76f, 1.00f);
    constexpr ImVec4 accent_hover (0.31f, 0.56f, 0.86f, 1.00f);
    constexpr ImVec4 accent_active(0.38f, 0.65f, 0.94f, 1.00f);

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.13f, 0.14f, 0.16f, 0.98f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text]                  = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.28f, 0.45f, 0.71f, 0.45f);

    c[ImGuiCol_FrameBg]               = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.27f, 0.30f, 0.35f, 1.00f);

    c[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.31f, 0.34f, 0.39f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.42f, 0.49f, 1.00f);

    c[ImGuiCol_CheckMark]             = accent_active;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accent_active;

    c[ImGuiCol_Button]                = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive]          = accent;
    c[ImGuiCol_Header]                = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_HeaderActive]          = accent;
    c[ImGuiCol_Separator]             = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorHovered]      = accent_hover;
    c[ImGuiCol_SeparatorActive]       = accent_active;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]     = accent_hover;
    c[ImGuiCol_ResizeGripActive]      = accent_active;

    c[ImGuiCol_Tab]                   = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]            = accent_hover;
    c[ImGuiCol_TabSelected]           = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]   = accent;
    c[ImGuiCol_TabDimmed]             = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);

    c[ImGuiCol_DockingPreview]        = ImVec4(0.21f, 0.46f, 0.76f, 0.70f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    c[ImGuiCol_PlotLines]             = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = accent_hover;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_PlotHistogramHovered]  = accent_hover;

    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    c[ImGuiCol_NavCursor]             = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    // With viewports enabled, OS-level windows render with their own alpha;
    // force fully-opaque WindowBg so detached panels don't show through to
    // the desktop.
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        c[ImGuiCol_WindowBg].w = 1.0f;
    }
}

constexpr float kStatusBarHeight = 26.0f;

// One transparent host window covering the work area (minus our manual status
// bar) hosts the dockspace. ImGuiDockNodeFlags_PassthruCentralNode keeps the
// empty central area transparent so the GL clear color shows through.
void render_dockspace_host() {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(
        ImVec2(vp->WorkSize.x, vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    auto const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                     | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoBringToFrontOnFocus
                     | ImGuiWindowFlags_NoNavFocus
                     | ImGuiWindowFlags_NoBackground
                     | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##dockspace_host", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID const id = ImGui::GetID("MainDockSpace");

    // On first ever run (no imgui.ini), set up a default split: Tables on the
    // left, Table view central. Once the user has a saved layout it's
    // respected automatically.
    if (ImGui::DockBuilderGetNode(id) == nullptr) {
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);

        ImGuiID left  = 0;
        ImGuiID center = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.22f, &left, &center);

        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Table", center);

        ImGui::DockBuilderFinish(id);
    }

    ImGui::DockSpace(id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

void render_menubar(AppState &state, GLFWwindow *window) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
                open_project_dialog(state);
            }
            if (ImGui::MenuItem("Close Project", nullptr, false,
                                state.project.has_value())) {
                state.close_project();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("ImGui demo window", nullptr, &state.show_imgui_demo);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("SubuwuTuner %.*s",
                        static_cast<int>(st::Version::string().size()),
                        st::Version::string().data());
            ImGui::Separator();
            ImGui::TextWrapped("File → Open Project... (Ctrl+O) to open a .stune");
            ImGui::TextWrapped("directory. You can also pass one on the command");
            ImGui::TextWrapped("line: subuwutuner-gui my.stune");
            ImGui::Separator();
            ImGui::TextWrapped("This UI is read-only. Editing lands soon.");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void render_sidebar(AppState &state) {
    ImGui::Begin("Tables");

    if (!state.project.has_value()) {
        ImGui::TextDisabled("(no project open)");
        if (!state.status_msg.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", state.status_msg.c_str());
        }
        ImGui::End();
        return;
    }

    auto const &def = state.project->definition();
    ImGui::TextDisabled("Pack: %s", def.pack().id.c_str());
    ImGui::TextDisabled("%zu tables", def.tables().size());
    ImGui::Separator();

    for (auto const &t : def.tables()) {
        bool const selected = state.selected_table_id == t.id;
        if (ImGui::Selectable(t.id.c_str(), selected)) {
            state.select_table(t.id);
        }
        if (ImGui::IsItemHovered() && !t.name.empty()) {
            ImGui::SetTooltip("%s\nDim: %dD\nAddress: 0x%08zX",
                              t.name.c_str(), t.dimensions, t.address);
        }
    }
    ImGui::End();
}

void render_table_grid(st::Definition::TableData const &td,
                       st::Table const *               tbl,
                       st::Scaling const *             scal,
                       Fonts const &                   fonts) {
    int const precision = scal != nullptr ? scal->precision : 0;
    auto const cols     = static_cast<int>(td.axis_x.size()) + 1;
    if (cols < 2) {
        ImGui::TextDisabled("(table has no X axis)");
        return;
    }

    auto const flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                     | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY
                     | ImGuiTableFlags_SizingFixedFit;

    // Grids are numerical — push monospace so column alignment is honest.
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    if (!ImGui::BeginTable("grid", cols, flags)) {
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
        return;
    }

    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    for (auto const x : td.axis_x) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", x);
        ImGui::TableSetupColumn(buf, ImGuiTableColumnFlags_WidthFixed, 80.0f);
    }
    ImGui::TableHeadersRow();

    auto const print_cell = [&](double v) {
        ImGui::Text("%.*f", precision, v);
    };

    if (tbl != nullptr && tbl->dimensions == 3) {
        ImGui::EndTable();
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
        ImGui::TextDisabled("(3D tables: TODO — slice selector + per-z grid)");
        return;
    }

    for (std::size_t r = 0; r < td.values.size(); ++r) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (!td.axis_y.empty() && r < td.axis_y.size()) {
            ImGui::Text("%.*f", precision, td.axis_y[r]);
        }
        for (auto const v : td.values[r]) {
            ImGui::TableNextColumn();
            print_cell(v);
        }
    }
    ImGui::EndTable();
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
}

void render_table_view(AppState &state, Fonts const &fonts) {
    ImGui::Begin("Table");

    if (state.selected_table_id.empty() || !state.project.has_value()) {
        ImGui::TextDisabled("Select a table from the left panel.");
        ImGui::End();
        return;
    }
    if (!state.current_table_data.has_value()) {
        ImGui::TextWrapped("Could not read table '%s' from the working ROM.",
                           state.selected_table_id.c_str());
        ImGui::End();
        return;
    }

    auto const *tbl  = state.project->definition().find_table(state.selected_table_id);
    auto const *scal = tbl != nullptr
                           ? state.project->definition().find_scaling(tbl->scaling)
                           : nullptr;

    ImGui::TextUnformatted(state.selected_table_id.c_str());
    if (tbl != nullptr && !tbl->name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("— %s", tbl->name.c_str());
    }
    if (tbl != nullptr) {
        ImGui::TextDisabled(
            "%dD | address 0x%08zX | %s%s%s",
            tbl->dimensions, tbl->address,
            tbl->category.empty() ? "" : tbl->category.c_str(),
            tbl->category.empty() ? "" : " | ",
            tbl->engine_safety_critical ? "engine-safety-critical"
                                         : (tbl->emissions_relevant ? "emissions-relevant"
                                                                     : ""));
    }
    if (scal != nullptr && !scal->unit.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", scal->unit.c_str());
    }
    ImGui::Separator();

    render_table_grid(*state.current_table_data, tbl, scal, fonts);
    ImGui::End();
}

void render_status_bar(AppState &state) {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                     | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking
                     | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    if (state.project.has_value()) {
        bool const dirty =
            state.project->working_rom().crc32() != state.project->source_crc32_at_create();
        ImGui::Text("%s  |  source 0x%08X  |  working 0x%08X  %s  |  history: %zu / %zu",
                    state.project->display_name().c_str(),
                    state.project->source_crc32_at_create(),
                    state.project->working_rom().crc32(),
                    dirty ? "(edited)" : "(clean)",
                    state.project->history().cursor(),
                    state.project->history().size());
    } else {
        ImGui::TextDisabled("No project loaded. %s",
                            state.status_msg.empty() ? "" : state.status_msg.c_str());
    }
    ImGui::End();
}

} // namespace

int main(int argc, char *argv[]) {
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == 0) {
        std::fprintf(stderr, "subuwutuner-gui: glfwInit failed\n");
        return 1;
    }

    // Stick to OpenGL 3.0 core-ish — the lowest target ImGui supports cleanly
    // across Win/Mac/Linux without needing extension-loader gymnastics.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow *window =
        glfwCreateWindow(1400, 880, "SubuwuTuner", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "subuwutuner-gui: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    Fonts const fonts = load_fonts();
    apply_theme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    if (argc >= 2) {
        state.try_open_project(argv[1]);
    } else {
        state.status_msg = "Open a .stune project: File → Open (Ctrl+O).";
    }

    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Ctrl+Q to quit (in addition to the menu).
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        // Ctrl+O to open a project.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
            open_project_dialog(state);
        }

        render_menubar(state, window);
        render_dockspace_host();
        render_sidebar(state);
        render_table_view(state, fonts);
        render_status_bar(state);

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

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
