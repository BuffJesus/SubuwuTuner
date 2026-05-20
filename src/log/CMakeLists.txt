add_library(st_log STATIC
    src/log.cpp
    src/knock_dashboard.cpp
    src/adaptive_history.cpp
    src/coldstart.cpp
)
add_library(st::log ALIAS st_log)

target_include_directories(st_log
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_log PUBLIC cxx_std_23)

target_link_libraries(st_log
    PUBLIC
        st::core
        st::defs
        st::transport
    PRIVATE
        st::ecu
        st::warnings
        st::sanitizers
)
