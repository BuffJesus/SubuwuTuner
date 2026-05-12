include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(st_unit_tests
    unit/core/test_result.cpp
    unit/core/test_error.cpp
    unit/core/test_version.cpp
    unit/core/test_crc32.cpp
    unit/rom/test_rom.cpp
    unit/defs/test_defs.cpp
    unit/edit/test_edit.cpp
    unit/project/test_project.cpp
    unit/transport/test_mock.cpp
    unit/ecu/test_ssm.cpp
    unit/ecu/test_uds.cpp
    unit/log/test_log.cpp
    unit/can/test_can.cpp
    unit/dbc/test_dbc.cpp
    unit/discover/test_discover.cpp
    unit/flash/test_flash.cpp
    unit/autotune/test_autotune.cpp
)

target_link_libraries(st_unit_tests
    PRIVATE
        st::core
        st::rom
        st::defs
        st::edit
        st::project
        st::transport
        st::ecu
        st::log
        st::can
        st::dbc
        st::discover
        st::flash
        st::autotune
        st::warnings
        st::sanitizers
        st::platform
        Catch2::Catch2WithMain
)

# Catch2's discovery integrates each TEST_CASE as an individual ctest entry.
catch_discover_tests(st_unit_tests
    REPORTER console
    OUTPUT_ON_FAILURE
)
