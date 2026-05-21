set(_st_transport_sources
    src/mock.cpp
    src/obdx_dvi.cpp
    src/obdx_transport.cpp
    src/j2534.cpp
    src/j2534_transport.cpp
    src/j2534_discovery.cpp
    src/native.cpp
    src/native_transport.cpp
    src/factory.cpp
    src/uds_trace.cpp
)

# Platform-specific IByteChannel implementations. USB-CDC ACM
# adapters (OBDX Pro VX et al.) show up as a virtual COM port on
# every OS — Win32 CreateFile here, POSIX termios when Linux/macOS
# impls land.
if(WIN32)
    list(APPEND _st_transport_sources src/serial_byte_channel_win.cpp)
endif()

add_library(st_transport STATIC ${_st_transport_sources})

# Win32 registry access for j2534::discover_adapters. advapi32 is
# normally linked by default but we name it explicitly so MinGW +
# cross-compile configurations don't surprise us.
if(WIN32)
    target_link_libraries(st_transport PRIVATE advapi32)
endif()
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
