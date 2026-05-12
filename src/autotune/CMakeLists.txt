add_library(st_autotune STATIC
    src/autotune.cpp
)
add_library(st::autotune ALIAS st_autotune)

target_include_directories(st_autotune
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_autotune PUBLIC cxx_std_23)

target_link_libraries(st_autotune
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
