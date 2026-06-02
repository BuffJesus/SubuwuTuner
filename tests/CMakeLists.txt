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
    unit/config/test_config.cpp
    unit/rom/test_rom.cpp
    unit/defs/test_defs.cpp
    unit/defs/test_pack_registry.cpp
    unit/edit/test_edit.cpp
    unit/project/test_project.cpp
    unit/transport/test_mock.cpp
    unit/transport/test_obdx_dvi.cpp
    unit/transport/test_obdx_dvi_properties.cpp
    unit/transport/test_obdx_transport.cpp
    unit/transport/test_j2534.cpp
    unit/transport/test_j2534_transport.cpp
    unit/transport/test_native.cpp
    unit/transport/test_native_properties.cpp
    unit/transport/test_native_transport.cpp
    unit/transport/test_factory.cpp
    unit/transport/test_uds_trace.cpp
    unit/transport/test_j2534_discovery.cpp
    unit/ecu/test_ssm.cpp
    unit/ecu/test_ssm_properties.cpp
    unit/ecu/test_uds_properties.cpp
    unit/ecu/test_subaru_security.cpp
    unit/ecu/test_uds.cpp
    unit/ecu/test_uds_pdu_atomicity.cpp
    unit/ecu/test_bulk_reflash_cipher.cpp
    unit/log/test_log.cpp
    unit/log/test_knock_dashboard.cpp
    unit/log/test_adaptive_history.cpp
    unit/log/test_coldstart.cpp
    unit/log/test_ebcs.cpp
    unit/log/test_live_buffer.cpp
    unit/can/test_can.cpp
    unit/dbc/test_dbc.cpp
    unit/discover/test_discover.cpp
    unit/flash/test_flash.cpp
    unit/flash/test_checksum.cpp
    unit/flash/test_cancellation_invariants.cpp
    unit/flash/test_read_full_rom_sa_v4_port.cpp
    unit/flash/test_brick_recovery_fixture.cpp
    unit/flash/test_ascii_relay_bench_power.cpp
    # Bench-rig fixture sources (header + impl). The header lives
    # under tests/_helpers/; .cpp's compile into st_unit_tests so the
    # MockBenchPower + BrickRecoveryFixture unit tests can link.
    # AsciiRelayBenchPower is the first real IBenchPower concrete —
    # it's protocol-only (takes any IByteChannel, including the
    # LoopbackByteChannel in _helpers), so it lives here too and runs
    # in every CI build. tests/hil/ adds hardware-bound coverage on
    # top by passing a real serial channel to the same class.
    _helpers/mock_bench_power.cpp
    _helpers/brick_recovery_fixture.cpp
    _helpers/ascii_relay_bench_power.cpp
    unit/autotune/test_autotune.cpp
    unit/autotune/test_autotune_csv.cpp
    unit/autotune/test_autotune_lint.cpp
    unit/policy/test_policy.cpp
    unit/feature/test_feature.cpp
    unit/feature/test_feature_ir.cpp
    unit/feature_codegen/test_feature_codegen.cpp
    unit/feature_codegen/test_sh2a.cpp
    unit/feature_codegen/test_rh850.cpp
    unit/feature_codegen/test_address_gate.cpp
    unit/feature_patch/test_manifest.cpp
    unit/feature_patch/test_rom_allocator.cpp
    unit/feature_patch/test_splice_sh2a.cpp
    unit/feature_patch/test_splice_rh850.cpp
    unit/feature_patch/test_inserter.cpp
    unit/feature_patch/test_flash_bridge.cpp
    unit/diff/test_diff.cpp
    unit/audit/test_audit.cpp
    unit/profile/test_profile.cpp
)

target_link_libraries(st_unit_tests
    PRIVATE
        st::core
        st::config
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
        st::feature_patch
        st::diff
        st::audit
        st::profile
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

# Tests that pin specific encoder bit patterns (e.g. test_rh850.cpp)
# need direct access to the backend's private impl headers. Exposing
# the private dir as a test-only include keeps the test free of
# relative path-walking like "../../../src/feature_codegen/src/...".
# Only the test binary gets this; the rh850.hpp encoders remain
# internal to st::feature_codegen for normal callers.
target_include_directories(st_unit_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/feature_codegen/src")

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

# Hardware-in-the-loop tests — gated behind ST_BUILD_HIL_TESTS. The
# default (OFF) keeps st_unit_tests free of any real-hardware code
# paths so CI runners + dev machines without a bench can build + run
# the suite as usual. With the option ON, the HIL sources under
# tests/hil/ get added to st_unit_tests and any concrete IBenchPower
# (NumatoBenchPower etc., not yet shipped) gets linked in. See
# tests/hil/README.md for the runbook + docs/28 for the bench rig.
option(ST_BUILD_HIL_TESTS
    "Compile tests/hil/* into st_unit_tests for real-hardware runs. \
Requires a bench ECU + OBDX adapter + USB relay. Off by default."
    OFF)
if(ST_BUILD_HIL_TESTS)
    file(GLOB HIL_TEST_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/hil/*.cpp")
    if(HIL_TEST_SOURCES)
        target_sources(st_unit_tests PRIVATE ${HIL_TEST_SOURCES})
        message(STATUS "HIL tests enabled (${HIL_TEST_SOURCES})")
    endif()
endif()

# Catch2's discovery integrates each TEST_CASE as an individual ctest entry.
catch_discover_tests(st_unit_tests
    REPORTER console
    OUTPUT_ON_FAILURE
)
