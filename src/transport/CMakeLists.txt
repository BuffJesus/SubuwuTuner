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
    # COBB AccessPort V3 USB byte-channel: wire codec is always
    # built (no external deps); the libusb-backed channel impl is
    # the same TU regardless of ST_ENABLE_AP3 but compiles into a
    # NotImplemented stub when libusb isn't wired (see below).
    src/ets_packet.cpp
    src/ets_channel.cpp
)

# Platform-specific IByteChannel implementations. USB-CDC ACM
# adapters (OBDX Pro VX et al.) show up as a virtual COM port on
# every OS — Win32 CreateFile here, POSIX termios when Linux/macOS
# impls land. The POSIX stub returns NotImplemented so st_transport
# links cleanly on every CI matrix entry until the real backend ships.
if(WIN32)
    list(APPEND _st_transport_sources src/serial_byte_channel_win.cpp)
else()
    list(APPEND _st_transport_sources src/serial_byte_channel_posix.cpp)
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

# ---------------------------------------------------------------------------
# COBB AccessPort V3 (USB byte-channel for the AP3 file-vault).
# ---------------------------------------------------------------------------
#
# When ST_ENABLE_AP3=ON, FetchContent pulls the libusb-cmake wrapper
# around libusb-1.0 and the AP3 channel uses bulk OUT/IN on endpoints
# 0x03 / 0x82. When OFF (default), the channel TU compiles its
# NotImplemented fallback so the CLI's `ap3` subcommand still links
# and fails cleanly at runtime, and the binary never opens a USB
# device.
#
# Default flipped to OFF per HANDOFF-from-analyst-2026-06-14-edit-export-
# push-ptm.md: distribution must not pull libusb. Cornelio's local
# build sets -DST_ENABLE_COBB_AP_WORKFLOW=ON which auto-flips this back
# to ON via the top-level umbrella.
#
# Linux CI runners typically need `libudev-dev` (or distro equivalent)
# on PATH for libusb-cmake to build. Windows requires Zadig to rebind
# the AP3 device to WinUSB at runtime — see docs/install.md.
#
# Spec: D:\Subuwu\specs\references\cobb-ap3-usb-protocol.md §3
# Gating doc: docs/34-cobb-ap-as-tune-vault.md
option(ST_ENABLE_AP3
    "Build the libusb-backed AP3 file-vault channel. When ON, FetchContent \
pulls libusb-1.0 via the libusb-cmake wrapper. OFF by default — the AP3 \
transport links a NotImplemented stub and the file-vault CLI subcommand \
fails at runtime. Auto-enabled by ST_ENABLE_COBB_AP_WORKFLOW."
    OFF)

if(ST_ENABLE_AP3)
    include(FetchContent)
    # libusb-cmake is the upstream CMake wrapper around libusb-1.0.
    # Pinned to a recent stable; the upstream version of libusb wrapped
    # in this tag is 1.0.27, which has all the bulk_transfer + kernel-
    # driver-detach APIs the channel impl uses.
    set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    set(LIBUSB_ENABLE_UDEV ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libusb_cmake
        GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
        GIT_TAG        v1.0.27-0
        GIT_SHALLOW    TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(libusb_cmake)

    target_compile_definitions(st_transport PRIVATE ST_AP3_HAVE_LIBUSB=1)
    target_link_libraries(st_transport PRIVATE usb-1.0)
endif()
