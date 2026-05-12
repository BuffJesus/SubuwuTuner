add_library(st_discover STATIC
    src/discover.cpp
)
add_library(st::discover ALIAS st_discover)

target_include_directories(st_discover
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_discover PUBLIC cxx_std_23)

target_link_libraries(st_discover
    PUBLIC
        st::core
        st::can
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
