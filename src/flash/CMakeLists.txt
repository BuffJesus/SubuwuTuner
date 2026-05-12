add_library(st_flash STATIC
    src/flash.cpp
)
add_library(st::flash ALIAS st_flash)

target_include_directories(st_flash
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_flash PUBLIC cxx_std_23)

target_link_libraries(st_flash
    PUBLIC
        st::core
        st::transport
        st::ecu
    PRIVATE
        st::warnings
        st::sanitizers
)
