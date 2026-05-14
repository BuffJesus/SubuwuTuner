add_library(st_project STATIC
    src/project.cpp
)
add_library(st::project ALIAS st_project)

target_include_directories(st_project
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_project PUBLIC cxx_std_23)

target_link_libraries(st_project
    PUBLIC
        st::core
        st::rom
        st::defs
        st::edit
        st::policy
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
