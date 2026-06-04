add_library(st_ai STATIC
    src/drift.cpp
)
add_library(st::ai ALIAS st_ai)

target_include_directories(st_ai
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_ai PUBLIC cxx_std_23)

target_link_libraries(st_ai
    PUBLIC
        st::core
        st::log
    PRIVATE
        st::warnings
        st::sanitizers
)
