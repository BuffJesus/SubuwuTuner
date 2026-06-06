// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ecu/cobb_datalog.hpp"

#include <algorithm>
#include <array>

namespace st::ecu::cobb_datalog {

namespace {

// AP v1.7.6.0 layout per the analyst's CSV ↔ DID-width validation
// (findings/corpus-wide-re-2026-06-06/out/cobb_datalog/
// CSV_SIGNAL_LAYOUT.md). Order matches the in-DID byte order; the
// caller can iterate through F300 then F301 etc. to walk a complete
// 67-byte payload.
using S = CobbSignalStorage;

constexpr std::array<CobbSignalLayout, 43> kApLayout{{
    // F300 (22 bytes, 14 signals)
    {0xF300,  0, S::Uint8,   10, "AF Correction 1",       "%"},
    {0xF300,  1, S::Uint16,  10, "AF Learning 1",          "%"},
    {0xF300,  3, S::Uint16, 100, "AF Sens 1 Ratio",        "AFR"},
    {0xF300,  5, S::Uint8,    1, "AVCS Exh Left",          "deg"},
    {0xF300,  6, S::Uint8,    1, "AVCS Exh Right",         "deg"},
    {0xF300,  7, S::Int8,     1, "AVCS In Left",           "deg"},
    {0xF300,  8, S::Int8,     1, "AVCS In Right",          "deg"},
    {0xF300,  9, S::Uint8,    1, "Accel Position",         "%"},
    {0xF300, 10, S::Uint16,  10, "Baro Pressure",          "psi"},
    {0xF300, 12, S::Uint8,   10, "Battery Volts",          "V"},
    {0xF300, 13, S::Uint16, 100, "Boost Extended",         "psi"},
    {0xF300, 15, S::Uint16,  10, "Calculated Load",        "g/rev"},
    {0xF300, 17, S::Uint8,    1, "Closed Loop Sw",         "on/off"},
    {0xF300, 18, S::Uint16,  10, "Comm Fuel Final",        "AFR"},
    {0xF300, 20, S::Uint16,  10, "Coolant Temp",           "F"},
    // F301 (19 bytes, 12 signals)
    {0xF301,  0, S::Uint8,    1, "Dyn Adv Mult",           "DAM"},
    {0xF301,  1, S::Uint8,    1, "Feedback Knock",         "deg"},
    {0xF301,  2, S::Uint8,    1, "Fine Knock Learn",       "deg"},
    {0xF301,  3, S::Uint8,   10, "Fuel Pressure",          "psi"},
    {0xF301,  4, S::Uint8,   10, "Fuel Pressure Target",   "psi"},
    {0xF301,  5, S::Uint8,    1, "Gear Position",          "gear"},
    {0xF301,  6, S::Uint16,  10, "Ignition Timing",        "deg"},
    {0xF301,  8, S::Uint16, 100, "Inj Duty Cycle",         "%"},
    {0xF301, 10, S::Uint16,  10, "Intake Temp",            "F"},
    {0xF301, 12, S::Uint16,  10, "Intake Temp Manifold",   "F"},
    {0xF301, 14, S::Uint16,   1, "KS Noise Cyl 1",         "raw"},
    {0xF301, 16, S::Uint16,   1, "KS Noise Cyl 2",         "raw"},
    // F302 (10 bytes, 5 signals)
    {0xF302,  0, S::Uint16,   1, "KS Noise Cyl 3",         "raw"},
    {0xF302,  2, S::Uint16,   1, "KS Noise Cyl 4",         "raw"},
    {0xF302,  4, S::Uint8,    1, "Knock Sum",              "count"},
    {0xF302,  5, S::Uint16, 100, "MAF Corr Final",         "g/s"},
    {0xF302,  7, S::Uint16,  10, "MAF Volts",              "V"},
    // F303 (10 bytes, 6 signals)
    {0xF303,  0, S::Uint16, 100, "Man Abs Press",          "psi"},
    {0xF303,  2, S::Uint16,  10, "Oil Temp",               "F"},
    {0xF303,  4, S::Uint16,   1, "RPM",                    "RPM"},
    {0xF303,  6, S::Uint16, 100, "Req Torque",             "Nm"},
    {0xF303,  8, S::Uint8,    1, "Roughness Cyl 1",        "count"},
    {0xF303,  9, S::Uint8,    1, "Roughness Cyl 2",        "count"},
    // F304 (6 bytes, 5 signals)
    {0xF304,  0, S::Uint8,    1, "Roughness Cyl 3",        "count"},
    {0xF304,  1, S::Uint8,    1, "Roughness Cyl 4",        "count"},
    {0xF304,  2, S::Uint8,    1, "SD Mode Airflow",        "raw"},
    {0xF304,  3, S::Uint16, 100, "SD VE Comm",             "%"},
    {0xF304,  5, S::Uint8,    1, "TD Proportional",        "%"},
}};

} // namespace

std::span<CobbSignalLayout const> ap_v1_7_6_0_layout() noexcept {
    return {kApLayout.data(), kApLayout.size()};
}

CobbSignalLayout const *
find_signal(std::uint16_t did, std::uint8_t byte_offset) noexcept {
    auto const it = std::find_if(kApLayout.begin(), kApLayout.end(),
                                 [did, byte_offset](CobbSignalLayout const &s) {
                                     return s.did == did &&
                                            s.byte_offset == byte_offset;
                                 });
    if (it == kApLayout.end())
        return nullptr;
    return &*it;
}

} // namespace st::ecu::cobb_datalog
