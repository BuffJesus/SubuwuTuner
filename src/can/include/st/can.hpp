// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CAN_HPP
#define ST_CAN_HPP

#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::can {

// Bus identifier — vehicles commonly expose 2 (HS+MS) but we don't pin a
// fixed count. The numeric value matches the 0-indexed channel; .asc
// files use 1-indexed channels and the reader / writer translates.
enum class BusId : std::uint8_t {
    Hs = 0,
    Ms = 1,
    Bus2 = 2,
    Bus3 = 3,
};

// One CAN frame. Sized for classic CAN (8-byte payload); CAN-FD support
// will need a wider payload buffer + an FD flag, which is a follow-up.
//
// `timestamp_ns` is monotonic relative to the start of a capture (zero
// for the first frame in a trace) — not a wall-clock value. The reader
// preserves whatever offsets the .asc file declared.
struct Frame {
    std::int64_t timestamp_ns{0};
    BusId bus{BusId::Hs};
    std::uint32_t id{0};
    bool extended{false};               // 29-bit when true
    bool remote{false};                 // RTR frame (rare)
    std::uint8_t dlc{0};                // 0..8 for classic CAN
    std::array<std::uint8_t, 8> data{}; // first `dlc` bytes used

    [[nodiscard]] std::span<std::uint8_t const> payload() const noexcept {
        return {data.data(), dlc};
    }
};

// Returns true iff every field of `a` and `b` matches. Useful in tests
// for round-trip assertions. Equivalent to a memberwise == that ignores
// unused trailing bytes of `data` beyond `dlc`.
[[nodiscard]] bool frames_equal(Frame const &a, Frame const &b) noexcept;

// =====================================================================
// Vector ASCII (.asc) trace format I/O
// =====================================================================
//
// `.asc` is the lingua-franca text format for CAN logs — emitted and
// consumed by SavvyCAN, cantools, Vector CANalyzer, Wireshark, and
// every reverse-engineering blog post since 2010. We support the
// subset that captures actually contain in the wild: hex-base data
// frames, classic CAN (DLC <= 8), standard or extended IDs, Rx/Tx
// direction (we accept either; capture direction is informational
// only). Header lines and unknown line shapes are skipped silently
// so a file from any of those tools round-trips through `read_asc`.
//
// Frame line shape we parse and emit:
//
//   <ts_sec> <channel> <can_id>[x] Rx d <dlc> <byte> <byte> ...
//
// Example:
//
//     0.001234 1  140             Rx   d 8 00 11 22 33 44 55 66 77
//
// Returned frames are in file order. Timestamps are converted from
// the file's decimal-seconds representation to int64 nanoseconds.

[[nodiscard]] Result<std::vector<Frame>> parse_asc(std::string_view text);

[[nodiscard]] Result<std::vector<Frame>> read_asc(std::filesystem::path const &path);

[[nodiscard]] std::string format_asc(std::span<Frame const> frames);

[[nodiscard]] Status write_asc(std::filesystem::path const &path, std::span<Frame const> frames);

} // namespace st::can

#endif // ST_CAN_HPP
