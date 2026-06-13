// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::ets — IByteChannel factory for the AP3 USB endpoint.
//
// Owns a libusb-1.0 device handle bound to the AP3's vendor-specific
// interface and exposes it through the project's generic
// IByteChannel abstraction so the higher-level client codec in
// st::devices::ets doesn't have to know about USB at all.
//
// Build gating: the libusb-backed impl is compiled when CMake option
// `ST_ENABLE_AP3` is ON (the default). When OFF the factory returns
// NotImplemented, which lets every other call site (codec tests, CLI
// dispatcher) link cleanly without the dep.

#ifndef ST_TRANSPORT_COBB_AP_CHANNEL_HPP
#define ST_TRANSPORT_COBB_AP_CHANNEL_HPP

#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"

#include <cstdint>
#include <memory>

namespace st::transport::ets {

// AP3 USB ids per spec §3.
inline constexpr std::uint16_t kVendorId = 0x1A84;
inline constexpr std::uint16_t kProductId = 0x0121;
inline constexpr std::uint8_t kInterfaceNumber = 0;
inline constexpr std::uint8_t kBulkOutEndpoint = 0x03;
inline constexpr std::uint8_t kBulkInEndpoint = 0x82;

struct ChannelConfig {
    std::uint16_t vendor_id{kVendorId};
    std::uint16_t product_id{kProductId};
    std::uint8_t interface_number{kInterfaceNumber};
    std::uint8_t bulk_out_endpoint{kBulkOutEndpoint};
    std::uint8_t bulk_in_endpoint{kBulkInEndpoint};
};

// Open the first AP3 enumerated on the host's USB bus. Claims
// interface 0 (detaching any kernel driver on Linux/macOS first).
// Returns:
//   - InvalidArgument if libusb_init fails
//   - TransportUnavailable if no device matched VID/PID
//   - PermissionDenied if claim_interface fails (typical Linux:
//     missing udev rule; typical Windows: device not bound to WinUSB
//     via Zadig — see docs/install.md)
//   - NotImplemented when built with ST_ENABLE_AP3=OFF
[[nodiscard]] Result<std::unique_ptr<IByteChannel>>
open_channel(ChannelConfig const &cfg = {});

// Cheap presence check — enumerates USB devices, returns true iff a
// matching VID/PID is found. Doesn't claim the interface, doesn't
// touch device state. Safe to poll from the GUI's welcome panel /
// status bar to surface a "AccessPort detected" affordance.
//
// Returns false when:
//   - no matching device is enumerated
//   - libusb is unavailable (ST_ENABLE_AP3=OFF) — equivalent to "no device"
[[nodiscard]] bool detect_present(std::uint16_t vendor_id = kVendorId,
                                  std::uint16_t product_id = kProductId) noexcept;

} // namespace st::transport::ets

#endif // ST_TRANSPORT_COBB_AP_CHANNEL_HPP
