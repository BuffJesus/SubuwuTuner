add_library(st_config STATIC
    src/config.cpp
)
add_library(st::config ALIAS st_config)

target_include_directories(st_config
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_config PUBLIC cxx_std_23)

target_link_libraries(st_config
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
