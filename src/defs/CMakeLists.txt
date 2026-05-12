include(FetchContent)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(tomlplusplus)

add_library(st_defs STATIC
    src/defs.cpp
)
add_library(st::defs ALIAS st_defs)

target_include_directories(st_defs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_defs PUBLIC cxx_std_23)

target_link_libraries(st_defs
    PUBLIC
        st::core
        st::rom
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
