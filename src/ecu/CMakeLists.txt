set(ST_ECU_SOURCES
    src/extended_did_datalog.cpp
    src/ssm.cpp
    src/uds.cpp
    src/subaru_security.cpp
)
if (ST_ENABLE_BULK_REFLASH_CIPHER)
    list(APPEND ST_ECU_SOURCES src/bulk_reflash_cipher.cpp)
endif()

add_library(st_ecu STATIC ${ST_ECU_SOURCES})
add_library(st::ecu ALIAS st_ecu)

target_include_directories(st_ecu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_ecu PUBLIC cxx_std_23)

# Propagate the gate macro publicly so flash + cli + tests see the same
# #ifdef ST_ENABLE_BULK_REFLASH_CIPHER as the cipher TU itself.
if (ST_ENABLE_BULK_REFLASH_CIPHER)
    target_compile_definitions(st_ecu PUBLIC ST_ENABLE_BULK_REFLASH_CIPHER=1)
endif()

target_link_libraries(st_ecu
    PUBLIC
        st::core
        st::transport
        st::audit
    PRIVATE
        st::warnings
        st::sanitizers
)
