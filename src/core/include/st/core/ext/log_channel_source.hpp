// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_LOG_CHANNEL_SOURCE_HPP
#define ST_CORE_EXT_LOG_CHANNEL_SOURCE_HPP

#include "st/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace st::ext {

// One sample produced by a log channel. Timestamp is monotonic from
// the host's perspective (so derived channels can align by elapsed
// time, not wall clock). `value` is in *display* units — conversion
// from raw transport bytes happens inside the source.
struct LogSample {
    std::chrono::nanoseconds since_epoch{};
    double value{};
};

// LogChannelSource — produces samples for one named parameter.
//
// One source per parameter; a logger session composes many sources.
// Sources are typically driven by a poll loop calling `read()` at a
// configured cadence; the source decides how to extract one sample
// (it may cache the last transport read, may issue a fresh transport
// call, may compute a derived value from other channels' values).
//
// Sources MUST be non-blocking when `read()` is called with a zero
// timeout — callers (the GUI in particular) rely on read() to drain
// at frame rate.
class LogChannelSource {
public:
    virtual ~LogChannelSource() = default;

    // Stable identifier (e.g. "ssm.0x000008.coolant_temp",
    // "derived.afr_error").
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Human-readable name + unit (UI: "Coolant temperature [°C]").
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;
    [[nodiscard]] virtual std::string_view unit() const noexcept = 0;

    // The table id (in the active definition pack) this channel "is
    // produced by" or "is consumed by", if any. Used by the live-to-
    // table cross-reference feature. Optional — sources may return
    // empty.
    [[nodiscard]] virtual std::string_view producing_table_id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view consuming_table_id() const noexcept = 0;

    // Read the next sample, waiting up to `timeout`. Returns
    // std::nullopt if no sample is available within the window (NOT
    // an error — empty is the normal "no new data" state). Returns
    // ErrorCode::TransportTimeout / IoFailure / etc. for actual
    // failures.
    [[nodiscard]] virtual Result<std::optional<LogSample>>
    read(std::chrono::milliseconds timeout) = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_LOG_CHANNEL_SOURCE_HPP
