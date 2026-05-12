add_library(st_ecu STATIC
    src/ssm.cpp
    src/uds.cpp
)
add_library(st::ecu ALIAS st_ecu)

target_include_directories(st_ecu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_ecu PUBLIC cxx_std_23)

target_link_libraries(st_ecu
    PUBLIC
        st::core
        st::transport
    PRIVATE
        st::warnings
        st::sanitizers
)
