add_library(st_feature_patch STATIC
    src/manifest.cpp
    src/rom_allocator.cpp
    src/splice_sh2a.cpp
    src/splice_rh850.cpp
    src/inserter.cpp
)
add_library(st::feature_patch ALIAS st_feature_patch)

target_include_directories(st_feature_patch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_feature_patch PUBLIC cxx_std_23)

# inserter.hpp consumes st::feature_codegen::PatchObject; the
# coupling is intentional (feature_patch is the downstream consumer
# of feature_codegen's output).
target_link_libraries(st_feature_patch
    PUBLIC
        st::core
        st::feature_codegen
    PRIVATE
        st::warnings
        st::sanitizers
)
