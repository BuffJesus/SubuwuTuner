// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_UNIT_CONVERTER_HPP
#define ST_CORE_EXT_UNIT_CONVERTER_HPP

#include <string_view>

namespace st::ext {

// UnitConverter — bidirectional conversion between a table's storage
// representation and the user's preferred display representation.
//
// One converter per unit pair (e.g. metric ↔ imperial pairs, raw ↔
// engineering units). Hosts pick a converter for each table based on
// the table's declared unit + the user's preference.
//
// Pure functions of `double` — no state, no I/O. Safe to share across
// threads.
class UnitConverter {
public:
    virtual ~UnitConverter() = default;

    // Stable identifier (e.g. "celsius-to-fahrenheit",
    // "kpa-to-psi", "gps-to-lbs-per-min", "deg-c-identity").
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // The source + destination unit symbols. Free-form short strings
    // (e.g. "°C", "°F", "kPa", "psi", "g/s", "lb/min"). Round-trip
    // safe: `to_storage(to_display(x))` should equal `x` within
    // floating-point tolerance.
    [[nodiscard]] virtual std::string_view storage_unit() const noexcept = 0;
    [[nodiscard]] virtual std::string_view display_unit() const noexcept = 0;

    [[nodiscard]] virtual double to_display(double storage_value) const noexcept = 0;
    [[nodiscard]] virtual double to_storage(double display_value) const noexcept = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_UNIT_CONVERTER_HPP
