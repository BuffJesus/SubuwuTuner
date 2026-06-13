// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// libFuzzer harness for `st::library::decode_ptm_xml`. Drives the
// PrivateData XML parser with random byte sequences; same contract
// as fuzz_rom_load — rejection is fine, crashes are defects. The
// duplicate-rom_offset invariant pinned in patch_decoder.hpp gives
// the harness a meaningful target: a fuzz find that produces
// duplicate offsets is informative, not a defect, but a crash on
// any input shape is.

#include "st/library/patch_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
    std::string_view const xml{reinterpret_cast<char const *>(data), size};
    (void)st::library::decode_ptm_xml(xml);
    return 0;
}
