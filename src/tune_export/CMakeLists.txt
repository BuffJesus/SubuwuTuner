add_library(st_tune_export STATIC
    src/tune_export.cpp
)
add_library(st::tune_export ALIAS st_tune_export)

target_include_directories(st_tune_export
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_tune_export PUBLIC cxx_std_23)

# st::tune_export is pure math + std::vector. The only domain
# dependency is st::core for Result / Error.
target_link_libraries(st_tune_export
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
