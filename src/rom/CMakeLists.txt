add_library(st_rom STATIC
    src/rom.cpp
)
add_library(st::rom ALIAS st_rom)

target_include_directories(st_rom
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_rom PUBLIC cxx_std_23)

target_link_libraries(st_rom
    PUBLIC
        st::core
    PRIVATE
        st::warnings
        st::sanitizers
)
