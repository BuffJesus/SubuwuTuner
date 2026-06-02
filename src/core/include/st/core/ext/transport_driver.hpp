// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_TRANSPORT_DRIVER_HPP
#define ST_CORE_EXT_TRANSPORT_DRIVER_HPP

#include "st/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace st::ext {

// TransportDriver — a raw byte-frame channel to an ECU.
//
// This is the lowest-level abstraction in the extension surface. Frames
// are protocol-agnostic byte blobs: an SSM frame, a UDS PDU, an ISO-TP
// segment, or an arbitrary diagnostic payload depending on the link.
// The driver does not know about protocol semantics; that lives one
// layer up in `st::ecu`.
//
// Drivers are typically stateful (open/close lifecycle, buffered
// in-flight frames) but the protocol layer treats them as
// request/response oriented. Concurrency: a driver may be `send`-ed
// from one thread and `receive`-d from another, but multiple senders
// or multiple receivers on the same instance is implementation-defined.
class TransportDriver {
public:
    virtual ~TransportDriver() = default;

    // Stable identifier for the driver (e.g. "obdx-pro-vx-usbcdc",
    // "j2534-tactrix", "mock"). Used for selecting + logging.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Human-readable description (used in UI device pickers).
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;

    // Configure + open. After a successful `open`, `send` and `receive`
    // may be called. Repeated `open` on an already-open driver returns
    // an error; the host should `close()` first.
    [[nodiscard]] virtual Status open() = 0;

    // Closes the channel. Idempotent — calling `close()` on a closed
    // driver succeeds.
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    // Send a raw frame. The bytes are owned by the caller; the driver
    // must not retain a reference past the call. Blocking call; the
    // driver may impose its own internal timeout.
    [[nodiscard]] virtual Status send(std::span<std::uint8_t const> frame) = 0;

    // Receive the next frame, waiting up to `timeout`. Returns
    // ErrorCode::TransportTimeout if no frame arrives within the
    // window. The returned vector is owned by the caller and is
    // emptied on subsequent receives.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>>
    receive(std::chrono::milliseconds timeout) = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_TRANSPORT_DRIVER_HPP
