add_library(st_feature_codegen STATIC
    src/feature_codegen.cpp
)
add_library(st::feature_codegen ALIAS st_feature_codegen)

target_include_directories(st_feature_codegen
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_feature_codegen PUBLIC cxx_std_23)

target_link_libraries(st_feature_codegen
    PUBLIC
        st::core
        st::defs
        st::feature
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
