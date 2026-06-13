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
    unit/core/test_json_util.cpp
    unit/core/test_ext_interfaces.cpp
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
    unit/transport/test_cobb_ap_packet.cpp
    unit/devices/ap3/test_file_info.cpp
    unit/devices/ap3/test_live_smoke.cpp
    unit/devices/ap3/test_ptm_cipher.cpp
    unit/devices/ap3/test_ptm_cipher_private.cpp
    unit/devices/ap3/test_aes_ctr_chunk_boundary.cpp
    unit/devices/ap3/test_architectural_classifier.cpp
    unit/devices/ap3/test_user_info_parse.cpp
    unit/devices/ap3/test_user_info_parse_private.cpp
    unit/devices/ap3/test_zero_crc_tolerance.cpp
    unit/devices/ap3/test_read_file_path_encoding.cpp
    unit/devices/ap3/test_write_remove_path_encoding.cpp
    unit/devices/ap3/test_marriage_gate.cpp
    unit/devices/ap3/test_datalogger_cfg.cpp
    unit/devices/ap3/test_status_probes.cpp
    unit/devices/ap3/test_diagnostic_cmds.cpp
    unit/devices/ap3/test_error_frame_decode.cpp
    unit/library/test_patch_decoder.cpp
    unit/library/test_table_mapping.cpp
    unit/library/test_tune_diff.cpp
    unit/ecu/test_extended_did_datalog.cpp
    unit/ecu/test_ssm.cpp
    unit/ecu/test_ssm_properties.cpp
    unit/ecu/test_uds_properties.cpp
    unit/ecu/test_uds_state_machine_properties.cpp
    unit/ecu/test_subaru_security.cpp
    unit/ecu/test_cobb_gateway.cpp
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
    unit/flash/test_boot_signatures.cpp
    unit/flash/test_backup_store.cpp
    unit/flash/test_cancellation_invariants.cpp
    unit/flash/test_read_full_rom_sa_v4_port.cpp
    unit/flash/test_brick_recovery_fixture.cpp
    unit/flash/test_ascii_relay_bench_power.cpp
    unit/flash/test_subaru_sh_can_flash.cpp
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
    unit/policy/test_flash_preflight.cpp
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
    unit/audit/test_audit_subscribers.cpp
    unit/audit/test_apply_op_audit_contract.cpp
    unit/cli/test_cli_integration.cpp
    unit/cli/test_ptm_cli.cpp
    unit/profile/test_profile.cpp
    unit/ai/test_drift.cpp
    unit/ai/test_backend.cpp
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
        st::ai
        st::devices_ap3
        st::library
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

# Mirror the AP3 cipher gating to the test binary so the gated/ungated
# test cases compile to the right path. st_devices_ap3's PRIVATE
# definition doesn't reach test_ptm_cipher.cpp, so we set it again
# here when the option is ON.
if(ST_ENABLE_COBB_AP_CIPHER)
    target_compile_definitions(st_unit_tests PRIVATE ST_AP3_HAVE_CIPHER=1)
endif()
if(ST_ENABLE_COBB_AP_PTM_REWRITE)
    target_compile_definitions(st_unit_tests PRIVATE ST_AP3_HAVE_PTM_REWRITE=1)
endif()
# Mirror the Subaru-direct-flash gate to the test binary so
# test_subaru_sh_can_flash.cpp's #if branches select the right path.
if(ST_ENABLE_SUBARU_ECU_FLASH)
    target_compile_definitions(st_unit_tests PRIVATE ST_SUBARU_ECU_FLASH_ENABLED=1)
endif()

# Wire the CLI binary path + demo-fixture path into the test target so
# the CLI integration smoke (test_cli_integration.cpp) can locate the
# real subuwutuner-cli binary regardless of build dir. A build-time
# dependency on subuwutuner-cli guarantees the binary exists before
# tests start; if the consumer of this CMakeLists configured with
# tests-only (no CLI target), the integration cases self-skip via
# is_cli_available().
if (TARGET subuwutuner-cli)
    add_dependencies(st_unit_tests subuwutuner-cli)
    # $<TARGET_FILE> is a generator expression — resolved at build
    # time per-config (Debug / Release / etc).
    target_compile_definitions(st_unit_tests PRIVATE
        ST_CLI_BINARY_PATH="$<TARGET_FILE:subuwutuner-cli>")
endif()
target_compile_definitions(st_unit_tests PRIVATE
    ST_FIXTURE_DEMO_STUNE="${CMAKE_SOURCE_DIR}/fixtures/demo.stune")
# AP3 binary fixtures (six per spec §12 + a malformed-body negative
# vector per §4.2). Copied from D:\Subuwu\specs\fixtures\ap3\ — see
# fixtures/ap3/README.md for the per-fixture role.
target_compile_definitions(st_unit_tests PRIVATE
    ST_FIXTURE_AP3_DIR="${CMAKE_SOURCE_DIR}/fixtures/ap3")
target_compile_definitions(st_unit_tests PRIVATE
    ST_FIXTURE_AP3_CIPHER_DIR="${CMAKE_SOURCE_DIR}/fixtures/ap3/cipher")
# Exposes the source-tree definitions/ directory as a compile-time
# constant so tests can exercise real in-tree packs (e.g. asserting
# lf79103p.toml's [[workflow]] declarations still resolve every
# required_table). Same pattern as ST_FIXTURE_DEMO_STUNE.
target_compile_definitions(st_unit_tests PRIVATE
    ST_DEFINITIONS_DIR="${CMAKE_SOURCE_DIR}/definitions")

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
#
# WORKING_DIRECTORY is set to the project root so tests that reference
# fixture paths like "fixtures/demo.stune" resolve consistently
# whether ctest is invoked from the build dir, the project root, or
# anywhere else. Pre-fix, ~10 diff / verify_compare_inputs cases
# failed depending on invocation cwd.
catch_discover_tests(st_unit_tests
    REPORTER console
    OUTPUT_ON_FAILURE
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
