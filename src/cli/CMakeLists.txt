add_executable(subuwutuner-cli
    main.cpp
)

target_link_libraries(subuwutuner-cli
    PRIVATE
        st::core
        st::rom
        st::defs
        st::edit
        st::warnings
        st::sanitizers
        st::platform
)

set_target_properties(subuwutuner-cli PROPERTIES
    OUTPUT_NAME "subuwutuner-cli"
)
