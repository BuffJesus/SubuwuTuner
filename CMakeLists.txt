cmake_minimum_required(VERSION 3.28)

project(SubuwuTuner
    VERSION 0.1.0
    DESCRIPTION "ECU calibration suite for Subaru WRX (VA/VB)"
    HOMEPAGE_URL "https://github.com/"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Project options
# ---------------------------------------------------------------------------
option(ST_BUILD_TESTS    "Build unit tests"                ON)
option(ST_BUILD_CLI      "Build the subuwutuner-cli tool"  ON)
option(ST_BUILD_GUI      "Build the subuwutuner-gui tool"  ON)
option(ST_WARNINGS_AS_ERRORS "Treat warnings as errors"    ON)
set(ST_SANITIZE "" CACHE STRING
    "Semicolon-separated sanitizers to enable (e.g. address;undefined). Linux/Mac only.")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd / clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if the user didn't specify.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Output everything into a single place per-config for easy invocation.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Hide symbols by default on Unix; let library exports do the rest.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Position independent code so we can mix static libs into shared later.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/rom)
add_subdirectory(src/defs)
add_subdirectory(src/edit)
add_subdirectory(src/project)
add_subdirectory(src/transport)
add_subdirectory(src/ecu)
add_subdirectory(src/log)
add_subdirectory(src/can)
add_subdirectory(src/dbc)
add_subdirectory(src/discover)
add_subdirectory(src/flash)
add_subdirectory(src/autotune)
add_subdirectory(src/policy)
add_subdirectory(src/feature)
add_subdirectory(src/feature_codegen)

if(ST_BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(ST_BUILD_GUI)
    add_subdirectory(src/ui)
endif()

if(ST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
#
# Default install layout (Windows / MinGW; Linux uses `bin/` per convention):
#
#   <install>/
#     subuwutuner-gui.exe
#     subuwutuner-cli.exe
#     definitions/
#       <platform>.zip   (built at install time from definitions/<platform>/)
#
# The bundled-definitions archives drop disk footprint from ~71 MB loose to
# ~6 MB zipped (~92% compression on TOML — see commit 8192817 and docs/17
# §7). The runtime PackRegistry overlay (commit 571ca22) reads the archives
# AND lets users drop loose `<id>.toml` files alongside as overrides, so
# this packaging step doesn't lock users out of customization.

option(ST_INSTALL_ARCHIVES
    "Build + install per-platform definition zip archives instead of \
loose TOMLs. OFF leaves the install dir without definitions/ and the \
caller is responsible for staging packs themselves."
    ON)

if(WIN32)
    set(ST_INSTALL_BIN_DIR ".")
else()
    set(ST_INSTALL_BIN_DIR "bin")
endif()

if(ST_BUILD_CLI)
    install(TARGETS subuwutuner-cli
            RUNTIME DESTINATION ${ST_INSTALL_BIN_DIR})
endif()
if(ST_BUILD_GUI)
    install(TARGETS subuwutuner-gui
            RUNTIME DESTINATION ${ST_INSTALL_BIN_DIR})
endif()

# Definition archives. Built at install time by tools/build_definition_archives.py
# so the artifact lives in the install dir's `definitions/` next to the
# binaries — no copy step, no intermediate build-dir staging area.
if(ST_INSTALL_ARCHIVES)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(WARNING
            "ST_INSTALL_ARCHIVES=ON requires Python3 for the archive build "
            "step. Either install Python3 or pass -DST_INSTALL_ARCHIVES=OFF.")
    else()
        # CMAKE_INSTALL_PREFIX is resolved at install time so DESTDIR /
        # `--prefix` overrides at `cmake --install` work correctly. The
        # source-side variables (Python interp, script path, defs root)
        # are substituted now at configure time.
        install(CODE "
            set(_archives_out \"\${CMAKE_INSTALL_PREFIX}/definitions\")
            message(STATUS \"Building definition archives -> \${_archives_out}\")
            execute_process(
                COMMAND \"${Python3_EXECUTABLE}\"
                        \"${CMAKE_SOURCE_DIR}/tools/build_definition_archives.py\"
                        --defs-dir \"${CMAKE_SOURCE_DIR}/definitions\"
                        --output-dir \"\${_archives_out}\"
                RESULT_VARIABLE _archives_rv
            )
            if(NOT _archives_rv EQUAL 0)
                message(FATAL_ERROR
                    \"Definition archive build failed (exit code \${_archives_rv}); \"
                    \"either fix the failure or reconfigure with -DST_INSTALL_ARCHIVES=OFF.\")
            endif()
        ")
    endif()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "SubuwuTuner ${PROJECT_VERSION}")
message(STATUS "  C++ standard          : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type            : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler              : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests                 : ${ST_BUILD_TESTS}")
message(STATUS "  CLI                   : ${ST_BUILD_CLI}")
message(STATUS "  GUI                   : ${ST_BUILD_GUI}")
message(STATUS "  Warnings as errors    : ${ST_WARNINGS_AS_ERRORS}")
message(STATUS "  Sanitizers            : ${ST_SANITIZE}")
message(STATUS "  Install archives      : ${ST_INSTALL_ARCHIVES}")
message(STATUS "  Install bin dir       : ${ST_INSTALL_BIN_DIR}")
message(STATUS "")
