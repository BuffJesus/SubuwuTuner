add_library(st_can STATIC
    src/can.cpp
)
add_library(st::can ALIAS st_can)

target_include_directories(st_can
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_can PUBLIC cxx_std_23)

target_link_libraries(st_can
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
