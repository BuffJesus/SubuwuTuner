add_library(st_library STATIC
    src/patch_decoder.cpp
    src/table_mapping.cpp
    src/tune_diff.cpp
    src/ptm_xml_builder.cpp
    src/interpret.cpp
    src/atlas.cpp
    src/tune_index.cpp
    src/tune_annotation.cpp
    src/filename_classifier.cpp
    src/datalog_csv.cpp
    src/datalog_session.cpp
    src/log_analysis.cpp
    src/guided_tasks.cpp
)

target_include_directories(st_library
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(st_library
    PUBLIC
        st::core
        st::config # tune_index consults config::library_root for the default index path
        st::defs   # table_mapping.hpp consumes st::Definition
    PRIVATE
        st::devices_ets   # base64_decode lives here (cipher path)
        tomlplusplus::tomlplusplus   # atlas.cpp parses TOML
        st::warnings
        st::sanitizers
        st::platform
)

add_library(st::library ALIAS st_library)
