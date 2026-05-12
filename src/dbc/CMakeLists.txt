add_library(st_dbc STATIC
    src/dbc.cpp
)
add_library(st::dbc ALIAS st_dbc)

target_include_directories(st_dbc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_dbc PUBLIC cxx_std_23)

target_link_libraries(st_dbc
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
