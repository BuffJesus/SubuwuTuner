add_library(st_policy STATIC
    src/policy.cpp
)
add_library(st::policy ALIAS st_policy)

target_include_directories(st_policy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_policy PUBLIC cxx_std_23)

target_link_libraries(st_policy
    PRIVATE
        st::warnings
        st::sanitizers
)
