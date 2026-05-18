add_library(st_transport STATIC
    src/mock.cpp
    src/obdx_dvi.cpp
    src/obdx_transport.cpp
    src/j2534.cpp
    src/j2534_transport.cpp
    src/native.cpp
    src/native_transport.cpp
)
add_library(st::transport ALIAS st_transport)

target_include_directories(st_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_transport PUBLIC cxx_std_23)

target_link_libraries(st_transport
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
