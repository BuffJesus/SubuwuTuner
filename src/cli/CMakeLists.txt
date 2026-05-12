add_executable(subarutuner-cli
    main.cpp
)

target_link_libraries(subarutuner-cli
    PRIVATE
        st::core
        st::rom
        st::warnings
        st::sanitizers
        st::platform
)

set_target_properties(subarutuner-cli PROPERTIES
    OUTPUT_NAME "subarutuner-cli"
)
