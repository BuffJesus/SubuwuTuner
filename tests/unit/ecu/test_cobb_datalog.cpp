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

TEST_CASE("AP v1.7.6.0 layout exposes 44 signals across the 5 DIDs",
          "[ecu][cobb][datalog][layout]") {
    auto const layout = cb::ap_v1_7_6_0_layout();
    REQUIRE(layout.size() == 44);
    // Every entry's DID is in the polled set.
    for (auto const &s : layout) {
        REQUIRE(std::find(cb::kDidSet.begin(), cb::kDidSet.end(),
                          s.did) != cb::kDidSet.end());
    }
}

TEST_CASE("AP v1.7.6.0 has 12 R²-verified entries",
          "[ecu][cobb][datalog][layout][verified]") {
    auto const layout = cb::ap_v1_7_6_0_layout();
    auto const verified_count = std::count_if(
        layout.begin(), layout.end(), [](cb::CobbSignalLayout const &s) {
            return s.verification == cb::CobbVerification::Verified;
        });
    REQUIRE(verified_count == 12);
}

TEST_CASE("All Verified entries carry non-zero wire scale + RAM address",
          "[ecu][cobb][datalog][layout][verified]") {
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Verified)
            continue;
        REQUIRE(s.ram_address != 0u);
        REQUIRE(s.cobb_scale != 0.0);
    }
}

TEST_CASE("Hypothesized entries carry zero wire scale (sentinel)",
          "[ecu][cobb][datalog][layout][hypothesis]") {
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Hypothesized)
            continue;
        REQUIRE(s.cobb_scale == 0.0);
        REQUIRE(s.cobb_offset == 0.0);
    }
    // v1.7.4.2 is entirely Hypothesized.
    for (auto const &s : cb::ap_v1_7_4_2_layout()) {
        REQUIRE(s.verification == cb::CobbVerification::Hypothesized);
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
    REQUIRE(cb::ap_layout(cb::CobbApFirmware::V1_7_6_0_CCF_Gen3).size() == 44);
}

TEST_CASE("AP v1.7.4.2 Hypothesized offsets fit within payload widths",
          "[ecu][cobb][datalog][layout]") {
    // v1.7.4.2 is the only firmware where the hypothesis layout was
    // bounded by the bus-check.log-derived payload widths. v1.7.6.0
    // Verified entries (e.g. TD Boost Error Ext at F300:21 u16_le)
    // already exceed the 22-byte F300 width per the analyst's R²-fit
    // — investigation pending whether F300 is actually wider on
    // v1.7.6.0 or the analyst's byte-numbering convention differs.
    auto const layout = cb::ap_v1_7_4_2_layout();
    for (auto const &s : layout) {
        auto const it = std::find_if(
            cb::kDidPayloads.begin(), cb::kDidPayloads.end(),
            [&](cb::DidPayload const &p) { return p.did == s.did; });
        REQUIRE(it != cb::kDidPayloads.end());
        std::uint8_t const width =
            (s.storage == cb::CobbSignalStorage::Uint16 ||
             s.storage == cb::CobbSignalStorage::Int16 ||
             s.storage == cb::CobbSignalStorage::Uint16Le ||
             s.storage == cb::CobbSignalStorage::Int16Le)
                ? 2u
                : 1u;
        REQUIRE(s.byte_offset + width <= it->bytes);
    }
}

TEST_CASE("AP layouts carry RAM addresses + scaling expressions",
          "[ecu][cobb][datalog][layout][ram]") {
    // Each entry names a non-empty scaling expression and a
    // catalog-resolved RAM address. Most addresses fall in the
    // 0xFFF8xxxx band; Vehicle Speed at 0xFFF99835 is an exception.
    for (auto const &s : cb::ap_v1_7_4_2_layout()) {
        REQUIRE_FALSE(s.scaling.empty());
        REQUIRE(s.ram_address >= 0xFFF80000u);
    }
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        REQUIRE_FALSE(s.scaling.empty());
        REQUIRE(s.ram_address >= 0xFFF80000u);
    }
}

TEST_CASE("find_signal returns the F301:6 Verified RPM mapping (v1.7.6.0)",
          "[ecu][cobb][datalog][layout][verified]") {
    // R²-fit relocated RPM from the CSV-column-order hypothesis
    // (F303:4) to F301:6. Verified entries supersede.
    auto const *rpm = cb::find_signal(0xF301, 6);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    REQUIRE(rpm->ram_address == 0xFFF8D424u);
    REQUIRE(rpm->verification == cb::CobbVerification::Verified);
    REQUIRE(rpm->cobb_scale == 0.21246);
    REQUIRE(rpm->cobb_offset == -130.370);
}

TEST_CASE("Vehicle Speed Verified entry sits at F300:13 with the LE storage",
          "[ecu][cobb][datalog][layout][verified]") {
    auto const *vs = cb::find_signal(0xF300, 13);
    REQUIRE(vs != nullptr);
    REQUIRE(vs->name == "Vehicle Speed");
    REQUIRE(vs->storage == cb::CobbSignalStorage::Uint16Le);
    REQUIRE(vs->ram_address == 0xFFF99835u);
    REQUIRE(vs->verification == cb::CobbVerification::Verified);
}

TEST_CASE("find_signal hits a known F301 RPM mapping (v1.7.4.2)",
          "[ecu][cobb][datalog][layout]") {
    auto const *rpm = cb::find_signal(
        cb::CobbApFirmware::V1_7_4_2_CCF_Gen2, 0xF301, 17);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    // v1.7.4.2 packs RPM at F301:17 (Gen2 layout, hypothesis);
    // v1.7.6.0 packs it at F301:6 (verified). The RAM address is
    // unchanged across firmwares because the underlying calibration
    // is the same.
    REQUIRE(rpm->ram_address == 0xFFF8D424u);
}

TEST_CASE("find_signal returns nullptr for off-table position",
          "[ecu][cobb][datalog][layout]") {
    REQUIRE(cb::find_signal(0xF300, 99) == nullptr);
    REQUIRE(cb::find_signal(0xF399, 0) == nullptr);
    REQUIRE(cb::find_signal(cb::CobbApFirmware::V1_7_4_2_CCF_Gen2,
                            0xF399, 0) == nullptr);
}
