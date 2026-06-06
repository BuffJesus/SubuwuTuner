// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Pin the Cobb-AP datalog protocol constants + the AP v1.7.6.0 signal
// layout. Source of truth: analyst handoff
// HANDOFF-from-analyst-2026-06-06-cobb-datalog.md.

#include "st/ecu/cobb_datalog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

TEST_CASE("v1.7.4.2 DID payload widths sum to 67 bytes",
          "[ecu][cobb][datalog]") {
    std::size_t total = 0;
    for (auto const &p : cb::kDidPayloadsV1_7_4_2)
        total += p.bytes;
    REQUIRE(total == cb::kTotalPayloadBytesV1_7_4_2);
    REQUIRE(total == 67);
    // Backward-compat alias matches v1.7.4.2.
    REQUIRE(cb::kTotalPayloadBytes == 67);
}

TEST_CASE("v1.7.6.0 DID payload widths sum to 75 bytes",
          "[ecu][cobb][datalog]") {
    std::size_t total = 0;
    for (auto const &p : cb::kDidPayloadsV1_7_6_0)
        total += p.bytes;
    REQUIRE(total == cb::kTotalPayloadBytesV1_7_6_0);
    REQUIRE(total == 75);
}

TEST_CASE("did_payloads() + total_payload_bytes() dispatch by firmware",
          "[ecu][cobb][datalog]") {
    REQUIRE(cb::did_payloads(cb::CobbApFirmware::V1_7_4_2_CCF_Gen2).size() == 5);
    REQUIRE(cb::did_payloads(cb::CobbApFirmware::V1_7_6_0_CCF_Gen3).size() == 5);
    REQUIRE(cb::total_payload_bytes(cb::CobbApFirmware::V1_7_4_2_CCF_Gen2) == 67);
    REQUIRE(cb::total_payload_bytes(cb::CobbApFirmware::V1_7_6_0_CCF_Gen3) == 75);
}

TEST_CASE("Every v1.7.6.0 Verified entry carries an AP monitor_id",
          "[ecu][cobb][datalog][verified][monitor_id]") {
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Verified)
            continue;
        REQUIRE_FALSE(s.monitor_id.empty());
        // monitor_id prefix matches ram_authoritative: SSM_* means
        // OEM-RAM authoritative, RAM_* means COBB-defined candidate.
        bool const is_ssm = s.monitor_id.starts_with("SSM_");
        REQUIRE(s.ram_authoritative == is_ssm);
    }
}

TEST_CASE("Every Verified RAM address sits in the LF79103P band",
          "[ecu][cobb][datalog][verified][lf79103p]") {
    // Guard against accidental cross-CID contamination: the const-
    // data ram_address values are LF79103P-specific by design. Any
    // future commit that adds a RAM address from a different CID's
    // catalog should land in a separate per-CID pack, not the
    // primary layout. LF79103P RAM lives in 0xFFF80000-0xFFFFFFFF.
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Verified)
            continue;
        REQUIRE(s.ram_address >= 0xFFF80000u);
        REQUIRE(s.ram_address <= 0xFFFFFFFFu);
    }
}

TEST_CASE("v1.7.6.0 has 6 SSM_* + 6 RAM_* Verified rows",
          "[ecu][cobb][datalog][verified][monitor_id]") {
    int ssm = 0;
    int ram = 0;
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Verified)
            continue;
        if (s.ram_authoritative)
            ++ssm;
        else
            ++ram;
    }
    REQUIRE(ssm == 6);
    REQUIRE(ram == 6);
}

TEST_CASE("v1.7.6.0 Verified positions all fit within v1.7.6.0 payload widths",
          "[ecu][cobb][datalog][verified][widths]") {
    // Now that v1.7.6.0 F300 is 26 bytes (not 22), the F300:21 u16_le
    // TD Boost Error Ext fits cleanly within the DID.
    auto const widths = cb::did_payloads(cb::CobbApFirmware::V1_7_6_0_CCF_Gen3);
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Verified)
            continue;
        auto const it = std::find_if(
            widths.begin(), widths.end(),
            [&](cb::DidPayload const &p) { return p.did == s.did; });
        REQUIRE(it != widths.end());
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

// ---- decode_signal ------------------------------------------------------
//
// Mirrors the analyst's demo_decode_sniff.py end-to-end demo. Each test
// case picks a Verified entry, constructs a synthetic DID payload, and
// checks the decode produces a physically plausible value. Sources for
// the canonical idle/cruise values: the analyst's dmann sniff (746 RPM
// at sniff start, idle range 700-950).

namespace {

// Build a Uint16 big-endian sample at `off` in a buf of `did_bytes`.
std::array<std::uint8_t, 32> make_u16_be_payload(std::size_t did_bytes,
                                                  std::size_t off,
                                                  std::uint16_t value) {
    std::array<std::uint8_t, 32> buf{};
    (void)did_bytes;
    buf[off] = static_cast<std::uint8_t>(value >> 8);
    buf[off + 1] = static_cast<std::uint8_t>(value & 0xFF);
    return buf;
}

std::array<std::uint8_t, 32> make_u16_le_payload(std::size_t did_bytes,
                                                  std::size_t off,
                                                  std::uint16_t value) {
    std::array<std::uint8_t, 32> buf{};
    (void)did_bytes;
    buf[off] = static_cast<std::uint8_t>(value & 0xFF);
    buf[off + 1] = static_cast<std::uint8_t>(value >> 8);
    return buf;
}

} // namespace

TEST_CASE("decode_signal: RPM at F301:6 → idle range",
          "[ecu][cobb][datalog][decode]") {
    auto const *rpm = cb::find_signal(0xF301, 6);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->verification == cb::CobbVerification::Verified);
    // Reproduce ~746 RPM from a synthetic raw value. cobb_scale=0.21246,
    // cobb_offset=-130.370; 746 = raw*0.21246 - 130.370 → raw ≈ 4124.
    auto const payload = make_u16_be_payload(20, 6, 4124);
    auto const eng = cb::decode_signal(*rpm,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(746.0, 1.0));
}

TEST_CASE("decode_signal: Vehicle Speed u16_le respects little-endian",
          "[ecu][cobb][datalog][decode]") {
    auto const *vss = cb::find_signal(0xF300, 13);
    REQUIRE(vss != nullptr);
    REQUIRE(vss->storage == cb::CobbSignalStorage::Uint16Le);
    // raw=1100 → 1100*0.0098823 - 10.685 ≈ 0.185 mph (idle).
    auto const payload = make_u16_le_payload(26, 13, 1100);
    auto const eng = cb::decode_signal(*vss,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(0.185, 0.05));
}

TEST_CASE("decode_signal: Oil Temp u8 produces warm-idle value",
          "[ecu][cobb][datalog][decode]") {
    auto const *oil = cb::find_signal(0xF302, 0);
    REQUIRE(oil != nullptr);
    // raw=130 → 130*0.03105 + 208.990 ≈ 213.0 F (warm idle).
    std::array<std::uint8_t, 10> payload{};
    payload[0] = 130;
    auto const eng = cb::decode_signal(*oil,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(213.0, 0.1));
}

TEST_CASE("decode_signal: Battery Volts u8 produces 12-13V range",
          "[ecu][cobb][datalog][decode]") {
    auto const *batt = cb::find_signal(0xF301, 11);
    REQUIRE(batt != nullptr);
    // raw=70 → 70*0.0039108 + 12.177 ≈ 12.451 V.
    std::array<std::uint8_t, 20> payload{};
    payload[11] = 70;
    auto const eng = cb::decode_signal(*batt,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(12.45, 0.05));
}

TEST_CASE("decode_signal returns NaN for Hypothesized rows",
          "[ecu][cobb][datalog][decode][hypothesis]") {
    // Hypothesized entries have cobb_scale=0; decode_signal must refuse.
    for (auto const &s : cb::ap_v1_7_6_0_layout()) {
        if (s.verification != cb::CobbVerification::Hypothesized)
            continue;
        std::array<std::uint8_t, 32> payload{};
        auto const eng = cb::decode_signal(s, {payload.data(), payload.size()});
        REQUIRE(std::isnan(eng));
        break; // one is enough — invariant is a single conditional
    }
}

TEST_CASE("decode_signal returns NaN on short payload",
          "[ecu][cobb][datalog][decode][bounds]") {
    auto const *rpm = cb::find_signal(0xF301, 6);
    REQUIRE(rpm != nullptr);
    // Payload too short for byte_offset=6 + u16.
    std::array<std::uint8_t, 5> payload{};
    auto const eng = cb::decode_signal(*rpm,
                                       {payload.data(), payload.size()});
    REQUIRE(std::isnan(eng));
}

TEST_CASE("find_signal returns nullptr for off-table position",
          "[ecu][cobb][datalog][layout]") {
    REQUIRE(cb::find_signal(0xF300, 99) == nullptr);
    REQUIRE(cb::find_signal(0xF399, 0) == nullptr);
    REQUIRE(cb::find_signal(cb::CobbApFirmware::V1_7_4_2_CCF_Gen2,
                            0xF399, 0) == nullptr);
}
