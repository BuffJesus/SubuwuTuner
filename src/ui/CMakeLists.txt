include(FetchContent)

# GLFW provides the window + GL context + input.
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    SYSTEM
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
# Build the X11 backend only on Linux. Wayland sessions still get the GUI
# via XWayland, which is universally available on every Wayland desktop.
# Skipping the Wayland backend means GLFW does not need wayland-scanner at
# configure time — that binary lives in libwayland-bin on Ubuntu and has
# been a recurring CI dependency-drift footgun on GitHub-hosted runners
# (image-roll-related). The cost is a small perf delta on native Wayland;
# acceptable for a v1.0-targeted tuning workbench. Re-enable if native
# Wayland latency becomes a measurable problem.
set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Dear ImGui — docking branch. Adds ImGuiConfigFlags_DockingEnable (multi-pane
# layouts with split/tear-off panels) and ImGuiConfigFlags_ViewportsEnable
# (panels can detach into OS-level windows on multi-monitor setups). The
# docking branch is upstream-maintained and tracks master with periodic syncs.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
# SYSTEM so our strict warnings don't apply to ImGui's source.
target_include_directories(imgui_lib SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)

# ImPlot — datalogger charts, scope plots, surface preview on the 3D map
# editor. Same author + API style as Dear ImGui, no top-level CMakeLists.
# Tracks master because the last tagged release (v0.16, Aug 2023) predates
# the docking-branch ImGui API rename (IM_OFFSETOF, ImFont::FontSize, etc.).
# Master has the fixes; pin to a commit hash when reproducibility matters.
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(implot)

add_library(implot_lib STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
)
target_include_directories(implot_lib SYSTEM PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot_lib PUBLIC imgui_lib)

# nativefiledialog-extended — OS-native Open/Save dialogs. ImGui's built-in
# file picker is rudimentary; this gives us the platform-correct widget.
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
    GIT_SHALLOW    TRUE
    SYSTEM
)
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nfd)

find_package(OpenGL REQUIRED)

# Windows resource script — embeds assets/icon.ico into the EXE so
# Explorer/taskbar/Alt-Tab pick up the SubuwuTuner icon. The runtime
# GLFW window-title-bar icon is set separately in main.cpp via
# glfwSetWindowIcon (loads the same icon.png at startup). On non-Windows
# the .rc is silently skipped — Linux/macOS use the runtime path only.
if (WIN32)
    enable_language(RC)
    set(_subuwutuner_resources ${CMAKE_CURRENT_SOURCE_DIR}/subuwutuner.rc)
else()
    set(_subuwutuner_resources "")
endif()

add_executable(subuwutuner-gui
    src/main.cpp
    src/panels/adaptive_history.cpp
    src/panels/coldstart.cpp
    src/panels/command_palette.cpp
    src/panels/dtcs.cpp
    src/panels/ebcs.cpp
    src/panels/features_designer.cpp
    src/panels/gauge_cluster.cpp
    src/panels/history.cpp
    src/panels/knock_dashboard.cpp
    src/panels/menubar.cpp
    src/panels/sidebar.cpp
    src/panels/stats.cpp
    src/panels/status_bar.cpp
    src/panels/table_view.cpp
    src/panels/toasts.cpp
    src/panels/welcome.cpp
    src/panels/workspace.cpp
    src/actions.cpp
    src/app_state.cpp
    src/persistence.cpp
    src/project_io.cpp
    src/theme.cpp
    src/widgets/adapter_picker.cpp
    src/widgets/widgets.cpp
    src/modals/about.cpp
    src/modals/autotune_knock.cpp
    src/modals/autotune_maf.cpp
    src/modals/csv_import.cpp
    src/modals/def_registry.cpp
    src/modals/flash.cpp
    src/modals/new_project.cpp
    src/modals/read_rom.cpp
    src/modals/settings.cpp
    src/modals/shortcuts.cpp
    src/modals/unsaved.cpp
    ${_subuwutuner_resources}
)

# Lets per-folder headers (panels/, modals/, widgets/) include each other
# with flat paths like `#include "app_state.hpp"` from any depth — saves
# the per-cpp move steps from having to walk `../` to find shared headers.
target_include_directories(subuwutuner-gui PRIVATE src)

target_link_libraries(subuwutuner-gui
    PRIVATE
        st::core
        st::config
        st::rom
        st::defs
        st::edit
        st::flash
        st::policy
        st::project
        st::autotune
        st::feature
        st::log
        imgui_lib
        implot_lib
        nfd
        OpenGL::GL
        st::warnings
        st::sanitizers
        st::platform
)

set_target_properties(subuwutuner-gui PROPERTIES
    OUTPUT_NAME "subuwutuner-gui"
)

# MSVC strict-mode C4996 fix — see src/cli/CMakeLists.txt for rationale.
if (MSVC)
    target_compile_definitions(subuwutuner-gui PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Apple deprecated the desktop OpenGL framework in macOS 10.14 (Mojave),
# but it still works and ImGui's GL3 backend uses it. -Wdeprecated-declarations
# fires on every glViewport / glClearColor / glClear in the render loop;
# -Werror promotes the lot. GL_SILENCE_DEPRECATION is Apple's documented
# escape hatch — silences just the OpenGL-deprecation channel without
# affecting unrelated deprecation warnings.
if (APPLE)
    target_compile_definitions(subuwutuner-gui PRIVATE GL_SILENCE_DEPRECATION)
endif()
