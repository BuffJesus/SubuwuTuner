add_library(st_profile STATIC
    src/profile.cpp
)
add_library(st::profile ALIAS st_profile)

target_include_directories(st_profile
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_profile PUBLIC cxx_std_23)

target_link_libraries(st_profile
    PUBLIC
        st::core
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
