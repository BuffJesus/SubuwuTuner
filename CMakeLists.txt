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
message(STATUS "")
