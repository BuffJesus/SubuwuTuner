option(ST_ENABLE_SUBARU_ECU_FLASH
    "Build the direct-Subaru-ECU-flash path (st::flash::SubaruShCanFlash). \
OFF by default — methods return PolicyDenied until the build flag is ON \
AND the runtime CLI/GUI gate is armed (separate flag planned for Tier B). \
See docs/37-subaru-flash-protocol.md for the two-step arming model. With \
the flag ON, the methods compile but still return NotImplemented until \
Tier B fills in the UDS sequence body."
    OFF)

add_library(st_flash STATIC
    src/flash.cpp
    src/checksum.cpp
    src/backup_store.cpp
    src/boot_signatures.cpp
    # Subaru direct-flash skeleton. The .cpp picks PolicyDenied vs
    # NotImplemented stubs based on ST_SUBARU_ECU_FLASH_ENABLED.
    src/subaru_sh_can_flash.cpp
)
add_library(st::flash ALIAS st_flash)

if(ST_ENABLE_SUBARU_ECU_FLASH)
    target_compile_definitions(st_flash PRIVATE ST_SUBARU_ECU_FLASH_ENABLED=1)
endif()

target_include_directories(st_flash
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_flash PUBLIC cxx_std_23)

target_link_libraries(st_flash
    PUBLIC
        st::core
        st::defs
        st::policy
        st::transport
        st::ecu
    PRIVATE
        tomlplusplus::tomlplusplus
        st::warnings
        st::sanitizers
)
