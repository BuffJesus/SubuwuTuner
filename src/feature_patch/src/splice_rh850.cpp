// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/splice.hpp"

#include <cstdint>
#include <string>

namespace st::feature_patch {

namespace {

// RH850 JR disp22 — Format V (32-bit relative jump, no register
// operands). Encoding sourced from the public binutils v850-opc.c
// operand table; the base encoding `two(0x0780, 0x0000)` with mask
// `two(0xffc0, 0x0001)` splits disp22 across two halfwords:
//
//   hw1: [00000111_10_DDDDDD]   — bits[5:0] hold disp22[21:16]
//   hw2: [DDDDDDDD_DDDDDDDD_0]  — bits[15:1] hold disp22[15:1];
//                                 bit 0 is always 0 (instructions
//                                 are 2-aligned, so the LSB of any
//                                 valid displacement is naturally 0)
//
// Target = PC + disp22, where PC is the JR instruction's address
// and disp22 is the signed 22-bit byte displacement.
//
// Range: signed 22-bit covers [-2097152, +2097150] byte offsets
// (steps by 2 because LSB is implicit zero) — ±~2 MB.
//
// RH850 is little-endian: each 16-bit halfword is written LSB-first.

[[nodiscard]] constexpr std::uint16_t enc_jr_hw1(std::int32_t disp22) noexcept {
    return static_cast<std::uint16_t>(
        0x0780U | (static_cast<std::uint32_t>(disp22 >> 16) & 0x3FU));
}

[[nodiscard]] constexpr std::uint16_t enc_jr_hw2(std::int32_t disp22) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(disp22) & 0xFFFEU);
}

void emit_le16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

} // namespace

std::optional<std::int32_t> rh850_jr_disp22(std::uint32_t splice_address,
                                            std::uint32_t patch_address) noexcept {
    auto const signed_gap = static_cast<std::int64_t>(patch_address) -
                            static_cast<std::int64_t>(splice_address);
    if ((signed_gap & 1) != 0) {
        return std::nullopt; // unaligned — RH850 targets must be 2-aligned
    }
    // 22-bit signed byte range, even-only (LSB always 0).
    constexpr std::int64_t kMin = -2097152;
    constexpr std::int64_t kMax = 2097150;
    if (signed_gap < kMin || signed_gap > kMax) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(signed_gap);
}

Result<SpliceBytes> emit_rh850_splice(std::uint32_t splice_address,
                                      std::uint32_t patch_address) {
    if ((splice_address & 1U) != 0U) {
        return failure(ErrorCode::InvalidArgument,
                       "emit_rh850_splice: splice_address must be 2-aligned");
    }
    if ((patch_address & 1U) != 0U) {
        return failure(ErrorCode::InvalidArgument,
                       "emit_rh850_splice: patch_address must be 2-aligned");
    }

    auto const disp = rh850_jr_disp22(splice_address, patch_address);
    if (disp.has_value()) {
        SpliceBytes out{};
        out.form = SpliceForm::Rh850ShortJr;
        out.splice_address = splice_address;
        out.patch_address = patch_address;
        out.bytes.reserve(4);
        emit_le16(out.bytes, enc_jr_hw1(*disp));
        emit_le16(out.bytes, enc_jr_hw2(*disp));
        return out;
    }

    // Long form (MOVHI + MOVEA + JMP [reg], 12 bytes) lands in the
    // next bundle. Same shape as the existing emit_rh850_*
    // fragments' JMP[lp] tail in feature_codegen but standalone
    // for splice use; needs a dual-bank-aware guard on the target.
    auto const signed_gap = static_cast<std::int64_t>(patch_address) -
                            static_cast<std::int64_t>(splice_address);
    std::string msg{"emit_rh850_splice: splice→patch displacement "};
    msg.append(std::to_string(signed_gap));
    msg.append(" bytes is outside JR range [-2097152, 2097150] — "
               "long-form (MOVHI + MOVEA + JMP [reg]) not yet "
               "implemented in this slice");
    return failure(ErrorCode::NotImplemented, std::move(msg));
}

} // namespace st::feature_patch
