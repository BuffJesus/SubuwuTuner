// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_patch/splice.hpp"

#include <cstdint>
#include <string>

namespace st::feature_patch {

namespace {

// SH-2A BRA disp12 encoder. 1010 dddd dddd dddd; target = PC + 4 + disp*2
// where PC is the BRA instruction's address. Mirrors the equivalent
// helper in src/feature_codegen/src/sh2a.hpp (private to that target).
// Local copy here so feature_patch doesn't depend on feature_codegen
// for ISA encoder primitives; BRA's encoding is silicon-stable.
[[nodiscard]] constexpr std::uint16_t enc_bra(std::int16_t disp) noexcept {
    return static_cast<std::uint16_t>(0xA000U |
                                      (static_cast<std::uint16_t>(disp) & 0x0FFFU));
}

// SH-2A NOP. Encoding 0x0009. Fills delay slots so the splice has no
// side effect beyond the branch itself.
[[nodiscard]] constexpr std::uint16_t enc_nop() noexcept {
    return 0x0009U;
}

// MOV.L @(disp, PC), Rn — 8-bit unsigned disp, target =
// ((PC + 4) & ~3) + disp*4. Encoding 1101 nnnn dddddddd; for our
// long-form splice we always use R0 (n=0) and disp=1 (literal lives
// at splice_addr+8). Encoding for {R0, disp=1} = 0xD001.
[[nodiscard]] constexpr std::uint16_t enc_mov_l_at_pc_r0(std::uint8_t disp) noexcept {
    return static_cast<std::uint16_t>(0xD000U | disp);
}

// JMP @Rn — 0100 nnnn 0010 1011 = 0x402B | (n << 8). For R0: 0x402B.
[[nodiscard]] constexpr std::uint16_t enc_jmp_at_reg(std::uint8_t reg) noexcept {
    return static_cast<std::uint16_t>(0x402BU | (static_cast<std::uint32_t>(reg) << 8U));
}

// SH-2A is big-endian on the wire. Write a u16 in BE order.
void emit_be16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

// Write a u32 in BE order (for the inline literal).
void emit_be32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

// Emit the SH-2A long-form splice: a 12-byte sequence that reaches
// any 32-bit address. Requires splice_addr to be 4-aligned so the
// inline literal pool entry is naturally 4-aligned at splice_addr+8.
//
// Layout:
//   splice_addr+0 : MOV.L @(1, PC), R0   ; load patch_addr from offset 8
//   splice_addr+2 : JMP @R0
//   splice_addr+4 : NOP                  ; delay slot
//   splice_addr+6 : NOP                  ; pad for 4-aligned literal
//   splice_addr+8 : .long patch_address
//
// MOV.L's PC-relative target is `((PC + 4) & ~3) + disp*4`. With
// splice_addr 4-aligned, PC=splice_addr, (PC+4) is already 4-aligned,
// disp=1 → literal at splice_addr+8 ✓.
//
// CAVEAT: 12 bytes of original ROM at splice_addr are displaced.
// The inserter does NOT preserve those displaced instructions today
// — anyone splicing into the middle of a function with significant
// post-splice code needs to flag this. Mid-function splice
// preserve-and-prepend logic is queued for a follow-up bundle.
[[nodiscard]] SpliceBytes emit_sh2a_long_splice(std::uint32_t splice_address,
                                                std::uint32_t patch_address) {
    SpliceBytes out{};
    out.form = SpliceForm::Sh2aLongJmp;
    out.splice_address = splice_address;
    out.patch_address = patch_address;
    out.bytes.reserve(12);
    emit_be16(out.bytes, enc_mov_l_at_pc_r0(1));
    emit_be16(out.bytes, enc_jmp_at_reg(0));
    emit_be16(out.bytes, enc_nop()); // delay slot
    emit_be16(out.bytes, enc_nop()); // pad
    emit_be32(out.bytes, patch_address);
    return out;
}

} // namespace

std::optional<std::int16_t> sh2a_bra_disp12(std::uint32_t splice_address,
                                            std::uint32_t patch_address) noexcept {
    // target = splice + 4 + disp*2  ⇒  disp = (target - splice - 4) / 2
    auto const signed_gap = static_cast<std::int64_t>(patch_address) -
                            static_cast<std::int64_t>(splice_address) - 4;
    if ((signed_gap % 2) != 0) {
        return std::nullopt; // unaligned — SH-2A targets must be 2-aligned
    }
    auto const halfword_disp = signed_gap / 2;
    // 12-bit signed range: [-2048, 2047].
    if (halfword_disp < -2048 || halfword_disp > 2047) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(halfword_disp);
}

Result<SpliceBytes> emit_sh2a_splice(std::uint32_t splice_address,
                                     std::uint32_t patch_address) {
    if ((splice_address & 1U) != 0U) {
        return failure(ErrorCode::InvalidArgument,
                       "emit_sh2a_splice: splice_address must be 2-aligned "
                       "(SH-2A is a 16-bit fixed-instruction ISA)");
    }
    if ((patch_address & 1U) != 0U) {
        return failure(ErrorCode::InvalidArgument,
                       "emit_sh2a_splice: patch_address must be 2-aligned");
    }

    auto const disp = sh2a_bra_disp12(splice_address, patch_address);
    if (disp.has_value()) {
        SpliceBytes out{};
        out.form = SpliceForm::Sh2aShortBra;
        out.splice_address = splice_address;
        out.patch_address = patch_address;
        out.bytes.reserve(4);
        emit_be16(out.bytes, enc_bra(*disp));
        emit_be16(out.bytes, enc_nop()); // delay slot
        return out;
    }

    // Long form: MOV.L + JMP @R0 + delay-slot NOP + pad NOP + 4-byte
    // inline literal. 12 bytes total. Requires splice_address to be
    // 4-aligned so the inline literal-pool entry sits at a 4-aligned
    // address (the MOV.L disp8 math assumes PC+4 already-aligned).
    if ((splice_address & 0x3U) != 0U) {
        std::string msg{"emit_sh2a_splice: long-form requires 4-aligned splice_address "
                        "(got 0x"};
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", splice_address);
        msg.append(buf);
        msg.append(") — short-form BRA is out of range and long-form's "
                   "literal-pool math requires 4-byte alignment");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    return emit_sh2a_long_splice(splice_address, patch_address);
}

} // namespace st::feature_patch
