// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_TRANSPORT_SERIAL_BYTE_CHANNEL_HPP
#define ST_TRANSPORT_SERIAL_BYTE_CHANNEL_HPP

// Serial-port-backed IByteChannel — the platform shim that lets
// obdx::Transport and native::Transport talk to a USB-CDC ACM
// adapter that enumerates as a virtual COM port.
//
// USB-CDC ACM devices (OBDX Pro VX, most modern OBD adapters, the
// doc-18 standalone handheld) show up on every OS as a serial port:
//
//   Windows: \\.\COM5
//   Linux:   /dev/ttyACM0
//   macOS:   /dev/cu.usbmodem*
//
// That means we DON'T need libusb for this path — the OS already
// owns the USB-class driver and exposes a stable byte stream via
// the standard serial API (Win32 CreateFile + DCB, POSIX termios).
// libusb stays optional for adapters that don't enumerate as CDC
// (none of the v1.0 target adapters fall in that category).
//
// Construction is via the platform-dispatching `make_serial_byte_channel`
// factory. Each platform supplies its own .cpp; the cross-platform
// CMakeLists.txt selects the right one. Where a platform's impl
// hasn't landed yet, the factory returns NotImplemented with a
// message identifying the missing piece.

#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace st::transport {

struct SerialChannelConfig {
    // Device path the OS exposes. Format is per-platform (see header
    // comment); the factory takes it verbatim.
    std::string_view  device_path;
    // Baud rate. Most modern OBD adapters speak at 500000 or 921600;
    // OBDX Pro VX defaults to 500000. SSM K-line default is 4800.
    std::uint32_t     baud_rate{500000};
    // Read timeout (per read_bytes call). Matches the IByteChannel
    // semantics: empty-vector return on timeout, NOT an error.
    std::uint32_t     read_timeout_ms{1000};
    // Write timeout (per full write_bytes call). Timeouts during
    // write surface as TransportUnavailable.
    std::uint32_t     write_timeout_ms{1000};
};

// Construct a platform-appropriate serial-port IByteChannel.
// Returns:
//   FileNotFound          — device_path can't be opened
//   PermissionDenied      — opened but lacks read/write access
//   InvalidArgument       — baud rate or device_path is malformed
//   NotImplemented        — current platform has no impl yet
//   TransportUnavailable  — generic I/O error during setup
[[nodiscard]] st::Result<std::unique_ptr<IByteChannel>>
make_serial_byte_channel(SerialChannelConfig const &cfg);

} // namespace st::transport

#endif // ST_TRANSPORT_SERIAL_BYTE_CHANNEL_HPP
