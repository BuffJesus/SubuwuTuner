// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::flash — bootloader-signature host-side verifier.
//
// The LF79103P bootloader (FUN_000000E8 reset entry → FUN_00000C54
// signature check, per analyst handoff 2026-06-06,
// `findings/APP_CHECKSUM_VERIFICATION.md`) gates the jump from
// bootloader to application on three fixed-pattern signatures
// resident in flash:
//
//   - `*(uint16_t*)0x006000  == 0x5555`   (BE)
//   - `*(uint16_t*)0x1FFFF2  == 0xAAAA`   (BE)
//   - `*(uint16_t*)0x00006C  == *(uint16_t*)0x006010`
//
// If any of these fails, bit 1 of MMIO `0xFFF99819` is set and the
// bootloader stays in waiting-for-reprogram mode (the jump target
// at `0x001F094C` is not reached). A separate RAM consistency hash
// over `0xFFF82008..0xFFF82040` is part of the same decision but
// does not constrain the flash image — only the three flash
// signatures do.
//
// This module is the host-side pre-flight: run on the working
// buffer before sending bytes to the ECU, fail any image that
// wouldn't boot. Verified against a factory-virgin 2 MB SH-2A
// reference ROM and an independently-produced aftermarket-installed
// reference ROM (both pass the three-signature check). Cross-CID
// confirmation across other LF79xxxP variants is a Tier-4
// follow-up.

#ifndef ST_FLASH_BOOT_SIGNATURES_HPP
#define ST_FLASH_BOOT_SIGNATURES_HPP

#include <cstdint>
#include <span>
#include <string_view>

namespace st::flash {

enum class BootSignatureFailure : std::uint8_t {
    Ok = 0,
    TooSmall,      // image span doesn't cover all signature offsets
    SigA,          // 0x006000 doesn't read 0x5555
    SigB,          // 0x1FFFF2 doesn't read 0xAAAA
    PairMismatch,  // 0x00006C and 0x006010 differ
};

struct BootSignatureReport {
    BootSignatureFailure failure{BootSignatureFailure::Ok};
    std::uint16_t sig_a_actual{0};   // BE u16 at 0x006000
    std::uint16_t sig_b_actual{0};   // BE u16 at 0x1FFFF2
    std::uint16_t pair_at_6c{0};     // BE u16 at 0x00006C
    std::uint16_t pair_at_6010{0};   // BE u16 at 0x006010

    [[nodiscard]] bool ok() const noexcept {
        return failure == BootSignatureFailure::Ok;
    }
};

// Bookkeeping constants from FUN_00000C54. Public so the CLI's
// preflight printer and tests can refer to them without
// duplicating literals.
inline constexpr std::uint32_t kBootSigAOffset = 0x006000u;
inline constexpr std::uint32_t kBootSigBOffset = 0x1FFFF2u;
inline constexpr std::uint32_t kBootPairLowOffset = 0x00006Cu;
inline constexpr std::uint32_t kBootPairHighOffset = 0x006010u;
inline constexpr std::uint16_t kBootSigAExpected = 0x5555u;
inline constexpr std::uint16_t kBootSigBExpected = 0xAAAAu;

// Run all three signature checks on a 2 MB SH-2A FA-DIT image
// (LF79103P-verified, presumed-portable across the LF79xxxP family).
// Pure read — never mutates. Image too short fails fast with
// TooSmall; partial reports for SigA / SigB / PairMismatch include
// the actual bytes for diagnostics. Multiple failures collapse to
// the first one encountered in offset order.
[[nodiscard]] BootSignatureReport
verify_boot_signatures_sh2a_2mb(std::span<std::uint8_t const> image) noexcept;

// Human-readable failure-code label. Stable strings — safe to log,
// match in tests, surface in CLI / GUI output.
[[nodiscard]] std::string_view
boot_signature_failure_name(BootSignatureFailure f) noexcept;

} // namespace st::flash

#endif // ST_FLASH_BOOT_SIGNATURES_HPP
