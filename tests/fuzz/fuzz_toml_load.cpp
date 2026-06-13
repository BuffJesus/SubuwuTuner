// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// libFuzzer harness for tomlplusplus's `toml::parse` (the parse path
// the definition-pack loader uses internally via `Definition::from_file`).
// We feed bytes directly to `toml::parse` rather than writing to a
// temp file + reading back; the parser surface is the same and we
// avoid filesystem overhead per iteration.

#include <toml++/toml.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
    std::string_view const text{reinterpret_cast<char const *>(data), size};
    try {
        // toml++ throws toml::parse_error on malformed input; the
        // harness's job is to confirm no crash / UB / sanitizer trip
        // beyond the typed exception.
        (void)toml::parse(text);
    } catch (toml::parse_error const &) {
        // Expected.
    } catch (...) {
        // Anything else (including std::bad_alloc, runtime_error,
        // logic_error) is an unexpected failure mode worth reporting.
        // libFuzzer treats an uncaught exception as a crash, which is
        // what we want — let it propagate.
        throw;
    }
    return 0;
}
