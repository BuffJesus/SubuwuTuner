add_library(st_edit STATIC
    src/edit.cpp
)
add_library(st::edit ALIAS st_edit)

target_include_directories(st_edit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_edit PUBLIC cxx_std_23)

target_link_libraries(st_edit
    PUBLIC
        st::core
        st::defs
    PRIVATE
        st::warnings
        st::sanitizers
)
