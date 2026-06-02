// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_VALIDATOR_HPP
#define ST_CORE_EXT_VALIDATOR_HPP

#include "st/core/diagnostic.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace st::ext {

// Validator — a stateless functor that inspects a byte buffer (a ROM, a
// region within a ROM, a definition pack body, a log frame, ...) and
// emits zero or more Diagnostics describing what it found.
//
// Stateless means: implementations should not retain per-call state.
// Hosts call `validate()` on many buffers from many threads; correctness
// must not depend on call order.
//
// `context` is an optional, opaque marker string the host attaches to
// describe where the buffer came from (e.g. "rom:0x1234..0x1244").
// Implementations may include it in diagnostic messages.
//
// Idea-level inspiration: the analyzer chain common to many static-
// analysis frameworks. Implementation is original.
class Validator {
public:
    virtual ~Validator() = default;

    // Stable, unique identifier for this validator. Used for logging,
    // configuration, and selective enabling. Should be a-z + dashes
    // (e.g. "axis-monotonicity", "value-range-fuel-base").
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Human-readable name for UI surfaces. May change between versions.
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;

    // Inspect `bytes` and return any findings. Empty vector means "no
    // issues". Implementations MUST NOT throw — failures are diagnostics
    // at `Severity::Warning` or `Severity::Blocker`.
    [[nodiscard]] virtual std::vector<Diagnostic>
    validate(std::span<std::uint8_t const> bytes,
             std::string_view context = {}) const = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_VALIDATOR_HPP
