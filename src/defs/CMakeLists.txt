include(FetchContent)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(tomlplusplus)

# miniz: single-file public-domain zip / DEFLATE library. Used by
# st::defs::PackRegistry to read per-platform <platform>.zip archives
# at runtime (the ship-time form for bundled definitions; see
# tools/build_definition_archives.py and docs/17 for the overlay
# pattern that keeps loose .toml overrides above zipped entries).
#
# miniz 3.0.2's CMakeLists.txt declares cmake_minimum_required pre-3.5;
# CMake 4 errors on that by default. CMAKE_POLICY_VERSION_MINIMUM tells
# CMake to treat the subproject as if it required 3.5 (functionally
# unchanged for our scope — read-only zip parsing).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(MINIZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(miniz)

add_library(st_defs STATIC
    src/defs.cpp
    src/zip_archive.cpp
    src/pack_registry.cpp
)
add_library(st::defs ALIAS st_defs)

target_include_directories(st_defs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${miniz_BINARY_DIR}   # for the generated miniz.h after amalgamation
        ${miniz_SOURCE_DIR}   # for the loose miniz headers
)

target_compile_features(st_defs PUBLIC cxx_std_23)

target_link_libraries(st_defs
    PUBLIC
        st::core
        st::rom
    PRIVATE
        tomlplusplus::tomlplusplus
        miniz
        st::warnings
        st::sanitizers
)
