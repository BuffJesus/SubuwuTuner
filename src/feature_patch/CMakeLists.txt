add_library(st_feature_patch STATIC
    src/manifest.cpp
    src/rom_allocator.cpp
    src/splice_sh2a.cpp
)
add_library(st::feature_patch ALIAS st_feature_patch)

target_include_directories(st_feature_patch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_feature_patch PUBLIC cxx_std_23)

target_link_libraries(st_feature_patch
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
