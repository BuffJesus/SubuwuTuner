# cmake/Sanitizers.cmake
# Applied via the `st_sanitizers` INTERFACE target. ST_SANITIZE is a list,
# e.g. "address;undefined" or "thread".

add_library(st_sanitizers INTERFACE)
add_library(st::sanitizers ALIAS st_sanitizers)

if(NOT ST_SANITIZE)
    return()
endif()

if(MSVC)
    if("address" IN_LIST ST_SANITIZE)
        target_compile_options(st_sanitizers INTERFACE /fsanitize=address)
    endif()
    return()
endif()

set(_flags "")
foreach(_san IN LISTS ST_SANITIZE)
    list(APPEND _flags -fsanitize=${_san})
endforeach()

target_compile_options(st_sanitizers INTERFACE ${_flags} -fno-omit-frame-pointer -g)
target_link_options(st_sanitizers INTERFACE ${_flags})
