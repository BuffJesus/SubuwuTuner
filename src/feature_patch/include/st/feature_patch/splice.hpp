// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Splice mechanics — emit the few bytes that rewrite an existing
// instruction at `splice_address` so execution jumps to `patch_address`
// (the patch's allocated ROM region). The bytes returned are what the
// inserter writes back to the source ROM at `splice_address`.
//
// Two encoding forms per ISA:
//
//   Short form — single relative-branch instruction, reaches a limited
//                distance from the splice point. SH-2A: `BRA disp12 +
//                delay-slot NOP` (4 bytes, ±~4 KB). RH850: `JR disp22`
//                (4 bytes, ±2 MB).
//
//   Long form  — load-the-target-then-jump pattern, unlimited range.
//                SH-2A: `MOV.L @(disp, PC), Rn + JMP @Rn + delay-slot
//                NOP` + literal-pool entry (effectively 8 bytes of
//                displacement at the splice site). RH850: `MOVHI +
//                MOVEA + JMP [reg]` (12 bytes).
//
// `emit_sh2a_splice` picks the form automatically based on the
// computed displacement: short if it fits, long otherwise. Returns
// the bytes to overwrite at splice_address. Caller is responsible
// for the displaced-instruction audit (PC-relative refs in the
// displaced window break silently — `analyze_displacement` reports
// those before commit; see splice.cpp).
//
// This first slice implements short-form only; long-form lands with
// the JMP encoder + literal-pool placement in the next bundle.

#pragma once

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace st::feature_patch {

// Tag for which encoding the splice ended up using. The inserter
// uses this to decide how many original instructions it needs to
// preserve (short = 1 instruction displaced; long = 4).
enum class SpliceForm : std::uint8_t {
    Sh2aShortBra,  // BRA disp12 + NOP delay slot (4 bytes)
    Sh2aLongJmp,   // MOV.L @(d,PC), Rn + JMP @Rn + NOP + literal (8 bytes site)
    Rh850ShortJr,  // JR disp22 (4 bytes)
    Rh850LongJmp,  // MOVHI + MOVEA + JMP [reg] (12 bytes)
};

struct SpliceBytes {
    std::vector<std::uint8_t> bytes; // overwrite splice_address with these
    SpliceForm form;
    std::uint32_t splice_address;    // where the bytes go (echoed for caller)
    std::uint32_t patch_address;     // where execution lands
};

// Emit the SH-2A splice from `splice_address` to `patch_address`.
// Tries short form first (BRA disp12 + delay-slot NOP, 4 bytes,
// reach ±4 KB from PC+4). If the displacement doesn't fit, falls
// back to long form (NOT YET IMPLEMENTED — returns NotImplemented
// with a descriptive message; the next bundle adds it).
//
// SH-2A branch semantics:
//   - BRA disp12 target = PC + 4 + disp*2, where PC is the BRA
//     instruction's address. Disp is a signed 12-bit halfword
//     count; the target byte range from PC is [PC-4092, PC+4098].
//   - The instruction after BRA executes (delay slot) before the
//     branch lands; we fill that slot with NOP so the splice has
//     no side effect besides the branch itself.
//
// Both addresses must be 2-aligned (SH-2A is fixed 16-bit ISA);
// returns InvalidArgument otherwise. `splice_address == patch_address`
// is allowed but pointless (an infinite-loop NOP); returns ok with a
// branch-to-self.
[[nodiscard]] Result<SpliceBytes> emit_sh2a_splice(std::uint32_t splice_address,
                                                   std::uint32_t patch_address);

// SH-2A BRA displacement helpers (exposed for tests + diagnostics).
// `disp12_from_addrs` returns the signed 12-bit halfword displacement
// that BRA at `splice_address` would need to reach `patch_address`,
// or nullopt if the gap doesn't fit in 12 bits.
[[nodiscard]] std::optional<std::int16_t>
sh2a_bra_disp12(std::uint32_t splice_address, std::uint32_t patch_address) noexcept;

inline constexpr std::int32_t kSh2aBraMinByteOffset = -4092; // PC + 4 + (-2048 * 2)
inline constexpr std::int32_t kSh2aBraMaxByteOffset = 4098;  // PC + 4 + (2047 * 2)

// Emit the RH850 splice from `splice_address` to `patch_address`.
// Tries short form first (JR disp22, 4 bytes, reach ±~2 MB from PC).
// Falls back to long form (MOVHI + MOVEA + JMP [reg]) — NOT YET
// IMPLEMENTED; returns NotImplemented with a descriptive message in
// this slice.
//
// RH850 JR semantics:
//   - JR disp22 target = PC + disp22, where PC is the JR
//     instruction's address and disp22 is a signed 22-bit byte
//     displacement with implicit LSB = 0 (steps by 2).
//   - Range: [-2097152, +2097150] bytes from PC.
//
// Both addresses must be 2-aligned; returns InvalidArgument
// otherwise. The dual-bank gate (RH850 mustn't splice into the
// currently-executing bank) lives at a higher layer — `PatchInserter`
// in the next bundle — because bank state is a runtime property the
// splice encoder can't see.
[[nodiscard]] Result<SpliceBytes> emit_rh850_splice(std::uint32_t splice_address,
                                                    std::uint32_t patch_address);

// RH850 JR displacement helper (exposed for tests + diagnostics).
// Returns the signed 22-bit byte displacement that JR at
// `splice_address` would need to reach `patch_address`, or nullopt
// if the gap doesn't fit or the addresses are not 2-aligned.
[[nodiscard]] std::optional<std::int32_t>
rh850_jr_disp22(std::uint32_t splice_address, std::uint32_t patch_address) noexcept;

inline constexpr std::int32_t kRh850JrMinByteOffset = -2097152;
inline constexpr std::int32_t kRh850JrMaxByteOffset = 2097150;

} // namespace st::feature_patch
