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
FetchContent_MakeAvailable(glfw)

# Dear ImGui doesn't ship a top-level CMakeLists, so we fetch the source and
# build a static lib ourselves with the GLFW + OpenGL3 backends.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.0
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

find_package(OpenGL REQUIRED)

add_executable(subuwutuner-gui
    src/main.cpp
)

target_link_libraries(subuwutuner-gui
    PRIVATE
        st::core
        st::rom
        st::defs
        st::edit
        st::project
        imgui_lib
        OpenGL::GL
        st::warnings
        st::sanitizers
        st::platform
)

set_target_properties(subuwutuner-gui PROPERTIES
    OUTPUT_NAME "subuwutuner-gui"
)
