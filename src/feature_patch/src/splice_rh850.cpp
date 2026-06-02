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

// MOVHI imm16, reg1, reg2 — Format VI; reg2 = (imm16 << 16) | (reg1
// upper 16 bits maybe sign-extended). For the splice we use reg1=0
// (R0, always zero) so the result is just imm16 << 16. Encoding per
// the public Renesas reference: 0b110001 opcode in hw1 + imm16 in hw2.
// hw1 = (reg2 << 11) | (0b110001 << 5) | reg1; reg1=0, scratch reg=1.
[[nodiscard]] constexpr std::uint16_t enc_movhi_hw1(std::uint8_t reg1, std::uint8_t reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b110001U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) | (kOpcode << 5U) | reg1);
}

// MOVEA imm16, reg1, reg2 — Format VI; reg2 = reg1 + sign-ext(imm16).
// Encoding 0b110000 + same shape as MOVHI. Used to add the low 16
// bits of the target address to the upper-shifted result of MOVHI.
[[nodiscard]] constexpr std::uint16_t enc_movea_hw1(std::uint8_t reg1, std::uint8_t reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b110000U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) | (kOpcode << 5U) | reg1);
}

// JMP [reg1] — Format I, opcode 0b000110, reg2 field zero. Same
// encoder as feature_codegen/src/rh850.hpp::enc_jmp_reg; inlined
// here to keep feature_patch independent of that header.
[[nodiscard]] constexpr std::uint16_t enc_jmp_reg(std::uint8_t reg) noexcept {
    constexpr std::uint16_t kOpcode = 0b000110U;
    return static_cast<std::uint16_t>((kOpcode << 5U) | reg);
}

// split_imm32 — MOVHI uses the high 16 bits of a 32-bit address as
// the upper-shifted immediate; MOVEA's low 16 bits are sign-extended
// when added, so if bit 15 of the low half is set we need to bump
// the high half by 1 to compensate. Standard RH850 32-bit-immediate
// materialization pattern; mirrors what's in feature_codegen.
struct Imm32Split {
    std::uint16_t hi;
    std::uint16_t lo;
};
[[nodiscard]] constexpr Imm32Split split_imm32(std::uint32_t value) noexcept {
    auto const lo = static_cast<std::uint16_t>(value & 0xFFFFU);
    auto hi = static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU);
    if ((lo & 0x8000U) != 0U) {
        // MOVEA will sign-extend lo — compensate by pre-incrementing hi.
        hi = static_cast<std::uint16_t>(hi + 1U);
    }
    return {hi, lo};
}

// Emit the RH850 long-form splice: MOVHI + MOVEA + JMP [reg], 10
// bytes total. Reaches any 32-bit address. Uses R1 as scratch for
// the materialized address — R1 is caller-save on the RH850 ABI
// and not in the codegen's reserved working-register set
// (r10..r14), so trampoline use here doesn't conflict with the
// patch body's register state.
//
// CAVEAT: 10 bytes of original ROM at splice_addr are displaced.
// Same preserve-or-flag caveat as the SH-2A long form.
[[nodiscard]] SpliceBytes emit_rh850_long_splice(std::uint32_t splice_address,
                                                 std::uint32_t patch_address) {
    constexpr std::uint8_t kZero = 0;
    constexpr std::uint8_t kScratch = 1;
    auto const sp = split_imm32(patch_address);
    SpliceBytes out{};
    out.form = SpliceForm::Rh850LongJmp;
    out.splice_address = splice_address;
    out.patch_address = patch_address;
    out.bytes.reserve(10);
    emit_le16(out.bytes, enc_movhi_hw1(kZero, kScratch));
    emit_le16(out.bytes, sp.hi);
    emit_le16(out.bytes, enc_movea_hw1(kScratch, kScratch));
    emit_le16(out.bytes, sp.lo);
    emit_le16(out.bytes, enc_jmp_reg(kScratch));
    return out;
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

    // Long form: MOVHI + MOVEA + JMP [reg], 10 bytes total. Reaches
    // any 32-bit address. RH850 has no delay slots, so no NOP is
    // required after JMP. The 10-byte length leaves bytes at
    // splice_addr+10 and +11 unmodified — they may still hold the
    // original ROM bytes, but execution never reaches them post-JMP.
    return emit_rh850_long_splice(splice_address, patch_address);
}

} // namespace st::feature_patch
