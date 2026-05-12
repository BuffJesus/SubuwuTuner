add_library(st_core STATIC
    src/error.cpp
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
    PRIVATE
        st::warnings
        st::sanitizers
)
