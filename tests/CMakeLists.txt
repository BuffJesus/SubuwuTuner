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
    unit/transport/test_obdx_dvi.cpp
    unit/transport/test_obdx_dvi_properties.cpp
    unit/transport/test_obdx_transport.cpp
    unit/transport/test_j2534.cpp
    unit/transport/test_j2534_transport.cpp
    unit/transport/test_native.cpp
    unit/transport/test_native_transport.cpp
    unit/transport/test_factory.cpp
    unit/transport/test_uds_trace.cpp
    unit/transport/test_j2534_discovery.cpp
    unit/ecu/test_ssm.cpp
    unit/ecu/test_ssm_properties.cpp
    unit/ecu/test_subaru_security.cpp
    unit/ecu/test_uds.cpp
    unit/ecu/test_uds_pdu_atomicity.cpp
    unit/log/test_log.cpp
    unit/log/test_knock_dashboard.cpp
    unit/log/test_adaptive_history.cpp
    unit/log/test_coldstart.cpp
    unit/log/test_ebcs.cpp
    unit/can/test_can.cpp
    unit/dbc/test_dbc.cpp
    unit/discover/test_discover.cpp
    unit/flash/test_flash.cpp
    unit/flash/test_checksum.cpp
    unit/flash/test_cancellation_invariants.cpp
    unit/autotune/test_autotune.cpp
    unit/autotune/test_autotune_csv.cpp
    unit/autotune/test_autotune_lint.cpp
    unit/policy/test_policy.cpp
    unit/feature/test_feature.cpp
    unit/feature/test_feature_ir.cpp
    unit/feature_codegen/test_feature_codegen.cpp
    unit/feature_codegen/test_sh2a.cpp
    unit/feature_codegen/test_address_gate.cpp
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
        st::policy
        st::feature
        st::feature_codegen
        st::warnings
        st::sanitizers
        st::platform
        Catch2::Catch2WithMain
)

# MSVC's strict mode (/WX) elevates C4996 to an error for strncpy /
# sscanf / getenv / etc. Same posture as the cli/gui targets — see
# src/cli/CMakeLists.txt for rationale.
if (MSVC)
    target_compile_definitions(st_unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Private integration tests — exercise against locally-staged ROM
# dumps in fixtures/private/. Source files are gitignored
# (`.gitignore` whitelists only README + template); CONFIGURE_DEPENDS
# re-runs the glob at build-system regen so dropping a new *.cpp in
# tests/private/ picks it up without editing this file. Filter at
# runtime with the [private] Catch2 tag the template demonstrates.
file(GLOB PRIVATE_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/private/*.cpp")
if(PRIVATE_TEST_SOURCES)
    target_sources(st_unit_tests PRIVATE ${PRIVATE_TEST_SOURCES})
    message(STATUS "Private integration tests staged: "
                   "${PRIVATE_TEST_SOURCES}")
endif()

# Catch2's discovery integrates each TEST_CASE as an individual ctest entry.
catch_discover_tests(st_unit_tests
    REPORTER console
    OUTPUT_ON_FAILURE
)
