add_library(st_diff STATIC
    src/diff.cpp
)
add_library(st::diff ALIAS st_diff)

target_include_directories(st_diff
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_diff PUBLIC cxx_std_23)

# st::diff consumes Rom + Definition. The structured DiffSet output
# composes their existing public types; no new dependencies beyond
# the obvious upstream modules.
target_link_libraries(st_diff
    PUBLIC
        st::core
        st::rom
        st::defs
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
