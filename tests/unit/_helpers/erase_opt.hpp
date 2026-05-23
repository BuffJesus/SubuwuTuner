// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// The 9-byte eraseMemory option record the Flasher orchestrator emits
// (mirrors `build_erase_option_record` in `src/flash/src/flash.cpp`):
// aLFI = 0x44 (4-byte size, 4-byte address) followed by 32-bit address
// then 32-bit size, all big-endian.
//
// Used by multiple test files (`test_flash.cpp`,
// `test_cancellation_invariants.cpp`) to seed MockTransport expectations
// for routine_control(eraseMemory). Lives here so the encoding stays
// single-sourced — if the format ever changes in flash.cpp, only one
// helper needs to update.

#ifndef ST_TESTS_HELPERS_ERASE_OPT_HPP
#define ST_TESTS_HELPERS_ERASE_OPT_HPP

#include <cstdint>
#include <vector>

namespace st::test {

[[nodiscard]] inline std::vector<std::uint8_t> erase_opt(std::uint32_t addr,
                                                         std::uint32_t size) {
    return {
        0x44,
        static_cast<std::uint8_t>((addr >> 24) & 0xFFU),
        static_cast<std::uint8_t>((addr >> 16) & 0xFFU),
        static_cast<std::uint8_t>((addr >> 8) & 0xFFU),
        static_cast<std::uint8_t>(addr & 0xFFU),
        static_cast<std::uint8_t>((size >> 24) & 0xFFU),
        static_cast<std::uint8_t>((size >> 16) & 0xFFU),
        static_cast<std::uint8_t>((size >> 8) & 0xFFU),
        static_cast<std::uint8_t>(size & 0xFFU),
    };
}

} // namespace st::test

#endif // ST_TESTS_HELPERS_ERASE_OPT_HPP
