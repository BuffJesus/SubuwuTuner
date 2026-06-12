add_library(st_library STATIC
    src/patch_decoder.cpp
    src/table_mapping.cpp
)

target_include_directories(st_library
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(st_library
    PUBLIC
        st::core
        st::defs   # table_mapping.hpp consumes st::Definition
    PRIVATE
        st::devices_ap3   # base64_decode lives here (cipher path)
        st::warnings
        st::sanitizers
        st::platform
)

add_library(st::library ALIAS st_library)
