// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// libFuzzer harness for `st::Rom::load_bytes`. Drives the ROM loader
// with random byte sequences; the contract is "no crash, no UB, no
// sanitizer trips for any input the loader rejects with an error
// Result". A panic or sanitizer hit is a defect.

#include "st/rom.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
    std::vector<std::uint8_t> bytes(data, data + size);
    // The loader is allowed to reject — we're fuzzing the rejection
    // path, not the success path. The contract is "no crash."
    (void)st::Rom::load_bytes(std::move(bytes));
    return 0;
}
