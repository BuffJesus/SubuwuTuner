// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::flash — post-write checksum repair.
//
// After any flash that modifies calibration bytes, the ECU's
// firmware will refuse to boot (or trip a sanity check) unless
// the in-image checksum bytes match the calibration content.
// Different ECU families use different algorithms; the pack's
// `[pack].checksum_type` field (added in d68d796) selects which.
//
// This header defines the seam:
//   - ChecksumKind enum mirroring RomRaider's ChecksumXxx family
//   - parse_checksum_kind / checksum_kind_from_pack helpers
//   - IChecksumRepair interface (consumed by st::flash::Flasher
//     during program → verify)
//   - make_checksum_repair factory
//
// **Status today**: every concrete algorithm (SubaruStd, SubaruAlt,
// SubaruAlt2) returns NotImplemented with a message naming where
// the algorithm lives in public references. The interface +
// factory + dispatch are real; the math fills in per-kind when we
// have a stock ROM to byte-validate against.

#ifndef ST_FLASH_CHECKSUM_HPP
#define ST_FLASH_CHECKSUM_HPP

#include "st/core/result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace st::flash {

// Mirrors RomRaider's class family + the docs/11 enum values. New
// kinds get added at the end (None stays 0 for "no checksum" /
// default-constructed packs).
enum class ChecksumKind : std::uint8_t {
    None = 0,       // no-op repair (pack didn't declare a kind)
    SubaruStd = 1,  // RR ChecksumSTD — most VA + early VB ECUs
    SubaruAlt = 2,  // RR ChecksumALT — older 16-bit families
    SubaruAlt2 = 3, // RR ChecksumALT2 — some 32-bit ROMs (per RR release notes)
};

// CLI-canonical string form (lowercase, snake_case). Matches the
// value packs declare in `[pack].checksum_type`.
[[nodiscard]] char const *checksum_kind_name(ChecksumKind k) noexcept;

// Inverse of checksum_kind_name. Returns nullopt for any string
// not on the recognized list — caller decides whether that's a
// hard error or a "treat as None" default.
[[nodiscard]] std::optional<ChecksumKind> parse_checksum_kind(std::string_view s) noexcept;

// Lenient parse for the pack field: empty / unrecognized → None.
// Used by the Flasher (and by future tooling) to map
// Pack::checksum_type → ChecksumKind without requiring callers to
// handle the "no kind declared" case separately.
[[nodiscard]] ChecksumKind checksum_kind_from_pack(std::string_view checksum_type_field) noexcept;

// Repair the checksum bytes in a ROM image in place. The span
// covers the ENTIRE ROM (not just the checksum bytes) — the impl
// reads calibration bytes to compute the new value + writes the
// checksum slot. Repair is idempotent: calling it on an already-
// correct ROM is a no-op.
//
// Returns:
//   ok()             — checksum repaired (or was already valid).
//   NotImplemented   — algorithm exists in spec but not yet
//                      implemented in this build. Today every
//                      Subaru* impl returns this.
//   InvalidArgument  — ROM too short, malformed, or doesn't carry
//                      the bytes the algorithm expects.
class IChecksumRepair {
public:
    IChecksumRepair() = default;
    virtual ~IChecksumRepair() = default;
    IChecksumRepair(IChecksumRepair const &) = delete;
    IChecksumRepair &operator=(IChecksumRepair const &) = delete;
    IChecksumRepair(IChecksumRepair &&) noexcept = default;
    IChecksumRepair &operator=(IChecksumRepair &&) noexcept = default;

    [[nodiscard]] virtual st::Status repair(std::span<std::uint8_t> rom_bytes) noexcept = 0;

    // Stable identifier for logs + the FlashReport. Matches
    // checksum_kind_name on the wrapped kind.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// Construct a concrete repair impl. Never returns nullptr — the
// `None` kind returns a working no-op implementation so callers
// can blindly invoke `repair()` without branching on "did the
// pack declare a kind?"
[[nodiscard]] std::unique_ptr<IChecksumRepair> make_checksum_repair(ChecksumKind kind);

// ---- SubaruStd configuration -----------------------------------------
//
// SubaruStd needs to know which bytes participate in the sum and where
// the two checksum slots live. Per the public algorithm shape
// (RomRaider's ChecksumSTD + Subaru tuning literature):
//
//   - The cal region is summed as 16-bit big-endian words mod 2^32.
//   - The two slot bytes are SKIPPED while summing (otherwise the
//     algorithm is self-referential).
//   - The lower-address slot stores the sum.
//   - The upper-address slot stores `~sum` (one's complement). Boot-
//     time integrity check then verifies sum + complement == 0xFFFFFFFF.
//
// Defaults below target the common SH-2A 2 MB family (SH72546R) — the
// 2017 WRX FA20DIT ECU lives here. For other ROM sizes / families
// supply an explicit config.
struct SubaruStdConfig {
    std::uint32_t cal_start{0x10000};     // first byte included in the sum
    std::uint32_t cal_end{0x200000};      // one-past-last (exclusive upper bound)
    std::uint32_t sum_slot{0x100};        // offset of the 4-byte sum word
    std::uint32_t complement_slot{0x104}; // offset of the 4-byte ~sum word

    // Validation invariants used by make_checksum_repair before
    // constructing the impl. cal_end > cal_start; both slots
    // 4-byte-aligned; slots distinct; slots within cal_start..cal_end.
    // Returns the first invariant violated (empty when valid).
    [[nodiscard]] std::string_view validate() const noexcept;
};

// Construct a SubaruStd repair with explicit slot config. Useful when
// the platform isn't the default SH-2A 2 MB layout.
[[nodiscard]] std::unique_ptr<IChecksumRepair>
make_subaru_std_repair(SubaruStdConfig const &cfg);

} // namespace st::flash

// Forward-declared in st:: so apply_checksum_repair can take a
// Definition without dragging the full defs.hpp into flash/checksum.
namespace st {
class Definition;
}

namespace st::flash {

// Convenience: pack lookup + factory + repair in one call. The
// Definition's pack.checksum_type field selects the kind via
// checksum_kind_from_pack (lenient — empty / unrecognized → None
// → no-op). One entry point for the Flasher integration, the CLI
// checksum-repair command, and any future "pre-flash prep" code
// path that needs to repair bytes before they touch hardware.
//
// Operates in place on the span (typically a working-ROM byte
// buffer). Returns whatever the underlying repair() returned:
// ok() for None, NotImplemented for any concrete Subaru kind
// today, and eventually the real algorithm's result once the
// implementations land.
[[nodiscard]] st::Status apply_checksum_repair(std::span<std::uint8_t> rom_bytes,
                                               Definition const &def) noexcept;

} // namespace st::flash

#endif // ST_FLASH_CHECKSUM_HPP
