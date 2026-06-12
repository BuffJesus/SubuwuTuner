// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Pin the aftermarket-datalogger SSM-extended DID protocol constants
// + the wide-layout signal map.

#include "st/ecu/extended_did_datalog.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace cb = st::ecu::extended_did_datalog;

TEST_CASE("Datalog protocol constants match the captured wire shape",
          "[ecu][did-datalog]") {
    REQUIRE(cb::kRequestCanId == 0x7E0);
    REQUIRE(cb::kResponseCanId == 0x7E8);
    REQUIRE(cb::kReadDataByIdentifier == 0x22);
    REQUIRE(cb::kMedianPollIntervalMs == 39);
    REQUIRE(cb::kDidSet.size() == 5);
    REQUIRE(cb::kDidSet[0] == 0xF300);
    REQUIRE(cb::kDidSet[4] == 0xF304);
}

TEST_CASE("Narrow DID payload widths sum to 67 bytes",
          "[ecu][did-datalog]") {
    std::size_t total = 0;
    for (auto const &p : cb::kDidPayloadsNarrow)
        total += p.bytes;
    REQUIRE(total == cb::kTotalPayloadBytesNarrow);
    REQUIRE(total == 67);
    // Backward-compat alias matches the narrow layout.
    REQUIRE(cb::kTotalPayloadBytes == 67);
}

TEST_CASE("Wide DID payload widths sum to 75 bytes",
          "[ecu][did-datalog]") {
    std::size_t total = 0;
    for (auto const &p : cb::kDidPayloadsWide)
        total += p.bytes;
    REQUIRE(total == cb::kTotalPayloadBytesWide);
    REQUIRE(total == 75);
}

TEST_CASE("did_payloads() + total_payload_bytes() dispatch by firmware",
          "[ecu][did-datalog]") {
    REQUIRE(cb::did_payloads(cb::DidLayoutVersion::Narrow).size() == 5);
    REQUIRE(cb::did_payloads(cb::DidLayoutVersion::Wide).size() == 5);
    REQUIRE(cb::total_payload_bytes(cb::DidLayoutVersion::Narrow) == 67);
    REQUIRE(cb::total_payload_bytes(cb::DidLayoutVersion::Wide) == 75);
}

TEST_CASE("Every wide-layout Verified entry carries a monitor_id",
          "[ecu][did-datalog][verified][monitor_id]") {
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Verified)
            continue;
        REQUIRE_FALSE(s.monitor_id.empty());
    }
}

TEST_CASE("Every Verified RAM address sits in the LF79103P catalog band",
          "[ecu][did-datalog][verified][lf79103p]") {
    // The ram_address axis is sourced from the LF79103P live-signals
    // catalog. (The byte-position axis was R²-verified against the
    // user's LF79101P-content car — separate confidence axes.) Any
    // future commit that adds a RAM address from a different CID's
    // catalog should land in a separate per-CID pack, not the
    // primary layout. LF79103P RAM lives in 0xFFF80000-0xFFFFFFFF.
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Verified)
            continue;
        REQUIRE(s.ram_address >= 0xFFF80000u);
        REQUIRE(s.ram_address <= 0xFFFFFFFFu);
    }
}

TEST_CASE("Wide-layout Verified split: 15 authoritative + 21 candidate RAM",
          "[ecu][did-datalog][verified][monitor_id]") {
    // Per-monitor classification has more nuance than the SSM_*/RAM_*
    // prefix would imply — e.g. SSM_BST_REL_EXT_SHDIT (Boost Extended)
    // is marked candidate per the corpus-wide cross-check.
    // Counts sourced directly from that TOML.
    int auth = 0;
    int cand = 0;
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Verified)
            continue;
        if (s.ram_authoritative)
            ++auth;
        else
            ++cand;
    }
    REQUIRE(auth == 15);
    REQUIRE(cand == 21);
}

TEST_CASE("Wide-layout Verified positions all fit within payload widths",
          "[ecu][did-datalog][verified][widths]") {
    // Now that wide-layout F300 is 26 bytes (not 22), the F300:21 u16_le
    // TD Boost Error Ext fits cleanly within the DID.
    auto const widths = cb::did_payloads(cb::DidLayoutVersion::Wide);
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Verified)
            continue;
        auto const it = std::find_if(
            widths.begin(), widths.end(),
            [&](cb::DidPayload const &p) { return p.did == s.did; });
        REQUIRE(it != widths.end());
        std::uint8_t const width =
            (s.storage == cb::SignalStorage::Uint16 ||
             s.storage == cb::SignalStorage::Int16 ||
             s.storage == cb::SignalStorage::Uint16Le ||
             s.storage == cb::SignalStorage::Int16Le)
                ? 2u
                : 1u;
        REQUIRE(s.byte_offset + width <= it->bytes);
    }
}

TEST_CASE("Wide layout exposes 36 signals across the 5 DIDs",
          "[ecu][did-datalog][layout]") {
    auto const layout = cb::v_wide_layout();
    REQUIRE(layout.size() == 36);
    // Every entry's DID is in the polled set.
    for (auto const &s : layout) {
        REQUIRE(std::find(cb::kDidSet.begin(), cb::kDidSet.end(),
                          s.did) != cb::kDidSet.end());
    }
}

TEST_CASE("Wide layout has 36 R^2-verified entries",
          "[ecu][did-datalog][layout][verified]") {
    auto const layout = cb::v_wide_layout();
    auto const verified_count = std::count_if(
        layout.begin(), layout.end(), [](cb::SignalLayout const &s) {
            return s.verification == cb::Verification::Verified;
        });
    REQUIRE(verified_count == 36);
}

TEST_CASE("All Verified entries carry non-zero wire scale + RAM address",
          "[ecu][did-datalog][layout][verified]") {
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Verified)
            continue;
        REQUIRE(s.ram_address != 0u);
        REQUIRE(s.scale != 0.0);
    }
}

TEST_CASE("Hypothesized entries carry zero wire scale (sentinel)",
          "[ecu][did-datalog][layout][hypothesis]") {
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Hypothesized)
            continue;
        REQUIRE(s.scale == 0.0);
        REQUIRE(s.offset == 0.0);
    }
    // Narrow layout is entirely Hypothesized.
    for (auto const &s : cb::v_narrow_layout()) {
        REQUIRE(s.verification == cb::Verification::Hypothesized);
    }
}

TEST_CASE("Narrow layout exposes 31 signals across 3 DIDs",
          "[ecu][did-datalog][layout]") {
    auto const layout = cb::v_narrow_layout();
    REQUIRE(layout.size() == 31);
    // Narrow layout only covers F300/F301/F302 (pre-expansion).
    for (auto const &s : layout) {
        REQUIRE((s.did == 0xF300 || s.did == 0xF301 || s.did == 0xF302));
    }
}

TEST_CASE("ap_layout dispatches to the right firmware",
          "[ecu][did-datalog][layout]") {
    REQUIRE(cb::ap_layout(cb::DidLayoutVersion::Narrow).size() == 31);
    REQUIRE(cb::ap_layout(cb::DidLayoutVersion::Wide).size() == 36);
}

TEST_CASE("Narrow Hypothesized offsets fit within payload widths",
          "[ecu][did-datalog][layout]") {
    // Narrow is the only layout where the hypothesis offsets were
    // bounded by the captured payload widths. Wide-layout
    // Verified entries (e.g. TD Boost Error Ext at F300:21 u16_le)
    // already exceed the 22-byte F300 width per the R²-fit
    // — investigation pending whether F300 is actually wider on
    // or the byte-numbering convention differs.
    auto const layout = cb::v_narrow_layout();
    for (auto const &s : layout) {
        auto const it = std::find_if(
            cb::kDidPayloads.begin(), cb::kDidPayloads.end(),
            [&](cb::DidPayload const &p) { return p.did == s.did; });
        REQUIRE(it != cb::kDidPayloads.end());
        std::uint8_t const width =
            (s.storage == cb::SignalStorage::Uint16 ||
             s.storage == cb::SignalStorage::Int16 ||
             s.storage == cb::SignalStorage::Uint16Le ||
             s.storage == cb::SignalStorage::Int16Le)
                ? 2u
                : 1u;
        REQUIRE(s.byte_offset + width <= it->bytes);
    }
}

TEST_CASE("AP layouts carry RAM addresses + scaling expressions",
          "[ecu][did-datalog][layout][ram]") {
    // Each entry names a non-empty scaling expression and a
    // catalog-resolved RAM address. Most addresses fall in the
    // 0xFFF8xxxx band; Vehicle Speed at 0xFFF99835 is an exception.
    for (auto const &s : cb::v_narrow_layout()) {
        REQUIRE_FALSE(s.scaling.empty());
        REQUIRE(s.ram_address >= 0xFFF80000u);
    }
    for (auto const &s : cb::v_wide_layout()) {
        // Aligned layout dropped the catalog scaling expression
        // strings — every Verified row carries scale + offset
        // for direct decode and that's all the layer needs.
        REQUIRE(s.ram_address >= 0xFFF80000u);
    }
}

TEST_CASE("find_signal returns the F301:6 Verified RPM mapping (wide)",
          "[ecu][did-datalog][layout][verified]") {
    // 2026-06-06 aligned: RPM at F301:6 u16_be with the canonical SSM
    // scale x/5.12 = 0.19531 and zero offset. v5 carried a misfit
    // 0.21246 scale that was compensating for time-alignment error.
    auto const *rpm = cb::find_signal(0xF301, 6);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    REQUIRE(rpm->ram_address == 0xFFF8D424u);
    REQUIRE(rpm->verification == cb::Verification::Verified);
    REQUIRE(rpm->scale == 0.19531);
    REQUIRE(rpm->offset == 0.0);
}

TEST_CASE("Vehicle Speed Verified entry sits at F302:0 (aligned position)",
          "[ecu][did-datalog][layout][verified]") {
    // 2026-06-06 aligned: VSS moved from v5's F300:13 u16_le to
    // F302:0 u8 with canonical scale 0.62389 (mph per raw count) and
    // a tiny -0.1256 offset.
    auto const *vs = cb::find_signal(0xF302, 0);
    REQUIRE(vs != nullptr);
    REQUIRE(vs->name == "Vehicle Speed");
    REQUIRE(vs->storage == cb::SignalStorage::Uint8);
    REQUIRE(vs->ram_address == 0xFFF99835u);
    REQUIRE(vs->verification == cb::Verification::Verified);
}

TEST_CASE("find_signal hits a known F301 RPM mapping (narrow)",
          "[ecu][did-datalog][layout]") {
    auto const *rpm = cb::find_signal(
        cb::DidLayoutVersion::Narrow, 0xF301, 17);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->name == "RPM");
    // Narrow layout packs RPM at F301:17 (hypothesis);
    // Wide layout packs it at F301:6 (verified). The RAM address is
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

} // namespace

TEST_CASE("decode_signal: RPM at F301:6 -> idle (aligned canonical scale)",
          "[ecu][did-datalog][decode]") {
    // 2026-06-06 aligned correction: RPM uses the canonical SSM scale
    // x/5.12 = 0.19531, no offset. Analyst's smoking-gun: first sniff
    // response F301[6-7] = 4158 (u16_be) → AP CSV RPM at t=0 was 808.
    // 4158 * 0.19531 = 812.1 (matches within ~4 RPM interpolation noise).
    auto const *rpm = cb::find_signal(0xF301, 6);
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->verification == cb::Verification::Verified);
    REQUIRE(rpm->offset == 0.0);
    auto const payload = make_u16_be_payload(20, 6, 4158);
    auto const eng = cb::decode_signal(*rpm,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(812.1, 1.0));
}

TEST_CASE("decode_signal: Vehicle Speed u8 at F302:0 (post-aligned position)",
          "[ecu][did-datalog][decode]") {
    auto const *vss = cb::find_signal(0xF302, 0);
    REQUIRE(vss != nullptr);
    REQUIRE(vss->name == "Vehicle Speed");
    REQUIRE(vss->storage == cb::SignalStorage::Uint8);
    // raw=80 → 80*0.62389 - 0.1256 ≈ 49.79 mph (cruise).
    std::array<std::uint8_t, 10> payload{};
    payload[0] = 80;
    auto const eng = cb::decode_signal(*vss,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(49.8, 0.2));
}

TEST_CASE("decode_signal: Coolant Temp at F303:3 -> warm-idle ~190 F",
          "[ecu][did-datalog][decode]") {
    // Coolant Temp at F303:3 with canonical 1.8 scale (C↔F conversion)
    // and offset -40. raw=128 → 128*1.8001 - 40.008 ≈ 190.4 F.
    auto const *ct = cb::find_signal(0xF303, 3);
    REQUIRE(ct != nullptr);
    REQUIRE(ct->name == "Coolant Temp");
    std::array<std::uint8_t, 10> payload{};
    payload[3] = 128;
    auto const eng = cb::decode_signal(*ct,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(190.4, 0.1));
}

TEST_CASE("decode_signal: AVCS Exh Left u16_le exercises LE endianness",
          "[ecu][did-datalog][decode]") {
    auto const *av = cb::find_signal(0xF302, 8);
    REQUIRE(av != nullptr);
    REQUIRE(av->storage == cb::SignalStorage::Uint16Le);
    // raw=12880 (LE bytes 0x50 0x32) → 12880*0.0039086 - 50.356 ≈ 0 deg
    // (at-rest cam position).
    std::array<std::uint8_t, 10> payload{};
    payload[8] = 0x50; // low byte
    payload[9] = 0x32; // high byte (12880 LE)
    auto const eng = cb::decode_signal(*av,
                                       {payload.data(), payload.size()});
    REQUIRE_THAT(eng, Catch::Matchers::WithinAbs(0.0, 0.1));
}

TEST_CASE("decode_signal returns NaN for Hypothesized rows",
          "[ecu][did-datalog][decode][hypothesis]") {
    // Hypothesized entries have scale=0; decode_signal must refuse.
    for (auto const &s : cb::v_wide_layout()) {
        if (s.verification != cb::Verification::Hypothesized)
            continue;
        std::array<std::uint8_t, 32> payload{};
        auto const eng = cb::decode_signal(s, {payload.data(), payload.size()});
        REQUIRE(std::isnan(eng));
        break; // one is enough — invariant is a single conditional
    }
}

TEST_CASE("decode_signal returns NaN on short payload",
          "[ecu][did-datalog][decode][bounds]") {
    auto const *rpm = cb::find_signal(0xF301, 6);
    REQUIRE(rpm != nullptr);
    // Payload too short for byte_offset=6 + u16.
    std::array<std::uint8_t, 5> payload{};
    auto const eng = cb::decode_signal(*rpm,
                                       {payload.data(), payload.size()});
    REQUIRE(std::isnan(eng));
}

TEST_CASE("find_signal returns nullptr for off-table position",
          "[ecu][did-datalog][layout]") {
    REQUIRE(cb::find_signal(0xF300, 99) == nullptr);
    REQUIRE(cb::find_signal(0xF399, 0) == nullptr);
    REQUIRE(cb::find_signal(cb::DidLayoutVersion::Narrow,
                            0xF399, 0) == nullptr);
}
