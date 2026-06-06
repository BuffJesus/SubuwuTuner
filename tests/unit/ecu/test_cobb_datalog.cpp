// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Pin the Cobb-AP datalog protocol constants + the AP v1.7.6.0 signal
// layout. Source of truth: analyst handoff
// HANDOFF-from-analyst-2026-06-06-cobb-datalog.md.

#include "st/ecu/cobb_datalog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>

namespace cb = st::ecu::cobb_datalog;

TEST_CASE("Cobb datalog protocol constants match bus-check.log",
          "[ecu][cobb][datalog]") {
    REQUIRE(cb::kRequestCanId == 0x7E0);
    REQUIRE(cb::kResponseCanId == 0x7E8);
    REQUIRE(cb::kReadDataByIdentifier == 0x22);
    REQUIRE(cb::kMedianPollIntervalMs == 39);
    REQUIRE(cb::kDidSet.size() == 5);
    REQUIRE(cb::kDidSet[0] == 0xF300);
    REQUIRE(cb::kDidSet[4] == 0xF304);
}

TEST_CASE("Cobb DID payload widths sum to 67 bytes",
          "[ecu][cobb][datalog]") {
    std::size_t total = 0;
    for (auto const &p : cb::kDidPayloads)
        total += p.bytes;
    REQUIRE(total == cb::kTotalPayloadBytes);
    REQUIRE(total == 67);
}

TEST_CASE("AP v1.7.6.0 layout exposes 43 signals across the 5 DIDs",
          "[ecu][cobb][datalog][layout]") {
    auto const layout = cb::ap_v1_7_6_0_layout();
    REQUIRE(layout.size() == 43);
    // Every entry's DID is in the polled set.
    for (auto const &s : layout) {
        REQUIRE(std::find(cb::kDidSet.begin(), cb::kDidSet.end(),
                          s.did) != cb::kDidSet.end());
    }
}

TEST_CASE("AP layout offsets fit within each DID's payload width",
          "[ecu][cobb][datalog][layout]") {
    auto const layout = cb::ap_v1_7_6_0_layout();
    for (auto const &s : layout) {
        auto const it = std::find_if(
            cb::kDidPayloads.begin(), cb::kDidPayloads.end(),
            [&](cb::DidPayload const &p) { return p.did == s.did; });
        REQUIRE(it != cb::kDidPayloads.end());
        std::uint8_t const width =
            (s.storage == cb::CobbSignalStorage::Uint16 ||
             s.storage == cb::CobbSignalStorage::Int16)
                ? 2u
                : 1u;
        REQUIRE(s.byte_offset + width <= it->bytes);
    }
}

TEST_CASE("find_signal hits a known F303 RPM mapping",
          "[ecu][cobb][datalog][layout]") {
    auto const *rpm = cb::find_signal(0xF303, 4);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    REQUIRE(rpm->scale == 1);
    REQUIRE(rpm->storage == cb::CobbSignalStorage::Uint16);
}

TEST_CASE("find_signal returns nullptr for off-table position",
          "[ecu][cobb][datalog][layout]") {
    REQUIRE(cb::find_signal(0xF300, 99) == nullptr);
    REQUIRE(cb::find_signal(0xF399, 0) == nullptr);
}
