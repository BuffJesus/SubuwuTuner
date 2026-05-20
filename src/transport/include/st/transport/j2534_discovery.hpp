// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::j2534::discover_adapters — enumerate J2534 v04.04
// vendor DLLs installed on this host.
//
// The SAE J2534 spec specifies that compliant adapters register
// themselves under HKLM\Software\PassThruSupport.04.04 (32-bit
// view: HKLM\Software\Wow6432Node\PassThruSupport.04.04 on 64-bit
// Windows). Each subkey describes one installed adapter with at
// minimum a Name + FunctionLibrary path; richer keys also carry
// vendor + protocol bitmask + COM-port hints.
//
// We use this to populate a "pick adapter" list — both for the CLI
// (`subuwutuner-cli transport-list`) and the eventual GUI
// connection dialog. On non-Windows hosts `discover_adapters()`
// returns an empty list (J2534 vendor DLLs are Windows-only in
// practice — Tactrix ships a macOS/Linux runtime separately).
//
// The function does NOT load any DLL — discovery is read-only
// registry access. Loading happens later via
// `J2534Library::load(adapter.function_library)`.

#ifndef ST_TRANSPORT_J2534_DISCOVERY_HPP
#define ST_TRANSPORT_J2534_DISCOVERY_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace st::transport::j2534 {

// Which registry view the adapter was found under. Most J2534 DLLs
// are 32-bit (the API predates x64); on 64-bit Windows they live
// under Wow6432Node. A 64-bit DLL would appear under the native
// view. Recorded so the CLI can tell the user which DLL they're
// about to load + so a future loader can pick the right
// LoadLibraryA flag (LOAD_LIBRARY_AS_IMAGE_RESOURCE isn't relevant,
// but bit-width-mismatch with the host process produces
// ERROR_BAD_EXE_FORMAT — worth surfacing in the error message).
enum class RegistryView : std::uint8_t {
    Native64, // HKLM\Software\PassThruSupport.04.04 (read via KEY_WOW64_64KEY)
    Wow6432,  // HKLM\Software\Wow6432Node\PassThruSupport.04.04 (KEY_WOW64_32KEY)
};

[[nodiscard]] char const *registry_view_name(RegistryView v) noexcept;

// One discovered J2534 adapter. Fields mirror the values the SAE
// J2534-1 spec asks every DLL to register; richer values (Vendor,
// CAN-channels, etc.) are read when present + concatenated into
// `extra`. We do not silently drop fields the spec didn't predict.
struct AdapterInfo {
    // Subkey name under PassThruSupport.04.04. Stable across runs;
    // useful as a key in user-config remembering "last chosen
    // adapter."
    std::string subkey;
    // Display name from the DLL's `Name` registry value. Falls
    // back to the subkey when missing.
    std::string name;
    // Filesystem path to the vendor DLL (`FunctionLibrary` value).
    // This is what J2534Library::load() will eventually take.
    std::string function_library;
    // Vendor / manufacturer string (`Vendor` value, optional).
    std::string vendor;
    // ProtocolsSupported bitmask (per the spec — bit per
    // ProtocolId). Zero when the value isn't present in the
    // registry, which is allowed by the spec and surprisingly
    // common in older vendor installers.
    std::uint32_t protocols_supported{0};
    // Which registry view (32-bit vs 64-bit) this entry was found
    // under. Affects which architecture host process can load
    // the DLL.
    RegistryView view{RegistryView::Native64};
};

// Walk the J2534 v04.04 registry keys and return every adapter
// found. On non-Windows hosts returns an empty vector (J2534
// vendor DLLs are Windows-only — Tactrix's macOS / Linux runtime
// uses a different distribution channel that doesn't surface
// through the registry).
//
// Read-only — never modifies the registry. Quiet — never logs.
// Errors talking to the registry (key missing, access denied) are
// treated as "no adapters of that view present" and contribute
// nothing to the result, NOT as fatal — a user with only one of
// the two views populated should still see their installed
// adapter.
//
// The two registry views are queried separately + concatenated;
// no dedup is performed (the same DLL CAN legitimately be
// registered twice if a vendor's installer touched both views).
[[nodiscard]] std::vector<AdapterInfo> discover_adapters();

// Format a ProtocolsSupported bitmask as a human-readable list of
// J2534 protocol names. Useful for the CLI's transport-list
// output + the eventual GUI's adapter chooser. Pure utility (no
// registry access).
//
//   format_protocols_supported(0x00000008 | 0x00000040)
//     → "ISO9141, ISO15765"
//
// Unknown bits are reported as their hex value:
//
//   format_protocols_supported(0x80000000)
//     → "(unknown: 0x80000000)"
//
// Returns "(none)" for a zero mask.
[[nodiscard]] std::string format_protocols_supported(std::uint32_t mask);

} // namespace st::transport::j2534

#endif // ST_TRANSPORT_J2534_DISCOVERY_HPP
