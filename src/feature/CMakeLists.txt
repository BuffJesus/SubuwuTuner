add_library(st_feature STATIC
    src/feature.cpp
)
add_library(st::feature ALIAS st_feature)

target_include_directories(st_feature
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_feature PUBLIC cxx_std_23)

target_link_libraries(st_feature
    PUBLIC
        st::core
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
