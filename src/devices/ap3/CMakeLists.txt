add_library(st_devices_ap3 STATIC
    src/file_info.cpp
    src/client.cpp
    src/architectural_classifier.cpp
)
add_library(st::devices_ap3 ALIAS st_devices_ap3)

target_include_directories(st_devices_ap3
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_devices_ap3 PUBLIC cxx_std_23)

target_link_libraries(st_devices_ap3
    PUBLIC
        st::core
        st::transport
    PRIVATE
        st::warnings
        st::sanitizers
)
