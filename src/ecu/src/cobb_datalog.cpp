// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/cobb_datalog.hpp"

#include <algorithm>
#include <array>

namespace st::ecu::cobb_datalog {

namespace {

// AP firmware layouts per the analyst handoff 2026-06-06 (see
// findings/corpus-wide-re-2026-06-06/out/cobb_datalog/did_to_ram_map.json).
// CSV column order × LF79103P live-signals catalog name-match.
// RAM addresses are LF79103P-specific; same Cobb signal name on a
// different CID will land at a different address.
using S = CobbSignalStorage;

// AP v1.7.4.2: 31 signals
constexpr std::array<CobbSignalLayout, 31> kApV1_7_4_2Layout{{
    {0xF300,  0,   S::Uint8, 0xFFF8AEE9, "AC Compressor Sw", "on/off", "x"},
    {0xF300,  1,  S::Uint16, 0xFFF8B956, "AF Correction 1", "%", "(((((((x-32768)>>8)+128)/32)*250)-1000)/10)"},
    {0xF300,  3,  S::Uint16, 0xFFF8AC3C, "AF Learning 1", "%", "((((((x>>1)+64)/16)*125)-1000)/10)"},
    {0xF300,  5,  S::Uint16, 0xFFF88E88, "AF Sens 1 Ratio", "AFR", "((x*0.00179)/14.7)"},
    {0xF300,  7,   S::Uint8, 0xFFF88DEE, "Accel Position", "%", "((x/65535)*100)"},
    {0xF300,  8,  S::Uint16, 0xFFF891AE, "Baro Pressure", "psi", "((x>>8)*0.145038)"},
    {0xF300, 10,  S::Uint16, 0xFFF8D5B0, "Boost", "psi", "((x/24576)*14.5038)"},
    {0xF300, 12,  S::Uint16, 0xFFF8BA38, "Boost Extended", "psi", "((x/24000)-1)"},
    {0xF300, 14,  S::Uint16, 0xFFF8B9A6, "CL Fuel Target", "AFR", "x"},
    {0xF300, 16,  S::Uint16, 0xFFF8B3E6, "Calculated Load", "g/rev", "((x/65535)*3.3)"},
    {0xF300, 18,   S::Uint8, 0xFFF8B5D0, "Comm Fuel Final", "AFR", "(x/14.7)"},
    {0xF300, 19,   S::Uint8, 0xFFF891B0, "Coolant Temp", "F", "(((x*5)>>11)-40)"},
    {0xF300, 20,   S::Uint8, 0xFFF8123E, "Dyn Adv Mult", "DAM", "(x/16)"},
    {0xF300, 21,   S::Uint8, 0xFFF8AD56, "Feedback Knock", "deg", "(x*0.3515625)"},
    {0xF301,  0,   S::Uint8, 0xFFF8AD66, "Fine Knock Learn", "deg", "(x*0.3515625)"},
    {0xF301,  1,  S::Uint16, 0xFFF8BBAA, "Fuel Pressure", "psi", "(x*10)"},
    {0xF301,  3,  S::Uint16, 0xFFF8BBAA, "Fuel Pressure Target", "psi", "(x*10)"},
    {0xF301,  5,   S::Uint8, 0xFFF8ABC3, "Gear Position", "Gear", "x"},
    {0xF301,  6,  S::Uint16, 0xFFF8ACFE, "Ignition Timing", "deg", "(((((x+128)/2)*10)-640)/10)"},
    {0xF301,  8,  S::Uint16, 0xFFF8D60C, "Inj Duty Cycle", "%", "(((((x*13107)>>20)/125)*3200)/100)"},
    {0xF301, 10,   S::Uint8, 0xFFF8B157, "Intake Temp", "F", "(x-40)"},
    {0xF301, 11,   S::Uint8, 0xFFF889F3, "Intake Temp Manifold", "F", "x"},
    {0xF301, 12,  S::Uint16, 0xFFF8C914, "MAF Corr Final", "g/s", "(x/60)"},
    {0xF301, 14,  S::Uint16, 0xFFF8D120, "MAF Volts", "V", "((((x>>16)*125)>>15)/50)"},
    {0xF301, 16,   S::Uint8, 0xFFF891B4, "Oil Temp", "F", "(((x*205)>>16)-40)"},
    {0xF301, 17,  S::Uint16, 0xFFF8D424, "RPM", "RPM", "(x/5.12)"},
    {0xF302,  0,   S::Uint8, 0xFFF8B4D8, "Req Torque", "Nm", "((x-16000)/80)"},
    {0xF302,  1,  S::Uint16, 0xFFF8BA12, "TD Boost Error Ext", "psi", "((x/25600)*100000)"},
    {0xF302,  3,   S::Uint8, 0xFFF8BA80, "TGV Map Ratio", "mult", "((x/65535)*100)"},
    {0xF302,  4,  S::Uint16, 0xFFF8A8E4, "Throttle Pos", "%", "(((((x>>16)*255)/65535)*20)/51)"},
    {0xF302,  6,   S::Uint8, 0xFFF8BA16, "Wastegate Duty", "%", "(x/256)"},
}};

// AP v1.7.6.0: 43 signals
constexpr std::array<CobbSignalLayout, 43> kApV1_7_6_0Layout{{
    {0xF300,  0,   S::Uint8, 0xFFF8B956, "AF Correction 1", "%", "(((((((x-32768)>>8)+128)/32)*250)-1000)/10)"},
    {0xF300,  1,  S::Uint16, 0xFFF8AC3C, "AF Learning 1", "%", "((((((x>>1)+64)/16)*125)-1000)/10)"},
    {0xF300,  3,  S::Uint16, 0xFFF88E88, "AF Sens 1 Ratio", "AFR", "((x*0.00179)/14.7)"},
    {0xF300,  5,   S::Uint8, 0xFFF8914E, "AVCS Exh Left", "deg", "((x*45)>>14)"},
    {0xF300,  6,   S::Uint8, 0xFFF8914C, "AVCS Exh Right", "deg", "((x*45)>>14)"},
    {0xF300,  7,    S::Int8, 0xFFF89142, "AVCS In Left", "deg", "((x*45)>>14)"},
    {0xF300,  8,    S::Int8, 0xFFF89140, "AVCS In Right", "deg", "((x*45)>>14)"},
    {0xF300,  9,   S::Uint8, 0xFFF88DEE, "Accel Position", "%", "((x/65535)*100)"},
    {0xF300, 10,  S::Uint16, 0xFFF891AE, "Baro Pressure", "psi", "((x>>8)*0.145038)"},
    {0xF300, 12,   S::Uint8, 0xFFF8BB98, "Battery Volts", "V", "(x/1000)"},
    {0xF300, 13,  S::Uint16, 0xFFF8BA38, "Boost Extended", "psi", "((x/24000)-1)"},
    {0xF300, 15,  S::Uint16, 0xFFF8B3E6, "Calculated Load", "g/rev", "((x/65535)*3.3)"},
    {0xF300, 17,   S::Uint8, 0xFFF8ADED, "Closed Loop Sw", "on/off", "((x&128)/128)"},
    {0xF300, 18,  S::Uint16, 0xFFF8B5D0, "Comm Fuel Final", "AFR", "(x/14.7)"},
    {0xF300, 20,  S::Uint16, 0xFFF891B0, "Coolant Temp", "F", "(((x*5)>>11)-40)"},
    {0xF301,  0,   S::Uint8, 0xFFF8123E, "Dyn Adv Mult", "DAM", "(x/16)"},
    {0xF301,  1,   S::Uint8, 0xFFF8AD56, "Feedback Knock", "deg", "(x*0.3515625)"},
    {0xF301,  2,   S::Uint8, 0xFFF8AD66, "Fine Knock Learn", "deg", "(x*0.3515625)"},
    {0xF301,  3,   S::Uint8, 0xFFF8BBAA, "Fuel Pressure", "psi", "(x*10)"},
    {0xF301,  4,   S::Uint8, 0xFFF8BBAA, "Fuel Pressure Target", "psi", "(x*10)"},
    {0xF301,  5,   S::Uint8, 0xFFF8ABC3, "Gear Position", "gear", "x"},
    {0xF301,  6,  S::Uint16, 0xFFF8ACFE, "Ignition Timing", "deg", "(((((x+128)/2)*10)-640)/10)"},
    {0xF301,  8,  S::Uint16, 0xFFF8D60C, "Inj Duty Cycle", "%", "(((((x*13107)>>20)/125)*3200)/100)"},
    {0xF301, 10,  S::Uint16, 0xFFF8B157, "Intake Temp", "F", "(x-40)"},
    {0xF301, 12,  S::Uint16, 0xFFF889F3, "Intake Temp Manifold", "F", "x"},
    {0xF301, 14,  S::Uint16, 0xFFF8C880, "KS Noise Cyl 1", "raw", "x"},
    {0xF301, 16,  S::Uint16, 0xFFF8C882, "KS Noise Cyl 2", "raw", "x"},
    {0xF302,  0,  S::Uint16, 0xFFF8C884, "KS Noise Cyl 3", "raw", "x"},
    {0xF302,  2,  S::Uint16, 0xFFF8C886, "KS Noise Cyl 4", "raw", "x"},
    {0xF302,  4,   S::Uint8, 0xFFF8AD56, "Knock Sum", "count", "(x*0.3515625)"},
    {0xF302,  5,  S::Uint16, 0xFFF8C914, "MAF Corr Final", "g/s", "(x/60)"},
    {0xF302,  7,  S::Uint16, 0xFFF8D120, "MAF Volts", "V", "((((x>>16)*125)>>15)/50)"},
    {0xF303,  0,  S::Uint16, 0xFFF8D078, "Man Abs Press", "psi", "((x/65535)*5)"},
    {0xF303,  2,  S::Uint16, 0xFFF891B4, "Oil Temp", "F", "(((x*205)>>16)-40)"},
    {0xF303,  4,  S::Uint16, 0xFFF8D424, "RPM", "RPM", "(x/5.12)"},
    {0xF303,  6,  S::Uint16, 0xFFF8B4D8, "Req Torque", "Nm", "((x-16000)/80)"},
    {0xF303,  8,   S::Uint8, 0xFFF8AF65, "Roughness Cyl 1", "count", "x"},
    {0xF303,  9,   S::Uint8, 0xFFF8AF66, "Roughness Cyl 2", "count", "x"},
    {0xF304,  0,   S::Uint8, 0xFFF8AF67, "Roughness Cyl 3", "count", "x"},
    {0xF304,  1,   S::Uint8, 0xFFF8AF68, "Roughness Cyl 4", "count", "x"},
    {0xF304,  2,   S::Uint8, 0xFFF8C914, "SD Mode Airflow", "raw", "(x/60)"},
    {0xF304,  3,  S::Uint16, 0xFFF8C91C, "SD VE Comm", "%", "(x*100)"},
    {0xF304,  5,   S::Uint8, 0xFFF8BA32, "TD Proportional", "%", "(x/256)"},
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

} // namespace st::ecu::cobb_datalog
