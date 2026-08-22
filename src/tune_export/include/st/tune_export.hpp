// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::tune_export — sum-preserving tune image builder for Subaru
// LF79xxxP FA20DIT firmware.
//
// Spec: docs/44-tune-export.md. Round-58 + round-59 analyst RE.
// The brick-protection contract on this firmware is
//   `u16 BE sum(0x6000..0x200000) == 0x5AA5`
// enforced by the commit handler at ROM 0x319E. COBB and after-
// market tunes preserve the sum by writing a 2-byte "balance" cell
// at flash offset `0x1FFFFE..0x1FFFFF` that compensates for any
// sum delta the rest of the tune introduces. Bit-exact verified
// against a real Fehr-plus-decat tune.
//
// CID portability: all members of the LF79xxxP family share the
// same invariants (sum target, boot integrity sigs, FACI window,
// trailer reserved region, balance cell). Round-59 T3#7 confirmed
// cross-CID against LF79002P (bench) and LF79101P (user's car
// family). Pipeline is family-agnostic for invariants but CID-
// specific for the base ROM that the caller provides.
//
// Verification protocol: emit_write_plan blocks must be verified
// via cross-power-cycle SSM-A8 RMBA in default session. `0xB7`
// readback during the active Programming session reads the FCU
// staging buffer, NOT real flash. Round-57's brick proved this.

#ifndef ST_TUNE_EXPORT_HPP
#define ST_TUNE_EXPORT_HPP

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::tune_export {

// ---- Architecture invariants ------------------------------------------
// All values are bench-RE-confirmed in the round-58/59 analyst
// handoffs and bit-exact verified against an actual Fehr decat
// tune. CID-portable across the LF79xxxP family.

inline constexpr std::uint16_t kSumTarget          = 0x5AA5U;
inline constexpr std::uint32_t kSumRangeStart      = 0x006000U;   // inclusive
inline constexpr std::uint32_t kSumRangeEnd        = 0x200000U;   // exclusive

inline constexpr std::uint32_t kFaciLockedEnd      = 0x008000U;   // exclusive
inline constexpr std::uint32_t kWritableStart      = 0x008000U;
inline constexpr std::uint32_t kWritableEnd        = 0x200000U;   // exclusive
inline constexpr std::uint32_t kWdtTrapStart       = 0x200000U;

inline constexpr std::uint32_t kTrailerReservedStart = 0x1FFFE0U;
inline constexpr std::uint32_t kTrailerReservedEnd   = 0x1FFFFEU;  // up to (not incl) balance
inline constexpr std::uint32_t kBalanceOffset      = 0x1FFFFEU;
inline constexpr std::uint32_t kBalanceLength      = 2U;

inline constexpr std::uint32_t kBlockSize          = 256U;

// Boot integrity check addresses that must NOT be modified by any
// CalDiff. From round-59 T3#7 cross-CID verification: these bytes
// are family-constant across LF79xxxP and the boot integrity
// check at FUN_00000C54 reads them on every cold start.
inline constexpr std::array<std::uint32_t, 5>
kBootIntegrityBytesMustBePreserved = {
    0x00006CU,   // u8 paired-equality byte (mirrors high byte of *0x6010)
    0x006000U,   // u16 BE flash sig 1 (stock = 0x5555)
    0x006010U,   // u16 BE paired-equality cell (stock = 0x0504)
    0x1FFFF2U,   // u16 BE flash sig 2 (stock = 0xAAAA)
    0x1FFFF3U,   // u16 BE sig 2 trailing byte (paired with 0x1FFFF2)
};

// ---- Inputs / outputs --------------------------------------------------

// One contiguous calibration patch to apply to the base ROM.
// `offset` is a flash address (matches ECU address space, not file
// offset); `bytes` is the new content for that address.
struct CalDiff {
    std::uint32_t              offset{0};
    std::vector<std::uint8_t>  bytes;
};

// Reason a CalDiff (or the whole tune) was rejected by validate().
// One struct per violation; validate() returns all violations so a
// UI can render every red squiggle at once.
struct ValidationError {
    enum class Kind {
        FaciLockedAddress,      // diff in [0x0, 0x8000) — silent F6 lie
        WdtTrapAddress,         // diff at or past 0x200000 — WDT reset
        BalanceClobber,         // diff overlaps 0x1FFFFE..0x1FFFFF
        TrailerClobber,         // diff overlaps [0x1FFFE0, 0x1FFFFE)
        BootIntegrityClobber,   // diff modifies a byte boot integrity reads
        BaseSizeMismatch,       // base ROM is not exactly 0x200000 bytes
    };
    Kind          kind;
    std::uint32_t offending_offset{0};
    std::string   explanation;
};

// One 256-byte write block, 256-byte aligned. Emitted in monotonic
// ascending address order EXCEPT the trailer block (containing
// kBalanceOffset) which is intentionally LAST so a partial install
// leaves the integrity sum failing — caught on next boot. `tag`
// labels the block's role for logging / progress reporting.
struct WriteBlock {
    enum class Tag {
        CalChange,         // contains one or more user CalDiff bytes
        BalanceWrite,      // trailer block carrying the recomputed balance
        RestoreFromBase,   // unchanged-from-base, emitted only when
                           // surrounded by changes (reserved for future
                           // intra-block coalescing)
    };
    std::uint32_t                     addr{0};
    std::array<std::uint8_t, 256>     data{};
    Tag                               tag{Tag::CalChange};
};

// ---- Free functions ----------------------------------------------------

// u16 big-endian rolling sum over `[a, e)`. Pure helper exposed
// for testing the brick-protection contract directly. Walks u16
// pairs in mod-2^16 to match the firmware's naive accumulator at
// ROM 0x3582.
[[nodiscard]] std::uint16_t
u16be_sum(std::span<std::uint8_t const> bytes,
          std::uint32_t                 a,
          std::uint32_t                 e) noexcept;

// Compute the u16 BE balance value that, when stored at
// kBalanceOffset, makes
//   `u16be_sum(rom, kSumRangeStart, kSumRangeEnd) == kSumTarget`.
// Input ROM is treated as if the balance cell were zero; returned
// value is what should be written there.
[[nodiscard]] std::uint16_t
compute_balance(std::span<std::uint8_t const> rom_zeroed_balance) noexcept;

// Check every CalDiff against the architecture invariants. Returns
// EVERY violation it finds; an empty vector means all clear. The
// base ROM is optional (pass empty span when only diff geometry
// matters); when present, base size is also validated.
[[nodiscard]] std::vector<ValidationError>
validate(std::span<std::uint8_t const> base,
         std::span<CalDiff const>      diffs);

// Build a sum-preserving image from `base` + `diffs`. Returns
// exactly kWritableEnd bytes — addresses match ECU flash 1:1.
// Validates first; on any violation, fails with PolicyDenied and
// the first violation's explanation as the error message. Use
// `validate()` directly to surface all violations to a UI.
[[nodiscard]] Result<std::vector<std::uint8_t>>
build_image(std::span<std::uint8_t const> base,
            std::span<CalDiff const>      diffs);

// Lay out 256-byte B6 write blocks from `built_image`. Blocks
// where every byte matches `base_rom` are SKIPPED (round-59 T3#8
// unchanged-block optimization — saves ~8000 bench writes for a
// small tune). The trailer block containing kBalanceOffset is
// emitted LAST regardless of whether it would otherwise be
// skipped — the balance cell is always (re)written.
//
// Returns empty vector if either span is not exactly kWritableEnd
// bytes.
[[nodiscard]] std::vector<WriteBlock>
emit_write_plan(std::span<std::uint8_t const> built_image,
                std::span<std::uint8_t const> base_rom);

// Human-readable label for a ValidationError::Kind.
[[nodiscard]] std::string_view
to_string(ValidationError::Kind k) noexcept;

// ---- Flash-ready checksum state ---------------------------------------
//
// The "flash-ready checksum" is a DIFFERENT thing from project integrity.
// Project integrity (st::Project::verify_persisted_state) asks "is the
// on-disk project self-consistent?". This asks "does the working ROM
// satisfy the ECU's brick-protection sum contract so a flash won't be
// rejected (NRC 0x22) or boot into a corrupt app?". A project can be
// perfectly coherent and still NOT be flash-ready because a cal edit
// moved the sum off 0x5AA5 without re-tuning the balance cell.
//
// These queries target the LF79xxxP 2 MB SH-2A family (kWritableEnd-sized
// images). For any other image size the answer is NotApplicable — the
// 2.5 MB A.3 and RH850 windows differ and are out of scope here.

enum class ChecksumState : std::uint8_t {
    NotApplicable, // image is not a 2 MB SH-2A ROM — contract unknown here
    FlashReady,    // u16be sum over [kSumRangeStart, kSumRangeEnd) == kSumTarget
    NeedsRepair,   // sum != target — rewrite the balance cell before flashing
};

[[nodiscard]] std::string_view to_string(ChecksumState s) noexcept;

// The u16 BE sum the ECU's commit gate checks, over the 2 MB app window.
// A FlashReady ROM sums to kSumTarget (0x5AA5). Returns 0 for a ROM that
// is too short to cover the window (also reported as NotApplicable by
// checksum_state).
[[nodiscard]] std::uint16_t
flash_checksum(std::span<std::uint8_t const> rom) noexcept;

// Classify a working ROM's flash-ready checksum state (see enum above).
[[nodiscard]] ChecksumState
checksum_state(std::span<std::uint8_t const> rom) noexcept;

// Re-tune the compensating balance word at kBalanceOffset so the ROM
// sums to kSumTarget. Idempotent: repairing an already-FlashReady ROM
// leaves it byte-identical. Returns InvalidArgument for a non-2 MB image
// (a size where the contract does not apply). This is the ONLY byte the
// repair touches — every cal edit is preserved.
[[nodiscard]] st::Status repair_balance(std::span<std::uint8_t> rom) noexcept;

}  // namespace st::tune_export

#endif  // ST_TUNE_EXPORT_HPP
