add_executable(subuwutuner-cli
    main.cpp
)

target_link_libraries(subuwutuner-cli
    PRIVATE
        st::core
        st::rom
        st::defs
        st::edit
        st::project
        st::transport
        st::ecu
        st::log
        st::can
        st::dbc
        st::discover
        st::flash
        st::autotune
        st::policy
        st::warnings
        st::sanitizers
        st::platform
)

set_target_properties(subuwutuner-cli PROPERTIES
    OUTPUT_NAME "subuwutuner-cli"
)
