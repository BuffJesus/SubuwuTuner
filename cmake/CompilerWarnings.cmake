# cmake/CompilerWarnings.cmake
# Project-wide warning configuration applied via the `st_warnings` INTERFACE target.

add_library(st_warnings INTERFACE)
add_library(st::warnings ALIAS st_warnings)

set(_msvc_warnings
    /W4
    /permissive-
    /w14242 # implicit conversion, possible loss of data
    /w14254 # operator: conversion from 'type1' to 'type2', possible loss of data
    /w14263 # member function does not override a base class virtual member function
    /w14265 # class has virtual functions but destructor is not virtual
    /w14287 # unsigned/negative constant mismatch
    /we4289 # nonstandard extension used: loop control variable used outside the for-loop
    /w14296 # expression is always 'boolean_value'
    /w14311 # pointer truncation
    /w14545 # expression before comma evaluates to a function which is missing an argument list
    /w14546 # function call before comma missing argument list
    /w14547 # operator before comma has no effect; expected operator with side-effect
    /w14549 # operator before comma has no effect; did you intend 'operator'?
    /w14555 # expression has no effect; expected expression with side-effect
    /w14619 # pragma warning: there is no warning number
    /w14640 # thread-unsafe static member initialisation
    /w14826 # conversion is sign-extended
    /w14905 # wide string literal cast to LPSTR
    /w14906 # string literal cast to LPWSTR
    /w14928 # illegal copy-initialization
    /Zc:__cplusplus
    /Zc:preprocessor
)

set(_gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
)

set(_gcc_only_warnings
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast
)

if(MSVC)
    target_compile_options(st_warnings INTERFACE ${_msvc_warnings})
    if(ST_WARNINGS_AS_ERRORS)
        target_compile_options(st_warnings INTERFACE /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(st_warnings INTERFACE ${_gcc_clang_warnings})
    if(ST_WARNINGS_AS_ERRORS)
        target_compile_options(st_warnings INTERFACE -Werror)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(st_warnings INTERFACE ${_gcc_clang_warnings} ${_gcc_only_warnings})
    if(ST_WARNINGS_AS_ERRORS)
        target_compile_options(st_warnings INTERFACE -Werror)
    endif()
endif()

# Platform-specific link tweaks. Statically link the MinGW C/C++ runtime so
# Windows executables don't drag DLLs around the filesystem next to themselves.
add_library(st_platform INTERFACE)
add_library(st::platform ALIAS st_platform)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(st_platform INTERFACE
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()
