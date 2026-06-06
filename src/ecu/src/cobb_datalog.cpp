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

// AP v1.7.6.0: 36 R²-verified signals from the 2026-06-06 aligned
// re-fit (analyst's BIG_FINDING_v5_was_wrong.md).
//
// HISTORY: The prior layout shipped 12 "Verified" rows from the
// 2026-05-28 dmann session's `cross_ref5` work + 32 "Hypothesized"
// rows from the CSV-column-order inference. The 2026-06-06 user-car
// driving sniff allowed a properly-time-aligned re-fit (sniff/CSV
// offset −1.6 s, RPM anchor R²=0.9999) which discovered that:
//   1. The dmann session's OLS fits were compensating for a time-
//      alignment error — most "Verified" scales were COBB-applied-
//      transform artifacts, not actual COBB transforms.
//   2. With proper alignment, COBB packs raw SSM values directly
//      (canonical scales: RPM is x/5.12 not the v5 OLS-fit 0.21246).
//   3. Only RPM (F301:6) and TGV Map Ratio (F301:10) byte positions
//      survived v5 → aligned. 10 of 12 v5 positions were wrong.
//   4. The CSV-column-order hypothesis was systematically wrong too,
//      so the prior 32 Hypothesized rows are dropped entirely.
//
// All entries here are Verified with R²≥0.95 from the aligned re-fit.
// 13 of 49 AP monitors remain unassigned (Baro, Battery V, DAM,
// Roughness 1-4, Inj Duty, Intake Temp non-manifold, Knock Sum,
// Feedback Knock, Fine Knock Learn, SD Mode Airflow) — they had
// low variance in the driving sniff and need a fresh capture with
// cold-start + AC cycling + a knock event to fall out.
constexpr std::array<CobbSignalLayout, 36> kApV1_7_6_0Layout{{
    {0xF300,  0,   S::Uint8, 0xFFF8C914, "MAF Corr Final", "g/s", "", V::Verified, 0.016651, -0.0149, "RAM_MAF_CORR_FINAL_SHDIT", false},
    {0xF300,  4,  S::Uint16, 0xFFF8C91C, "SD VE Comm", "%", "", V::Verified, 0.39583, -6334.1000, "RAM_SD_VE_COMM", false},
    {0xF300,  8,  S::Uint16, 0xFFF8BB72, "SD VE Est MAF", "%", "", V::Verified, 0.38237, -6116.2000, "RAM_SD_VE_EST_MAF", false},
    {0xF300, 12,    S::Int8, 0xFFF8BA12, "TD Boost Error Ext", "psi", "", V::Verified, 0.3911, 0.1949, "RAM_TD_BST_ERR_EXTENDED_SHDIT", false},
    {0xF300, 14,   S::Uint8, 0xFFF8BA38, "Boost Extended", "psi", "", V::Verified, 0.39099, -11.0860, "SSM_BST_REL_EXT_SHDIT", false},
    {0xF300, 16,  S::Uint16, 0xFFF891B4, "Oil Temp", "F", "", V::Verified, 0.0056325, -40.0830, "RAM_OIL_TEMP_SHDIT", false},
    {0xF300, 18,  S::Uint16, 0xFFF8C886, "KS Noise Cyl 4", "raw", "", V::Verified, 0.97506, 5.2449, "RAM_KS_NOISE_LVL_CYL_4_SHDIT", false},
    {0xF300, 20,  S::Uint16, 0xFFF8B4D8, "Req Torque", "Nm", "", V::Verified, 0.012474, -199.6100, "RAM_REQ_TOR", false},
    {0xF300, 22,  S::Uint16, 0xFFF8BBAA, "Fuel Pressure", "psi", "", V::Verified, 0.088332, 1.6627, "RAM_FUEL_RAIL_PRESS_SHDIT", false},
    {0xF300, 24,  S::Uint16, 0xFFF88E88, "AF Sens 1 Ratio", "AFR", "", V::Verified, 0.001793, 0.0208, "SSM_AFSNSR_1_RATIO", true},
    {0xF301,  2,  S::Uint16, 0xFFF8C884, "KS Noise Cyl 3", "raw", "", V::Verified, 0.98226, 6.2993, "RAM_KS_NOISE_LVL_CYL_3_SHDIT", false},
    {0xF301,  4,  S::Uint16, 0xFFF8C882, "KS Noise Cyl 2", "raw", "", V::Verified, 0.97029, 8.0129, "RAM_KS_NOISE_LVL_CYL_2_SHDIT", false},
    {0xF301,  6,  S::Uint16, 0xFFF8D424, "RPM", "RPM", "", V::Verified, 0.19531, 0.0000, "SSM_RPM", true},
    {0xF301,  8,  S::Uint16, 0xFFF8C880, "KS Noise Cyl 1", "raw", "", V::Verified, 0.96704, 7.2939, "RAM_KS_NOISE_LVL_CYL_1_SHDIT", false},
    {0xF301, 10,   S::Uint8, 0xFFF8BA80, "TGV Map Ratio", "mult", "", V::Verified, 0.0039163, -0.0005, "RAM_TGV_MAP_RATIO", false},
    {0xF301, 12,  S::Uint16, 0xFFF8BBAA, "Fuel Pressure Target", "psi", "", V::Verified, 0.088389, -0.0431, "RAM_FUEL_RAIL_PRESS_TARGET_SHDIT", false},
    {0xF301, 14, S::Int16Le, 0xFFF8B5D0, "Comm Fuel Final", "AFR", "", V::Verified, -0.00020043, 14.7430, "RAM_COMM_FUEL_FINAL", false},
    {0xF301, 16,  S::Uint16, 0xFFF8BA32, "TD Proportional", "%", "", V::Verified, 0.0039236, 0.0000, "RAM_TD_PROP", false},
    {0xF301, 18,  S::Uint16, 0xFFF8D078, "Man Abs Press", "psi", "", V::Verified, 0.00056581, 1.9241, "RAM_MAN_ABS_PRESS", false},
    {0xF302,  0,   S::Uint8, 0xFFF99835, "Vehicle Speed", "mph", "", V::Verified, 0.62389, -0.1256, "SSM_VSS", true},
    {0xF302,  1,   S::Uint8, 0xFFF8ABC3, "Gear Position", "gear", "", V::Verified, 0.99692, 0.0053, "RAM_GEAR_POS_EST", false},
    {0xF302,  2,   S::Uint8, 0xFFF89142, "AVCS In Left", "deg", "", V::Verified, 0.99726, -49.8920, "SSM_AVCS_INT_LEFT", true},
    {0xF302,  4,  S::Uint16, 0xFFF8ACFE, "Ignition Timing", "deg", "", V::Verified, 0.49872, -63.7960, "SSM_IGN_TIMING", true},
    {0xF302,  6,   S::Uint8, 0xFFF8B3E6, "Calculated Load", "g/rev", "", V::Verified, 0.012908, -0.0010, "RAM_CALC_LOAD", false},
    {0xF302,  7,   S::Uint8, 0xFFF8A8E4, "Throttle Pos", "%", "", V::Verified, 0.39278, -0.0038, "SSM_THROTTLE_POS", true},
    {0xF302,  8, S::Uint16Le, 0xFFF8914E, "AVCS Exh Left", "deg", "", V::Verified, 0.0039086, -50.3560, "SSM_AVCS_EXH_LEFT", true},
    {0xF303,  2,   S::Uint8, 0xFFF8B956, "AF Correction 1", "%", "", V::Verified, 0.7702, -98.5680, "SSM_AFCORR_1", true},
    {0xF303,  3,   S::Uint8, 0xFFF891B0, "Coolant Temp", "F", "", V::Verified, 1.8001, -40.0080, "SSM_COOLANT_TEMP", true},
    {0xF303,  4,   S::Uint8, 0xFFF88DEE, "Accel Position", "%", "", V::Verified, 0.39072, -0.0105, "SSM_APP_POS", true},
    {0xF303,  7,  S::Uint16, 0xFFF889F3, "Intake Temp Manifold", "F", "", V::Verified, 1.7999, -57.9890, "RAM_INT_TEMP_MAN_SHDIT", false},
    {0xF303,  9,   S::Uint8, 0xFFF8AC3C, "AF Learning 1", "%", "", V::Verified, 0.78164, -100.0500, "SSM_AFLRN_1", true},
    {0xF304,  2, S::Uint16Le, 0xFFF8ADED, "Closed Loop Sw", "on/off", "", V::Verified, -0.0019519, 1.9988, "RAM_CLOL_SW", false},
    {0xF304,  4,  S::Uint16, 0xFFF89140, "AVCS In Right", "deg", "", V::Verified, 0.99026, -23119.0000, "SSM_AVCS_INT_RIGHT", true},
    {0xF304,  6,   S::Uint8, 0xFFF8BA16, "Wastegate Duty", "%", "", V::Verified, 0.39221, 0.0000, "SSM_WG_DUTY", true},
    {0xF304,  7,   S::Uint8, 0xFFF8D120, "MAF Volts", "V", "", V::Verified, 0.019957, 0.0022, "SSM_MAF_VOLT", true},
    {0xF304,  8,   S::Uint8, 0xFFF8914C, "AVCS Exh Right", "deg", "", V::Verified, 1.0003, -50.0280, "SSM_AVCS_EXH_RIGHT", true},
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
