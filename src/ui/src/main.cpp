// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// subuwutuner-gui — minimal Dear ImGui front-end.
//
// First-cut UI: opens a .stune project passed as argv[1], renders a sidebar
// of tables, and shows the selected table as a read-only grid. Editing,
// project-management menus, and file-open dialogs land in follow-ups; the
// goal here is "something visual that runs end-to-end against the existing
// domain layer."

#include "st/core/version.hpp"
#include "st/project.hpp"

// ImGui + backends.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {

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
};

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

void render_menubar(AppState &state, GLFWwindow *window) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
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
            ImGui::TextWrapped("Open a .stune project by passing its path on the");
            ImGui::TextWrapped("command line:");
            ImGui::TextWrapped("    subuwutuner-gui my.stune");
            ImGui::Separator();
            ImGui::TextWrapped("This first-cut UI is read-only. Editing lands soon.");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void render_sidebar(AppState &state) {
    ImGui::SetNextWindowPos(ImVec2{0, 22}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{300, 0}, ImGuiCond_FirstUseEver);
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
                       st::Scaling const *             scal) {
    int const precision = scal != nullptr ? scal->precision : 0;
    auto const cols     = static_cast<int>(td.axis_x.size()) + 1;
    if (cols < 2) {
        ImGui::TextDisabled("(table has no X axis)");
        return;
    }

    auto const flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                     | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY
                     | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("grid", cols, flags)) {
        return;
    }

    // Column setup: leading axis-label column + one per X-axis value.
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
}

void render_table_view(AppState &state) {
    ImGui::SetNextWindowPos(ImVec2{310, 22}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{900, 600}, ImGuiCond_FirstUseEver);
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

    render_table_grid(*state.current_table_data, tbl, scal);
    ImGui::End();
}

void render_status_bar(AppState &state) {
    auto const &io = ImGui::GetIO();
    constexpr float kStatusHeight = 30.0f;
    ImGui::SetNextWindowPos(ImVec2{0, io.DisplaySize.y - kStatusHeight});
    ImGui::SetNextWindowSize(ImVec2{io.DisplaySize.x, kStatusHeight});
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                     | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoScrollbar);

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

    GLFWwindow *window = glfwCreateWindow(1280, 720, "SubuwuTuner", nullptr, nullptr);
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
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    AppState state;
    if (argc >= 2) {
        state.try_open_project(argv[1]);
    } else {
        state.status_msg = "Pass a .stune project path on the command line.";
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

        render_menubar(state, window);
        render_sidebar(state);
        render_table_view(state);
        render_status_bar(state);

        if (state.show_imgui_demo) {
            ImGui::ShowDemoWindow(&state.show_imgui_demo);
        }

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
