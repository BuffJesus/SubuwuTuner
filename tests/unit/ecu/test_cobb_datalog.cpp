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

TEST_CASE("AP v1.7.4.2 layout exposes 31 signals across 3 DIDs",
          "[ecu][cobb][datalog][layout]") {
    auto const layout = cb::ap_v1_7_4_2_layout();
    REQUIRE(layout.size() == 31);
    // v1.7.4.2 only covers F300/F301/F302 (CCF Gen2 pre-expansion).
    for (auto const &s : layout) {
        REQUIRE((s.did == 0xF300 || s.did == 0xF301 || s.did == 0xF302));
    }
}

TEST_CASE("ap_layout dispatches to the right firmware",
          "[ecu][cobb][datalog][layout]") {
    REQUIRE(cb::ap_layout(cb::CobbApFirmware::V1_7_4_2_CCF_Gen2).size() == 31);
    REQUIRE(cb::ap_layout(cb::CobbApFirmware::V1_7_6_0_CCF_Gen3).size() == 43);
}

TEST_CASE("AP v1.7.6.0 layout offsets fit within each DID's payload width",
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

TEST_CASE("AP layouts carry RAM addresses + scaling expressions",
          "[ecu][cobb][datalog][layout][ram]") {
    // Each entry must name a non-empty scaling expression and a RAM
    // address in the LF79103P RAM band (0xFFF8xxxx — analyst's join
    // sourced every address from the firmware's live-signals catalog).
    for (auto const &s : cb::ap_v1_7_4_2_layout()) {
        REQUIRE_FALSE(s.scaling.empty());
        REQUIRE(s.ram_address >= 0xFFF80000u);
        REQUIRE(s.ram_address < 0xFFF90000u);
    }
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        REQUIRE_FALSE(s.scaling.empty());
        REQUIRE(s.ram_address >= 0xFFF80000u);
        REQUIRE(s.ram_address < 0xFFF90000u);
    }
}

TEST_CASE("find_signal hits a known F303 RPM mapping (v1.7.6.0)",
          "[ecu][cobb][datalog][layout]") {
    auto const *rpm = cb::find_signal(0xF303, 4);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    REQUIRE(rpm->scaling == "(x/5.12)");
    REQUIRE(rpm->ram_address == 0xFFF8D424u);
    REQUIRE(rpm->storage == cb::CobbSignalStorage::Uint16);
}

TEST_CASE("find_signal hits a known F301 RPM mapping (v1.7.4.2)",
          "[ecu][cobb][datalog][layout]") {
    auto const *rpm = cb::find_signal(
        cb::CobbApFirmware::V1_7_4_2_CCF_Gen2, 0xF301, 17);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    // v1.7.4.2 packs RPM at F301:17 (Gen2 layout); v1.7.6.0 moved it
    // to F303:4 (Gen3 layout). The RAM address is unchanged across
    // firmwares because the underlying calibration is the same.
    REQUIRE(rpm->ram_address == 0xFFF8D424u);
}

TEST_CASE("find_signal returns nullptr for off-table position",
          "[ecu][cobb][datalog][layout]") {
    REQUIRE(cb::find_signal(0xF300, 99) == nullptr);
    REQUIRE(cb::find_signal(0xF399, 0) == nullptr);
    REQUIRE(cb::find_signal(cb::CobbApFirmware::V1_7_4_2_CCF_Gen2,
                            0xF399, 0) == nullptr);
}
