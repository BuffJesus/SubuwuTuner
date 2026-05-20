// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::open_transport — runtime-dispatched ITransport
// constructor.
//
// Sits between the CLI's `--transport <kind>` flag (and the GUI's
// eventual "pick adapter" dialog) and the concrete Transport
// implementations. Three families ship today:
//
//   - "j2534"  → j2534::Transport on a vendor DLL (Tactrix etc.)
//   - "obdx"   → obdx::Transport on USB CDC + DVI codec
//   - "native" → native::Transport on USB CDC + our own codec
//
// Each family needs different construction arguments — J2534 needs
// a DLL path; OBDX + Native need a USB device handle. The
// TransportSpec carries the union.
//
// **Status today**: the platform-specific layers (LoadLibraryA for
// J2534, libusb / native CDC for OBDX + Native) aren't yet wired,
// so open_transport returns NotImplemented for every concrete kind.
// The shape + dispatch + error messages are real — tests lock in
// the seam — and the platform paths fill in here when adapter
// hardware arrives.

#ifndef ST_TRANSPORT_FACTORY_HPP
#define ST_TRANSPORT_FACTORY_HPP

#include "st/transport.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace st::transport {

// Adapter family. Names match the CLI's `--transport <kind>` flag
// (lowercase, no separator). Mock is deliberately NOT in this
// enum — it has no spec, lives in tests only, and consumers that
// want a MockTransport construct one directly.
enum class Kind : std::uint8_t {
    J2534,  // Tactrix OpenPort 2.0 + any other J2534 v04.04 adapter
    Obdx,   // OBDX Pro VX over USB CDC ACM, DVI binary protocol
    Native, // doc-18 handheld over USB CDC ACM, native protocol
};

// Stable lowercase name for each Kind. Used for the CLI flag,
// help text, and error messages. Returns "unknown" for invalid
// enum values (defensive — every defined Kind has a real name).
[[nodiscard]] char const *kind_name(Kind k) noexcept;

// Inverse of kind_name. Returns nullopt for any input that
// doesn't match a defined Kind. Case-sensitive — the canonical
// form is lowercase ("j2534", not "J2534" or "Tactrix").
[[nodiscard]] std::optional<Kind> parse_kind(std::string_view s) noexcept;

// Per-kind construction arguments. Unused fields are ignored:
//   J2534  → reads dll_path
//   Obdx   → reads device_path
//   Native → reads device_path
struct TransportSpec {
    Kind kind{Kind::J2534};
    // Vendor DLL path (J2534 only). Typical Windows value:
    // "C:\Program Files\OpenPort 2.0\op20pt32.dll" or whichever
    // PassThruSupport.04.04 DLL the user wants. Resolved by
    // J2534Library::load() — platform-specific dynamic load
    // lands when hardware arrives.
    std::string dll_path;
    // USB device path (OBDX + Native). Platform-specific:
    //   Windows: COM port ("COM5") or libusb device descriptor
    //   Linux:   "/dev/ttyACM0" or libusb path
    //   macOS:   "/dev/cu.usbmodem*"
    // Resolved by the (future) platform IByteChannel impl.
    std::string device_path;
};

// Construct an open-able ITransport from a runtime spec. The
// returned Transport's open() has NOT been called yet — the caller
// is responsible for that with their own LinkConfig.
//
// Today every concrete path returns NotImplemented with a message
// that names the missing platform piece (e.g. "J2534Library::load:
// platform dynamic-load not yet implemented"). The dispatch + error
// shape is stable, so callers can wire `--transport <kind>` flags
// today and get clean errors until the platform layer lands.
//
// Validation errors (unknown kind, missing dll_path for J2534,
// missing device_path for OBDX/Native) surface as InvalidArgument
// before any platform call.
[[nodiscard]] Result<std::unique_ptr<ITransport>> open_transport(TransportSpec const &spec);

} // namespace st::transport

#endif // ST_TRANSPORT_FACTORY_HPP
