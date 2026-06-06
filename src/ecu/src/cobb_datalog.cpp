// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/cobb_datalog.hpp"

#include <algorithm>
#include <array>

namespace st::ecu::cobb_datalog {

namespace {

// AP firmware layouts per the analyst handoff 2026-06-06 (see
// findings/corpus-wide-re-2026-06-06/out/cobb_datalog/did_to_ram_map.json
// + VERIFIED_DID_TO_RAM.md).
//
// The signal-name → RAM-address join is high-confidence everywhere
// (catalog match scores ≥ 0.6, most exact). The byte position within
// each DID is a separate confidence axis:
//   - 12 signals in v1.7.6.0 are R²-verified ≥ 0.95 against a real
//     driving sniff (CobbVerification::Verified). Use as ground truth.
//   - Everything else is CobbVerification::Hypothesized — byte
//     position inferred from AP CSV column order; the catalog
//     name-match still applies, but a future sniff may relocate
//     the position within its DID.
//
// RAM addresses are LF79103P-specific; same Cobb signal name on a
// different CID may live at a different address.
using S = CobbSignalStorage;
using V = CobbVerification;

// AP v1.7.4.2: 31 signals (all Hypothesized — R²-fit was run against
// a v1.7.6.0 sniff only).
constexpr std::array<CobbSignalLayout, 31> kApV1_7_4_2Layout{{
    {0xF300,  0,   S::Uint8, 0xFFF8AEE9, "AC Compressor Sw", "on/off", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  1,  S::Uint16, 0xFFF8B956, "AF Correction 1", "%", "(((((((x-32768)>>8)+128)/32)*250)-1000)/10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  3,  S::Uint16, 0xFFF8AC3C, "AF Learning 1", "%", "((((((x>>1)+64)/16)*125)-1000)/10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  5,  S::Uint16, 0xFFF88E88, "AF Sens 1 Ratio", "AFR", "((x*0.00179)/14.7)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  7,   S::Uint8, 0xFFF88DEE, "Accel Position", "%", "((x/65535)*100)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  8,  S::Uint16, 0xFFF891AE, "Baro Pressure", "psi", "((x>>8)*0.145038)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 10,  S::Uint16, 0xFFF8D5B0, "Boost", "psi", "((x/24576)*14.5038)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 12,  S::Uint16, 0xFFF8BA38, "Boost Extended", "psi", "((x/24000)-1)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 14,  S::Uint16, 0xFFF8B9A6, "CL Fuel Target", "AFR", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 16,  S::Uint16, 0xFFF8B3E6, "Calculated Load", "g/rev", "((x/65535)*3.3)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 18,   S::Uint8, 0xFFF8B5D0, "Comm Fuel Final", "AFR", "(x/14.7)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 19,   S::Uint8, 0xFFF891B0, "Coolant Temp", "F", "(((x*5)>>11)-40)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 20,   S::Uint8, 0xFFF8123E, "Dyn Adv Mult", "DAM", "(x/16)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 21,   S::Uint8, 0xFFF8AD56, "Feedback Knock", "deg", "(x*0.3515625)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  0,   S::Uint8, 0xFFF8AD66, "Fine Knock Learn", "deg", "(x*0.3515625)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  1,  S::Uint16, 0xFFF8BBAA, "Fuel Pressure", "psi", "(x*10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  3,  S::Uint16, 0xFFF8BBAA, "Fuel Pressure Target", "psi", "(x*10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  5,   S::Uint8, 0xFFF8ABC3, "Gear Position", "Gear", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  6,  S::Uint16, 0xFFF8ACFE, "Ignition Timing", "deg", "(((((x+128)/2)*10)-640)/10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  8,  S::Uint16, 0xFFF8D60C, "Inj Duty Cycle", "%", "(((((x*13107)>>20)/125)*3200)/100)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 10,   S::Uint8, 0xFFF8B157, "Intake Temp", "F", "(x-40)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 11,   S::Uint8, 0xFFF889F3, "Intake Temp Manifold", "F", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 12,  S::Uint16, 0xFFF8C914, "MAF Corr Final", "g/s", "(x/60)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 14,  S::Uint16, 0xFFF8D120, "MAF Volts", "V", "((((x>>16)*125)>>15)/50)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 16,   S::Uint8, 0xFFF891B4, "Oil Temp", "F", "(((x*205)>>16)-40)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 17,  S::Uint16, 0xFFF8D424, "RPM", "RPM", "(x/5.12)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  0,   S::Uint8, 0xFFF8B4D8, "Req Torque", "Nm", "((x-16000)/80)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  1,  S::Uint16, 0xFFF8BA12, "TD Boost Error Ext", "psi", "((x/25600)*100000)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  3,   S::Uint8, 0xFFF8BA80, "TGV Map Ratio", "mult", "((x/65535)*100)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  4,  S::Uint16, 0xFFF8A8E4, "Throttle Pos", "%", "(((((x>>16)*255)/65535)*20)/51)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  6,   S::Uint8, 0xFFF8BA16, "Wastegate Duty", "%", "(x/256)", V::Hypothesized, 0.0, 0.0, "", false},
}};

// AP v1.7.6.0: 48 signals — 12 R²-verified (Verified) + 36 from the
// CSV-column-order hypothesis (Hypothesized). For the 12 Verified
// rows the previously-hypothesized positions are dropped; the
// Verified positions supersede.
//
// 5 of the 6 "overflow" signals (didn't fit in the 67-byte hypothesis
// budget) are now placed via R²: SD VE Est MAF, TD Boost Error Ext,
// TGV Map Ratio, Vehicle Speed, Wastegate Duty. Throttle Pos still
// missing — needs another correlation pass or an SSM-0xA8 RAM read.
constexpr std::array<CobbSignalLayout, 44> kApV1_7_6_0Layout{{
    // ---- Verified (R²-fit ≥ 0.95 against the dmann driving sniff) ----
    // Wire-side cobb_scale + cobb_offset for direct decoding;
    // catalog `scaling` expression still valid for firmware-internal
    // interpretation. monitor_id is the AP-internal cfg identifier;
    // ram_authoritative=true for SSM_* monitors (OEM SSM-protocol RAM),
    // false for RAM_* (COBB-defined RAM, address candidate-only).
    {0xF300, 13, S::Uint16Le, 0xFFF99835, "Vehicle Speed", "mph", "(x>>8)", V::Verified, 0.0098823, -10.685, "SSM_VSS", true},
    {0xF300, 21, S::Uint16Le, 0xFFF8BA12, "TD Boost Error Ext", "psi", "((x/25600)*100000)", V::Verified, -0.00095194, 11.061, "RAM_TD_BST_ERR_EXTENDED_SHDIT", false},
    {0xF301,  0,  S::Uint16, 0xFFF8BA16, "Wastegate Duty", "%", "(x/256)", V::Verified, 0.028779, -10.862, "SSM_WG_DUTY", true},
    {0xF301,  6,  S::Uint16, 0xFFF8D424, "RPM", "RPM", "(x/5.12)", V::Verified, 0.21246, -130.370, "SSM_RPM", true},
    {0xF301, 10,   S::Uint8, 0xFFF8BA80, "TGV Map Ratio", "mult", "((x/65535)*100)", V::Verified, 0.0039216, 0.000, "RAM_TGV_MAP_RATIO", false},
    {0xF301, 11,   S::Uint8, 0xFFF8BB98, "Battery Volts", "V", "(x/1000)", V::Verified, 0.0039108, 12.177, "SSM_BATT_VOLT", true},
    {0xF301, 14,  S::Uint16, 0xFFF8B5D0, "Comm Fuel Final", "AFR", "(x/14.7)", V::Verified, 0.018481, 9.879, "RAM_COMM_FUEL_FINAL", false},
    {0xF302,  0,   S::Uint8, 0xFFF891B4, "Oil Temp", "F", "(((x*205)>>16)-40)", V::Verified, 0.03105, 208.990, "RAM_OIL_TEMP_SHDIT", false},
    {0xF302,  4,  S::Uint16, 0xFFF8ABC3, "Gear Position", "gear", "x", V::Verified, 0.12453, -16.586, "RAM_GEAR_POS_EST", false},
    {0xF304,  1, S::Uint16Le, 0xFFF8BB72, "SD VE Est MAF", "%", "((x/32768)*100)", V::Verified, 0.098088, 76.415, "RAM_SD_VE_EST_MAF", false},
    {0xF304,  3, S::Uint16Le, 0xFFF88E88, "AF Sens 1 Ratio", "AFR", "((x*0.00179)/14.7)", V::Verified, 0.014329, -322.799, "SSM_AFSNSR_1_RATIO", true},
    {0xF304,  6,  S::Uint16, 0xFFF8AC3C, "AF Learning 1", "%", "((((((x>>1)+64)/16)*125)-1000)/10)", V::Verified, 0.061512, 0.097, "SSM_AFLRN_1", true},
    // ---- Hypothesized (CSV column order inference; catalog name +
    // RAM address are high-confidence, byte position is a guess) ----
    {0xF300,  0,   S::Uint8, 0xFFF8B956, "AF Correction 1", "%", "(((((((x-32768)>>8)+128)/32)*250)-1000)/10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  3,  S::Uint16, 0xFFF88E88, "AF Sens 1 Ratio (alt)", "AFR", "((x*0.00179)/14.7)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  5,   S::Uint8, 0xFFF8914E, "AVCS Exh Left", "deg", "((x*45)>>14)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  6,   S::Uint8, 0xFFF8914C, "AVCS Exh Right", "deg", "((x*45)>>14)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  7,    S::Int8, 0xFFF89142, "AVCS In Left", "deg", "((x*45)>>14)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  8,    S::Int8, 0xFFF89140, "AVCS In Right", "deg", "((x*45)>>14)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300,  9,   S::Uint8, 0xFFF88DEE, "Accel Position", "%", "((x/65535)*100)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 10,  S::Uint16, 0xFFF891AE, "Baro Pressure", "psi", "((x>>8)*0.145038)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 15,  S::Uint16, 0xFFF8B3E6, "Calculated Load", "g/rev", "((x/65535)*3.3)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 17,   S::Uint8, 0xFFF8ADED, "Closed Loop Sw", "on/off", "((x&128)/128)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF300, 20,  S::Uint16, 0xFFF891B0, "Coolant Temp", "F", "(((x*5)>>11)-40)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  1,   S::Uint8, 0xFFF8123E, "Dyn Adv Mult", "DAM", "(x/16)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  2,   S::Uint8, 0xFFF8AD56, "Feedback Knock", "deg", "(x*0.3515625)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  3,   S::Uint8, 0xFFF8AD66, "Fine Knock Learn", "deg", "(x*0.3515625)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  4,   S::Uint8, 0xFFF8BBAA, "Fuel Pressure", "psi", "(x*10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  5,   S::Uint8, 0xFFF8BBAA, "Fuel Pressure Target", "psi", "(x*10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301,  8,  S::Uint16, 0xFFF8ACFE, "Ignition Timing", "deg", "(((((x+128)/2)*10)-640)/10)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 12,  S::Uint16, 0xFFF8D60C, "Inj Duty Cycle", "%", "(((((x*13107)>>20)/125)*3200)/100)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF301, 16,  S::Uint16, 0xFFF8B157, "Intake Temp", "F", "(x-40)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  2,  S::Uint16, 0xFFF889F3, "Intake Temp Manifold", "F", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  6,  S::Uint16, 0xFFF8C880, "KS Noise Cyl 1", "raw", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF302,  8,  S::Uint16, 0xFFF8C882, "KS Noise Cyl 2", "raw", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF303,  0,  S::Uint16, 0xFFF8C884, "KS Noise Cyl 3", "raw", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF303,  2,  S::Uint16, 0xFFF8C886, "KS Noise Cyl 4", "raw", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF303,  4,   S::Uint8, 0xFFF8AD56, "Knock Sum", "count", "(x*0.3515625)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF303,  5,  S::Uint16, 0xFFF8C914, "MAF Corr Final", "g/s", "(x/60)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF303,  7,  S::Uint16, 0xFFF8D120, "MAF Volts", "V", "((((x>>16)*125)>>15)/50)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF303,  9,  S::Uint16, 0xFFF8D078, "Man Abs Press", "psi", "((x/65535)*5)", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF304,  0,  S::Uint16, 0xFFF8B4D8, "Req Torque", "Nm", "((x-16000)/80)", V::Hypothesized, 0.0, 0.0, "", false},
    // (Roughness cyl 1-4 moved to F304:2-5 because the 4 Verified
    // F302 positions displaced the original Roughness positions.)
    {0xF304,  2,   S::Uint8, 0xFFF8AF65, "Roughness Cyl 1", "count", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF304,  4,   S::Uint8, 0xFFF8AF66, "Roughness Cyl 2", "count", "x", V::Hypothesized, 0.0, 0.0, "", false},
    {0xF304,  5,   S::Uint8, 0xFFF8AF67, "Roughness Cyl 3", "count", "x", V::Hypothesized, 0.0, 0.0, "", false},
    // (Truncated to fit the 6-byte F304 budget — the 4th cylinder's
    // Roughness, SD Mode Airflow, SD VE Comm, TD Proportional moved
    // to hypothetical bytes that overlap Verified rows or overflow.
    // Pending another correlation pass for placement.)
}};

} // namespace

std::span<CobbSignalLayout const> ap_v1_7_4_2_layout() noexcept {
    return {kApV1_7_4_2Layout.data(), kApV1_7_4_2Layout.size()};
}

std::span<CobbSignalLayout const> ap_v1_7_6_0_layout() noexcept {
    return {kApV1_7_6_0Layout.data(), kApV1_7_6_0Layout.size()};
}

std::span<CobbSignalLayout const> ap_layout(CobbApFirmware fw) noexcept {
    switch (fw) {
    case CobbApFirmware::V1_7_4_2_CCF_Gen2:
        return ap_v1_7_4_2_layout();
    case CobbApFirmware::V1_7_6_0_CCF_Gen3:
        return ap_v1_7_6_0_layout();
    }
    return {};
}

std::span<DidPayload const> did_payloads(CobbApFirmware fw) noexcept {
    switch (fw) {
    case CobbApFirmware::V1_7_4_2_CCF_Gen2:
        return {kDidPayloadsV1_7_4_2.data(), kDidPayloadsV1_7_4_2.size()};
    case CobbApFirmware::V1_7_6_0_CCF_Gen3:
        return {kDidPayloadsV1_7_6_0.data(), kDidPayloadsV1_7_6_0.size()};
    }
    return {};
}

std::size_t total_payload_bytes(CobbApFirmware fw) noexcept {
    switch (fw) {
    case CobbApFirmware::V1_7_4_2_CCF_Gen2:
        return kTotalPayloadBytesV1_7_4_2;
    case CobbApFirmware::V1_7_6_0_CCF_Gen3:
        return kTotalPayloadBytesV1_7_6_0;
    }
    return 0;
}

CobbSignalLayout const *
find_signal(CobbApFirmware fw, std::uint16_t did,
            std::uint8_t byte_offset) noexcept {
    auto const layout = ap_layout(fw);
    auto const it =
        std::find_if(layout.begin(), layout.end(),
                     [did, byte_offset](CobbSignalLayout const &s) {
                         return s.did == did && s.byte_offset == byte_offset;
                     });
    if (it == layout.end())
        return nullptr;
    return &*it;
}

CobbSignalLayout const *
find_signal(std::uint16_t did, std::uint8_t byte_offset) noexcept {
    return find_signal(CobbApFirmware::V1_7_6_0_CCF_Gen3, did, byte_offset);
}

double decode_signal(CobbSignalLayout const &layout,
                     std::span<std::uint8_t const> did_payload) noexcept {
    auto const nan = std::numeric_limits<double>::quiet_NaN();
    if (layout.cobb_scale == 0.0)
        return nan;
    std::size_t const off = layout.byte_offset;
    auto const sz = did_payload.size();
    auto const u8 = [&](std::size_t i) { return did_payload[i]; };
    std::int64_t raw = 0;
    switch (layout.storage) {
    case CobbSignalStorage::Uint8:
        if (off >= sz)
            return nan;
        raw = static_cast<std::int64_t>(u8(off));
        break;
    case CobbSignalStorage::Int8:
        if (off >= sz)
            return nan;
        raw = static_cast<std::int64_t>(static_cast<std::int8_t>(u8(off)));
        break;
    case CobbSignalStorage::Uint16:
        if (off + 1 >= sz)
            return nan;
        raw = static_cast<std::int64_t>(
            (static_cast<std::uint16_t>(u8(off)) << 8) |
            static_cast<std::uint16_t>(u8(off + 1)));
        break;
    case CobbSignalStorage::Int16:
        if (off + 1 >= sz)
            return nan;
        raw = static_cast<std::int64_t>(static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(u8(off)) << 8) |
            static_cast<std::uint16_t>(u8(off + 1))));
        break;
    case CobbSignalStorage::Uint16Le:
        if (off + 1 >= sz)
            return nan;
        raw = static_cast<std::int64_t>(
            (static_cast<std::uint16_t>(u8(off + 1)) << 8) |
            static_cast<std::uint16_t>(u8(off)));
        break;
    case CobbSignalStorage::Int16Le:
        if (off + 1 >= sz)
            return nan;
        raw = static_cast<std::int64_t>(static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(u8(off + 1)) << 8) |
            static_cast<std::uint16_t>(u8(off))));
        break;
    }
    return static_cast<double>(raw) * layout.cobb_scale + layout.cobb_offset;
}

} // namespace st::ecu::cobb_datalog
