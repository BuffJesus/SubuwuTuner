# Gated cipher source per docs/34-cobb-ap-as-tune-vault.md +
# specs/cobb-ap3-tier3-cipher-implementation-spec.md. When OFF
# (default), the .cpp files compile their NotImplemented stubs and
# return PolicyDenied for every cipher operation. When ON, the
# `.ptm` XTEA-CBC outer + base64 envelope (Session 1) compile in;
# AES-256-CTR + bzip2 inner (Session 2) and Blowfish-CTR-double OTA
# (Session 3) are still NotImplemented at this stage.
#
# Mirrors the precedent set by ST_ENABLE_BULK_REFLASH_CIPHER in the
# top-level CMakeLists.txt — same two-step arming model (build flag +
# runtime --enable-cobb-ap-cipher).
option(ST_ENABLE_COBB_AP_CIPHER
    "Build the AP3 .ptm + OTA cipher source. OFF by default; see \
docs/34-cobb-ap-as-tune-vault.md for the two-step arming model and \
rationale. When ON, the cipher key constants compile into the \
binary's .rodata; when OFF, the cipher TU emits NotImplemented stubs."
    OFF)

# Optional second-layer gate: enable encrypt_ptm (the write/re-pack
# direction) on top of the read-only decrypt path. Default OFF even
# when COBB_AP_CIPHER=ON — the asymmetric posture frames SubuwuTuner
# as a .ptm reader+analyzer for v1.0; the developer who wants the
# full read+write surface opts in explicitly.
#
# When ON, bzip2 compress sources (blocksort.c + compress.c) are
# pulled into the build; the compress_stubs.c shim is dropped because
# the real symbols now resolve at link time.
option(ST_ENABLE_COBB_AP_PTM_REWRITE
    "Build the .ptm re-pack (encrypt_ptm) path. Requires \
ST_ENABLE_COBB_AP_CIPHER=ON. OFF by default — encrypt_ptm returns \
NotImplemented when this flag is OFF, even with the cipher built. \
When ON, bzip2 compress sources are pulled in for the layer-4 \
re-pack."
    OFF)
if(ST_ENABLE_COBB_AP_PTM_REWRITE AND NOT ST_ENABLE_COBB_AP_CIPHER)
    message(FATAL_ERROR
        "ST_ENABLE_COBB_AP_PTM_REWRITE=ON requires ST_ENABLE_COBB_AP_CIPHER=ON. "
        "The rewrite path depends on the cipher chain being built.")
endif()

add_library(st_devices_ap3 STATIC
    src/file_info.cpp
    src/client.cpp
    src/ptm_cipher.cpp
    src/architectural_classifier.cpp
    # Session 2 cipher wrappers (always compile — they pick OFF-path
    # stubs when ST_AP3_HAVE_CIPHER is undefined).
    src/aes256_ctr.cpp
    src/bzip2_decompress.cpp
)

if(ST_ENABLE_COBB_AP_CIPHER)
    # The top-level project() only declares LANGUAGES CXX; enable C
    # here so the vendored tiny-AES-c + bzip2 sources can compile.
    # enable_language() is idempotent — if libusb-cmake already
    # enabled it earlier, this is a no-op; otherwise it sets up the C
    # compiler toolchain.
    enable_language(C)
    target_compile_definitions(st_devices_ap3 PRIVATE ST_AP3_HAVE_CIPHER=1)
    # Vendored third-party crypto sources per the dep survey at
    # specs/cobb-ap3-tier3-dep-survey.md. Compile + link only in the
    # cipher-ON build so the default Apache 2.0 binary doesn't ship
    # any of them.
    target_sources(st_devices_ap3 PRIVATE
        # tiny-AES-c (public domain — third_party/tiny_aes/unlicense.txt)
        third_party/tiny_aes/aes.c
        # bzip2 1.0.8 decompress (BSD-style bzip2 license — third_party/bzip2_dec/LICENSE-bzip2)
        third_party/bzip2_dec/bzlib.c
        third_party/bzip2_dec/decompress.c
        third_party/bzip2_dec/huffman.c
        third_party/bzip2_dec/crctable.c
        third_party/bzip2_dec/randtable.c
    )
    if(ST_ENABLE_COBB_AP_PTM_REWRITE)
        # Pull in bzip2 compress side; the real symbols replace the
        # stub shims so we don't include compress_stubs.c.
        target_compile_definitions(st_devices_ap3 PRIVATE ST_AP3_HAVE_PTM_REWRITE=1)
        target_sources(st_devices_ap3 PRIVATE
            third_party/bzip2_dec/blocksort.c
            third_party/bzip2_dec/compress.c
        )
    else()
        # Stub the 3 compress-side symbols bzlib.c references so the
        # link succeeds. Calling BZ2_bzCompress* would no-op silently;
        # the wrapped public APIs return NotImplemented before
        # reaching them anyway.
        target_sources(st_devices_ap3 PRIVATE
            third_party/bzip2_dec/compress_stubs.c
        )
    endif()
    target_include_directories(st_devices_ap3 PRIVATE
        third_party/tiny_aes
        third_party/bzip2_dec
    )
    # Configure tiny-AES-c at compile time for AES-256-CTR only.
    # AES128/192/256 select key length; CBC/ECB/CTR select modes.
    # The upstream defaults to AES128 + CBC + ECB + CTR; we want
    # only AES256 + CTR.
    target_compile_definitions(st_devices_ap3 PRIVATE
        AES128=0
        AES192=0
        AES256=1
        CBC=0
        ECB=0
        CTR=1
    )
    # bzip2 + tiny-AES-c are C with their own conventions — suppress
    # the project's strict C++-oriented warnings on those sources by
    # treating their headers as system includes (the SYSTEM bit is
    # already on the include_directories above) and disabling the
    # most aggressive warnings on the C compile.
    set_source_files_properties(
        third_party/tiny_aes/aes.c
        third_party/bzip2_dec/bzlib.c
        third_party/bzip2_dec/decompress.c
        third_party/bzip2_dec/huffman.c
        third_party/bzip2_dec/crctable.c
        third_party/bzip2_dec/randtable.c
        third_party/bzip2_dec/compress_stubs.c
        third_party/bzip2_dec/blocksort.c
        third_party/bzip2_dec/compress.c
        PROPERTIES
            COMPILE_OPTIONS "-w"
    )
endif()
add_library(st::devices_ap3 ALIAS st_devices_ap3)

target_include_directories(st_devices_ap3
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(st_devices_ap3 PUBLIC cxx_std_23)

target_link_libraries(st_devices_ap3
    PUBLIC
        st::core
        st::transport
    PRIVATE
        st::warnings
        st::sanitizers
)
