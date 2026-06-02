add_executable(subuwutuner-cli
    main.cpp
)

target_link_libraries(subuwutuner-cli
    PRIVATE
        st::core
        st::config
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
        st::feature
        st::feature_codegen
        st::diff
        st::policy
        st::warnings
        st::sanitizers
        st::platform
)

set_target_properties(subuwutuner-cli PROPERTIES
    OUTPUT_NAME "subuwutuner-cli"
)

# MSVC's strict mode (/WX) elevates C4996 to an error for getenv / sscanf /
# strcpy / etc. The MS-recommended fix is to define _CRT_SECURE_NO_WARNINGS;
# the alternative is rewriting every cross-platform call to use the _s
# variants, which are Windows-only and would diverge the binary from
# its POSIX-portable cousins. SubuwuTuner targets parity across OSes,
# so we silence the deprecation warning on MSVC only.
if (MSVC)
    target_compile_definitions(subuwutuner-cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
