// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Internal header — SH-2A instruction encoders used by Sh2aBackend.
// Not exposed via `st/feature_codegen.hpp`; lives alongside the impl.
//
// Each encoder returns a raw 16-bit value; the emitter is responsible
// for writing it big-endian into the byte stream (SH-2A is BE; SH-2
// has selectable endianness but Subaru ships BE for these ECUs per
// public references). Encodings sourced from public Renesas SH-2A
// architecture references (the Renesas hardware manual is the canon)
// — fact-only, no expression copied.

#ifndef ST_FEATURE_CODEGEN_SH2A_HPP
#define ST_FEATURE_CODEGEN_SH2A_HPP

#include <cstdint>

namespace st::feature::codegen::sh2a {

// All SH-2A general-purpose registers are 4-bit fields. We tag the
// type so call sites can't confuse register indices with displacement
// values (both look like uint8_t).
enum class Reg : std::uint8_t {
    R0  = 0,  R1  = 1,  R2  = 2,  R3  = 3,
    R4  = 4,  R5  = 5,  R6  = 6,  R7  = 7,
    R8  = 8,  R9  = 9,  R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

// MOV.L @(disp, PC), Rn  — load 32-bit value from the literal pool
// at `((this_pc + 4) & ~3) + disp*4` into Rn.
//
// Encoding: 1101 nnnn dddddddd, where nnnn = Rn and dddddddd is the
// unsigned displacement in longwords (0..255). `disp` must fit in 8
// bits; out-of-range callers get an assertion at compile-time-equivalent
// — undefined behaviour means the emitter must range-check before
// calling.
[[nodiscard]] constexpr std::uint16_t enc_mov_l_disp_pc(Reg rn,
                                                         std::uint8_t disp) noexcept {
    return static_cast<std::uint16_t>(
        0xD000U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | static_cast<std::uint16_t>(disp));
}

// MOV.L Rm, @Rn  — store the 32-bit value in Rm to the address in Rn.
// Encoding: 0010 nnnn mmmm 0010.
[[nodiscard]] constexpr std::uint16_t enc_mov_l_reg_at_reg(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x2002U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// MOV.L @Rm, Rn  — load the 32-bit value at the address in Rm into Rn.
// Encoding: 0110 nnnn mmmm 0010.
[[nodiscard]] constexpr std::uint16_t enc_mov_l_at_reg_reg(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x6002U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// ADD Rm, Rn  — Rn = Rn + Rm. 32-bit signed (or unsigned — two's-
// complement wrap is identical). Encoding: 0011 nnnn mmmm 1100.
[[nodiscard]] constexpr std::uint16_t enc_add(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x300CU
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// SUB Rm, Rn  — Rn = Rn - Rm. Encoding: 0011 nnnn mmmm 1000.
// Caller convention: put the minuend in Rn and the subtrahend in Rm
// so the result lands in Rn following the codegen's "result in R1"
// convention.
[[nodiscard]] constexpr std::uint16_t enc_sub(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x3008U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// MUL.L Rm, Rn  — MACL = Rn * Rm (low 32 bits of the 64-bit signed
// product; the high 32 bits land in MACH). The result is in the
// MACL system register, not a GPR — callers must follow up with
// STS MACL, Rn to extract the value. Encoding: 0000 nnnn mmmm 0111.
[[nodiscard]] constexpr std::uint16_t enc_mul_l(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x0007U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// STS MACL, Rn  — Rn = MACL. Used after MUL.L to copy the low 32
// bits of the multiplication result into a general-purpose register.
// Encoding: 0000 nnnn 0001 1010.
[[nodiscard]] constexpr std::uint16_t enc_sts_macl(Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x001AU
        | (static_cast<std::uint16_t>(rn) << 8U));
}

// CMP/EQ Rm, Rn  — T = (Rn == Rm). Commutative — operand swap
// produces the same result. Encoding: 0011 nnnn mmmm 0000.
[[nodiscard]] constexpr std::uint16_t enc_cmp_eq(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x3000U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// CMP/GT Rm, Rn  — T = (Rn > Rm) signed. NOT commutative; callers
// pick the operand placement to express either `<` (CMP/GT R0, R1
// for "R1 > R0 = b > a = a < b") or `>` (CMP/GT R1, R0 for
// "R0 > R1 = a > b"). Encoding: 0011 nnnn mmmm 0111.
[[nodiscard]] constexpr std::uint16_t enc_cmp_gt(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x3007U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// MOVT Rn  — Rn = T (zero-extended; result is 0 or 1). Materializes
// the comparison outcome as a 32-bit register value so it can be
// stored to memory like any other SSA value. Encoding:
// 0000 nnnn 0010 1001.
[[nodiscard]] constexpr std::uint16_t enc_movt(Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x0029U
        | (static_cast<std::uint16_t>(rn) << 8U));
}

// AND Rm, Rn  — Rn = Rn & Rm. Bitwise AND of two 32-bit registers.
// On canonical 0/1 Bool inputs, produces canonical 0/1 logical AND.
// Encoding: 0010 nnnn mmmm 1001.
[[nodiscard]] constexpr std::uint16_t enc_and(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x2009U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// OR Rm, Rn  — Rn = Rn | Rm. Bitwise OR. On canonical 0/1 Bool
// inputs, produces canonical 0/1 logical OR.
// Encoding: 0010 nnnn mmmm 1011.
[[nodiscard]] constexpr std::uint16_t enc_or(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x200BU
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// TST Rm, Rn  — T = ((Rn & Rm) == 0). Used as the "is zero" test for
// logical NOT: TST Rn, Rn followed by MOVT flips 0↔1. Encoding:
// 0010 nnnn mmmm 1000.
[[nodiscard]] constexpr std::uint16_t enc_tst(Reg rm, Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x2008U
        | (static_cast<std::uint16_t>(rn) << 8U)
        | (static_cast<std::uint16_t>(rm) << 4U));
}

// RTS — return from subroutine. Encoded as 0x000B.
[[nodiscard]] constexpr std::uint16_t enc_rts() noexcept { return 0x000BU; }

// NOP — no operation. Encoded as 0x0009. Used as the delay-slot
// instruction after RTS and as padding to 4-byte-align the literal
// pool.
[[nodiscard]] constexpr std::uint16_t enc_nop() noexcept { return 0x0009U; }

// Layout constants for the canonical "load constant, store to RAM
// slot" sequence emitted by the LoadConstant slice live on the public
// API surface — see `kStoreSequenceSize` etc. in
// `st/feature_codegen.hpp` under namespace `sh2a`.

} // namespace st::feature::codegen::sh2a

#endif // ST_FEATURE_CODEGEN_SH2A_HPP
