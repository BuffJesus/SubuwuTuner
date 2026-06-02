// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_CHECKSUM_STRATEGY_HPP
#define ST_CORE_EXT_CHECKSUM_STRATEGY_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace st::ext {

// ChecksumStrategy — computes and updates an ECU family's calibration
// checksum bytes.
//
// One strategy per family / generation. The `FlashPreflight` validator
// `make_checksum_known` consults a registry of known strategies; the
// flash orchestrator calls `recompute_in_place` on the source ROM
// before write so the post-write readback verify succeeds.
//
// Strategies are pure functions of the bytes — no I/O, no clocks, no
// global state. Multiple instances are allowed; implementations should
// be thread-safe (const methods + no mutable members).
class ChecksumStrategy {
public:
    virtual ~ChecksumStrategy() = default;

    // Stable identifier (e.g. "subaru.sh2a.cal-v1", "subaru.rh850.cal-v2").
    // Used to look up the strategy at flash time.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Compute the checksum WITHOUT modifying `bytes`. Returns a value
    // that, when compared to the bytes' on-rom stored checksum,
    // indicates pass/fail. The exact width (16/32-bit) is family-
    // specific; expose it as a uint32 so the interface is uniform.
    [[nodiscard]] virtual Result<std::uint32_t>
    compute(std::span<std::uint8_t const> bytes) const = 0;

    // Recompute and write the checksum bytes back into `bytes` in
    // place. After this call, `bytes` is internally consistent and can
    // be safely written to the ECU. Returns an error iff the input
    // buffer is the wrong size or otherwise unprocessable for this
    // strategy.
    [[nodiscard]] virtual Status recompute_in_place(std::span<std::uint8_t> bytes) const = 0;

    // Byte ranges this strategy reads + writes. Returned as inclusive
    // [begin, end) pairs. The flash orchestrator uses this to know
    // which sectors must be re-checksummed even if the user only
    // edited an unrelated table. Implementations may return an empty
    // span if checksum-affected ranges are the entire buffer.
    struct Range {
        std::uint32_t begin;
        std::uint32_t end;
    };
    [[nodiscard]] virtual std::span<Range const> affected_ranges() const noexcept = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_CHECKSUM_STRATEGY_HPP
