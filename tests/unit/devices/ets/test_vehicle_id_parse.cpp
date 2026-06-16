// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for parse_vehicle_id_from_device_settings — the heuristic
// extractor that pulls the machine-readable vehicle code (e.g.
// "SUBA_US_WRXM_CF_17_F") out of the cmd 0x03 DeviceSettings body.
// Live-verified against SUB0484551 firmware v1.7.6.0-28785 on
// 2026-06-13 where the body carries the user's `SUBA_US_WRXM_CF_17_F`
// at offset 0x38 prefixed by a u32 LE length of 20.
//
// Wires into the L5 agreement chip (AP vs project vehicle match).

#include "st/devices/ets/client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

// Build a synthetic body matching the empirical cmd 0x03 layout: a
// Boost-archive prefix + filler + u32 LE length + N-byte uppercase-
// ASCII vehicle code. Real bodies have additional fields after; the
// extractor only cares about finding the first market-marked code.
std::vector<std::uint8_t> make_body_with_vehicle_id(std::string const &id,
                                                     std::size_t prefix_pad = 56) {
    std::vector<std::uint8_t> out;
    out.reserve(prefix_pad + 4 + id.size() + 16);
    // Filler that doesn't accidentally look like a length-prefixed
    // ASCII run with a market marker. Mix of small / zero / high bytes.
    for (std::size_t i = 0; i < prefix_pad; ++i) {
        out.push_back(static_cast<std::uint8_t>((i * 7) & 0xFF));
    }
    auto const len = static_cast<std::uint32_t>(id.size());
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    for (char c : id) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    // Trailing filler.
    for (int i = 0; i < 16; ++i) {
        out.push_back(0xFF);
    }
    return out;
}

} // namespace

TEST_CASE("parse_vehicle_id: extracts SUBA_US_ code at empirical offset",
          "[devices][ap3][vehicle_id]") {
    auto body = make_body_with_vehicle_id("SUBA_US_WRXM_CF_17_F");
    auto const r =
        st::devices::ets::parse_vehicle_id_from_device_settings(body);
    REQUIRE(r.has_value());
    CHECK(*r == "SUBA_US_WRXM_CF_17_F");
}

TEST_CASE("parse_vehicle_id: extracts JDM SUBA_JP_ code",
          "[devices][ap3][vehicle_id]") {
    auto body = make_body_with_vehicle_id("SUBA_JP_VAB_18_E");
    auto const r =
        st::devices::ets::parse_vehicle_id_from_device_settings(body);
    REQUIRE(r.has_value());
    CHECK(*r == "SUBA_JP_VAB_18_E");
}

TEST_CASE("parse_vehicle_id: extracts EU + AU market codes",
          "[devices][ap3][vehicle_id]") {
    auto eu = make_body_with_vehicle_id("EVOJ_EU_TENTH_X");
    auto const r_eu =
        st::devices::ets::parse_vehicle_id_from_device_settings(eu);
    REQUIRE(r_eu.has_value());
    CHECK(*r_eu == "EVOJ_EU_TENTH_X");
    auto au = make_body_with_vehicle_id("SUBA_AU_WRX_20_A");
    auto const r_au =
        st::devices::ets::parse_vehicle_id_from_device_settings(au);
    REQUIRE(r_au.has_value());
    CHECK(*r_au == "SUBA_AU_WRX_20_A");
}

TEST_CASE("parse_vehicle_id: nullopt on body with no market marker",
          "[devices][ap3][vehicle_id]") {
    // Length-prefixed ASCII but no _US_/_JP_/_EU_/_AU_ — heuristic
    // requires the market marker to avoid false positives on random
    // uppercase strings elsewhere in the body.
    auto body = make_body_with_vehicle_id("RANDOM_NOMARKER_STR");
    auto const r =
        st::devices::ets::parse_vehicle_id_from_device_settings(body);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("parse_vehicle_id: nullopt on too-short bodies",
          "[devices][ap3][vehicle_id]") {
    std::vector<std::uint8_t> tiny{0x01, 0x02};
    auto const r =
        st::devices::ets::parse_vehicle_id_from_device_settings(tiny);
    CHECK_FALSE(r.has_value());
    std::vector<std::uint8_t> empty;
    auto const r2 =
        st::devices::ets::parse_vehicle_id_from_device_settings(empty);
    CHECK_FALSE(r2.has_value());
}

TEST_CASE("parse_vehicle_id: nullopt on length out of plausible range",
          "[devices][ap3][vehicle_id]") {
    // Length = 4 (below 8 minimum) — too short, no extract even though
    // the bytes are ASCII.
    std::vector<std::uint8_t> body{0x04, 0x00, 0x00, 0x00,
                                     '_', 'U', 'S', '_'};
    auto const r =
        st::devices::ets::parse_vehicle_id_from_device_settings(body);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("parse_vehicle_id: picks the FIRST market-marked candidate",
          "[devices][ap3][vehicle_id]") {
    // Two candidates: SUBA_US first, SUBA_JP later. Should return
    // the first. Both must be length-prefixed correctly.
    std::vector<std::uint8_t> body;
    auto append_lp = [&](std::string const &s) {
        auto const len = static_cast<std::uint32_t>(s.size());
        body.push_back(static_cast<std::uint8_t>(len & 0xFF));
        body.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        body.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
        body.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
        for (char c : s) {
            body.push_back(static_cast<std::uint8_t>(c));
        }
    };
    // Padding so the first u32 isn't at offset 0.
    for (int i = 0; i < 32; ++i) {
        body.push_back(0x00);
    }
    append_lp("SUBA_US_WRXM_17_F");
    append_lp("SUBA_JP_VAB_18_E");
    auto const r =
        st::devices::ets::parse_vehicle_id_from_device_settings(body);
    REQUIRE(r.has_value());
    CHECK(*r == "SUBA_US_WRXM_17_F");
}
