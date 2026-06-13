// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the LF79103P region map and the classify / summarize
// pipeline. Pinned address values come from
// `docs/35-tuner-overlay-architecture.md` §"Per-ISA region addresses".

#include "st/devices/ets/architectural_classifier.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_CASE("classifier: LF79103P dead-fill region (0x078bc..0x10000) maps to TunerAddition",
          "[devices][ap3][classifier]") {
    auto const &map = st::devices::ets::default_lf79103p_layer_map();
    using L = st::devices::ets::Layer;
    REQUIRE(st::devices::ets::classify(map, 0x078BC) == L::TunerAddition);
    REQUIRE(st::devices::ets::classify(map, 0x08000) == L::TunerAddition);
    REQUIRE(st::devices::ets::classify(map, 0x0C000) == L::TunerAddition);
    REQUIRE(st::devices::ets::classify(map, 0x0FFFF) == L::TunerAddition);
    // 0x10000 is the first byte AFTER the dead-fill region.
    REQUIRE(st::devices::ets::classify(map, 0x10000) != L::TunerAddition);
}

TEST_CASE("classifier: LF79103P primary code region (0x60000..0x80000)",
          "[devices][ap3][classifier]") {
    auto const &map = st::devices::ets::default_lf79103p_layer_map();
    using L = st::devices::ets::Layer;
    REQUIRE(st::devices::ets::classify(map, 0x60000) == L::PrimaryCode);
    REQUIRE(st::devices::ets::classify(map, 0x7FFFF) == L::PrimaryCode);
    REQUIRE(st::devices::ets::classify(map, 0x80000) != L::PrimaryCode);
}

TEST_CASE("classifier: LF79103P secondary code region (0xC0000..0xD0000)",
          "[devices][ap3][classifier]") {
    auto const &map = st::devices::ets::default_lf79103p_layer_map();
    using L = st::devices::ets::Layer;
    REQUIRE(st::devices::ets::classify(map, 0xC0000) == L::SecondaryCode);
    REQUIRE(st::devices::ets::classify(map, 0xCFFFF) == L::SecondaryCode);
    REQUIRE(st::devices::ets::classify(map, 0xD0000) != L::SecondaryCode);
}

TEST_CASE("classifier: LF79103P main calibration + UDS retarget",
          "[devices][ap3][classifier]") {
    auto const &map = st::devices::ets::default_lf79103p_layer_map();
    using L = st::devices::ets::Layer;
    REQUIRE(st::devices::ets::classify(map, 0x12000) == L::MainCalibration);
    REQUIRE(st::devices::ets::classify(map, 0x14100) == L::MainCalibration);
    REQUIRE(st::devices::ets::classify(map, 0x1FF100) == L::UdsDispatchRetarget);
    REQUIRE(st::devices::ets::classify(map, 0x1FFFE0) == L::RomTail);
}

TEST_CASE("classifier: addresses outside every range fall to Uncharacterized",
          "[devices][ap3][classifier]") {
    auto const &map = st::devices::ets::default_lf79103p_layer_map();
    using L = st::devices::ets::Layer;
    // 0x10000..0x12000 is the gap between dead-fill and main cal.
    REQUIRE(st::devices::ets::classify(map, 0x10000) == L::Uncharacterized);
    REQUIRE(st::devices::ets::classify(map, 0x11FFF) == L::Uncharacterized);
    // 0x200000 is past the 2 MB ROM end.
    REQUIRE(st::devices::ets::classify(map, 0x200000) == L::Uncharacterized);
    REQUIRE(st::devices::ets::classify(map, 0xFFFFFFFFU) == L::Uncharacterized);
}

TEST_CASE("classifier: classify_patches preserves input order",
          "[devices][ap3][classifier]") {
    std::vector<std::uint32_t> const offsets{0x14000, 0x14100, 0x08000, 0x1FF100};
    std::vector<std::uint32_t> const lengths{100, 200, 50, 300};
    auto cls =
        st::devices::ets::classify_patches(st::devices::ets::default_lf79103p_layer_map(),
                                           offsets, lengths);
    REQUIRE(cls.size() == 4);
    using L = st::devices::ets::Layer;
    REQUIRE(cls[0].layer == L::MainCalibration);
    REQUIRE(cls[1].layer == L::MainCalibration);
    REQUIRE(cls[2].layer == L::TunerAddition);
    REQUIRE(cls[3].layer == L::UdsDispatchRetarget);
    REQUIRE(cls[0].rom_offset == 0x14000);
    REQUIRE(cls[0].length == 100);
}

TEST_CASE("classifier: classify_patches returns empty on size mismatch",
          "[devices][ap3][classifier]") {
    std::vector<std::uint32_t> const offsets{0x14000, 0x14100};
    std::vector<std::uint32_t> const lengths{100};
    auto cls =
        st::devices::ets::classify_patches(st::devices::ets::default_lf79103p_layer_map(),
                                           offsets, lengths);
    REQUIRE(cls.empty());
}

TEST_CASE("classifier: summarize aggregates and sorts by total_bytes",
          "[devices][ap3][classifier]") {
    std::vector<std::uint32_t> const offsets{0x14000, 0x14100, 0x08000, 0x1FF100};
    std::vector<std::uint32_t> const lengths{100, 200, 50, 300};
    auto cls =
        st::devices::ets::classify_patches(st::devices::ets::default_lf79103p_layer_map(),
                                           offsets, lengths);
    auto summary = st::devices::ets::summarize(cls);
    REQUIRE(summary.size() == 3);
    using L = st::devices::ets::Layer;
    // UDS retarget (300) sorts first, then MainCalibration (300), then TunerAddition (50).
    REQUIRE(summary[0].total_bytes == 300);
    REQUIRE(summary[1].total_bytes == 300);
    REQUIRE(summary[2].layer == L::TunerAddition);
    REQUIRE(summary[2].total_bytes == 50);
    REQUIRE(summary[2].patch_count == 1);

    // Find the MainCalibration row regardless of sort tie-break order.
    bool seen_main = false;
    for (auto const &s : summary) {
        if (s.layer == L::MainCalibration) {
            REQUIRE(s.patch_count == 2);
            REQUIRE(s.total_bytes == 300);
            seen_main = true;
        }
    }
    REQUIRE(seen_main);
}

TEST_CASE("classifier: summarize on empty input returns empty",
          "[devices][ap3][classifier]") {
    auto summary = st::devices::ets::summarize({});
    REQUIRE(summary.empty());
}

TEST_CASE("classifier: layer_label returns non-empty for every enumerator",
          "[devices][ap3][classifier]") {
    using L = st::devices::ets::Layer;
    for (auto layer : {L::HeaderBootSecurity, L::EarlyCalTables, L::TunerAddition,
                       L::MainCalibration, L::AlignmentPad, L::SecurityChecksumApproach,
                       L::UdsDispatchRetarget, L::RomTail, L::PrimaryCode,
                       L::SecondaryCode, L::Uncharacterized}) {
        REQUIRE_FALSE(st::devices::ets::layer_label(layer).empty());
    }
}
