// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/cobb_gateway.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

TEST_CASE("COBB CAN Gateway AES-128 key bytes match the RE wave 5 extraction",
          "[ecu][cobb_gateway][aes]") {
    // The key extracted by the analyst (RE wave 5 §ε4). If any byte
    // drifts, either the constant was transcribed wrong or the
    // Gateway firmware rotated its key — both load-bearing for any
    // future Gateway-protocol support.
    constexpr std::array<std::uint8_t, 16> kExpected = {
        0xf1, 0x03, 0xa6, 0x35, 0x2a, 0x5a, 0x45, 0xd1,
        0x21, 0x7b, 0xa2, 0x46, 0xfd, 0xda, 0xdf, 0x18,
    };
    REQUIRE(st::ecu::cobb_gateway::kCobbCanGatewayAesKey == kExpected);
}
