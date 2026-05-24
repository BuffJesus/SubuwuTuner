// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/subaru_security.hpp"

#include "st/core/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace st::ecu::subaru {

namespace {

// Shared failure message — keeps the user-facing diagnostic consistent
// across the three stubs and points at the same remediation steps.
constexpr char const *kStubMsg =
    "subaru security key stub — algorithm not provided. "
    "See src/ecu/include/st/ecu/subaru_security.hpp for how to plug in a "
    "license-compatible implementation via Flasher::set_security_key_fn(). "
    "Until then SecurityAccess will fail with this NotImplemented error.";

[[nodiscard]] Result<std::vector<std::uint8_t>>
not_implemented(std::span<std::uint8_t const> /*seed*/) {
    return failure(ErrorCode::NotImplemented, std::string{kStubMsg});
}

} // namespace

Result<std::vector<std::uint8_t>> ssmcan1_key_stub(std::span<std::uint8_t const> seed) {
    return not_implemented(seed);
}

Result<std::vector<std::uint8_t>> ssmk1_key_stub(std::span<std::uint8_t const> seed) {
    return not_implemented(seed);
}

Result<std::vector<std::uint8_t>> cy1_aes_key_stub(std::span<std::uint8_t const> seed) {
    return not_implemented(seed);
}

} // namespace st::ecu::subaru
