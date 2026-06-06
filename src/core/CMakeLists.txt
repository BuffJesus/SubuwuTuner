include(FetchContent)

# tl::expected is fetched unconditionally so the build is self-contained.
# The header is included only when std::expected isn't available (see
# `result.hpp`). When std::expected exists we still link the interface
# target so the include path resolves, but the std header takes priority.
FetchContent_Declare(
    tl_expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG        v1.1.0
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL    # suppresses tl_expected's install rules
)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tl_expected)

add_library(st_core STATIC
    src/error.cpp
    src/json_util.cpp
    src/version.cpp
)
add_library(st::core ALIAS st_core)

target_include_directories(st_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Generate version header from CMake project version.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/st/core/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/st/core/version.hpp
    @ONLY
)
target_include_directories(st_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)

target_compile_features(st_core PUBLIC cxx_std_23)

target_link_libraries(st_core
    PUBLIC
        tl::expected
    PRIVATE
        st::warnings
        st::sanitizers
)
