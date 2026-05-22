// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// POSIX (Linux + macOS) stub for make_serial_byte_channel. The real
// implementation is Windows-only at `serial_byte_channel_win.cpp` for
// now — the platform USB CDC byte channel on POSIX waits on the
// adapter-arrival gate per docs/04-roadmap.md Phase 3. Returning
// NotImplemented here lets st::transport link cleanly on every host
// CI matrix entry; the OBDX / native transports still build, they
// just can't open until the platform layer lands.
//
// Per the docs/02-architecture stance (no platform calls in domain
// code), this file is the dedicated POSIX seam — adding a termios /
// IOKit impl later only touches this file.

#include "st/core/error.hpp"
#include "st/transport/serial_byte_channel.hpp"

namespace st::transport {

st::Result<std::unique_ptr<IByteChannel>>
make_serial_byte_channel(SerialChannelConfig const & /*cfg*/) {
    return failure(ErrorCode::NotImplemented,
                   "make_serial_byte_channel: POSIX USB CDC channel not yet "
                   "implemented — adapter-arrival gated, see "
                   "docs/04-roadmap.md Phase 3");
}

} // namespace st::transport
