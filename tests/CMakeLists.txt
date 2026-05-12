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
)

target_link_libraries(st_unit_tests
    PRIVATE
        st::core
        st::rom
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
