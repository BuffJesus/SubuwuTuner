// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::IByteChannel — generic raw-byte channel.
//
// The abstraction layer between higher-level framed transports
// (obdx::Transport, native::Transport, future stn::Transport) and
// the concrete byte source (a USB CDC ACM endpoint, a serial port,
// a TCP socket, a test fake). Anything that looks like "write
// bytes / read bytes with a timeout" can implement this.
//
// J2534 sits OUTSIDE this abstraction — the vendor DLL owns the
// byte plumbing internally and we drive it via function pointers
// (see j2534::J2534Library + j2534::Transport). IByteChannel is
// only for the host-side-owns-the-framing path.
//
// Semantics:
//   write_bytes(span) — synchronously write every byte. Returns
//     TransportUnavailable if the channel went away.
//   read_bytes(max, timeout) — read AT MOST `max` bytes, with up
//     to `timeout` to wait for any bytes. Returns an empty vector
//     on timeout (NOT an error). Real USB endpoints exhibit this
//     "return what's there" behavior; the framing layer above is
//     responsible for accumulating multiple read_bytes calls into
//     a complete frame.

#ifndef ST_TRANSPORT_BYTE_CHANNEL_HPP
#define ST_TRANSPORT_BYTE_CHANNEL_HPP

#include "st/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace st::transport {

class IByteChannel {
  public:
    IByteChannel()                                = default;
    virtual ~IByteChannel()                       = default;
    IByteChannel(IByteChannel const &)            = delete;
    IByteChannel &operator=(IByteChannel const &) = delete;
    IByteChannel(IByteChannel &&) noexcept        = default;
    IByteChannel &operator=(IByteChannel &&) noexcept = default;

    [[nodiscard]] virtual st::Status write_bytes(
        std::span<std::uint8_t const> bytes) = 0;

    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read_bytes(
        std::size_t max_bytes,
        std::chrono::milliseconds timeout) = 0;
};

} // namespace st::transport

#endif // ST_TRANSPORT_BYTE_CHANNEL_HPP
