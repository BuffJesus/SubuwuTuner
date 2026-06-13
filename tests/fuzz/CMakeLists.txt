# Fuzz harness binaries. Default-OFF — flip ST_BUILD_FUZZ_HARNESSES=ON
# at configure time with a Clang toolchain to build them. The harnesses
# link libFuzzer (-fsanitize=fuzzer), which most distro GCCs don't ship.
#
# See tests/fuzz/README.md for the build + run recipe.

option(ST_BUILD_FUZZ_HARNESSES
    "Build the libFuzzer harnesses in tests/fuzz/. Requires Clang \
+ libFuzzer + ASan/UBSan in the compile flags. OFF by default — \
most CI runners don't have the sanitizer toolchain, and the \
default build doesn't need them. See tests/fuzz/README.md."
    OFF)

if(NOT ST_BUILD_FUZZ_HARNESSES)
    return()
endif()

# Each harness is a single .cpp + libFuzzer's main. We assume the
# caller passed `-fsanitize=fuzzer,address,undefined` (or equivalent)
# in CMAKE_CXX_FLAGS; CMake itself doesn't gate on the compiler. A
# wrong-compiler config will fail at link time with a clear missing-
# symbol error.
function(st_add_fuzz_harness name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        st::core
        st::rom
        st::defs
        st::library
        tomlplusplus::tomlplusplus
    )
    target_compile_features(${name} PRIVATE cxx_std_23)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

st_add_fuzz_harness(fuzz_rom_load    fuzz_rom_load.cpp)
st_add_fuzz_harness(fuzz_ptm_decode  fuzz_ptm_decode.cpp)
st_add_fuzz_harness(fuzz_toml_load   fuzz_toml_load.cpp)
