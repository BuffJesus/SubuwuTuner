add_library(st_audit STATIC
    src/audit.cpp
)
add_library(st::audit ALIAS st_audit)

target_include_directories(st_audit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_audit PUBLIC cxx_std_23)

target_link_libraries(st_audit
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
