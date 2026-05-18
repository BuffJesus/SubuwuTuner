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

// BT label  — branch if T=1. NO delay slot (the /S variant has one).
// 8-bit signed displacement in 16-bit-word units; target = PC + 4 +
// disp*2. Range: [PC-252, PC+258] bytes. Encoding: 1000 1001 dddddddd.
[[nodiscard]] constexpr std::uint16_t enc_bt(std::int8_t disp) noexcept {
    return static_cast<std::uint16_t>(
        0x8900U | static_cast<std::uint16_t>(static_cast<std::uint8_t>(disp)));
}

// BRA label  — unconditional branch WITH delay slot. 12-bit signed
// displacement (word units); target = PC + 4 + disp*2. Range:
// [PC-4092, PC+4098] bytes. Encoding: 1010 dddddddddddd.
[[nodiscard]] constexpr std::uint16_t enc_bra(std::int16_t disp) noexcept {
    return static_cast<std::uint16_t>(
        0xA000U
        | (static_cast<std::uint16_t>(disp) & 0x0FFFU));
}

// ---- FPU (single-precision, FPSCR.PR=0) -----------------------------
//
// SH-2A's FPU operates on FR0..FR15 single-precision registers (PR=0)
// or DR0..DR14 double-precision pairs (PR=1). We target single
// precision exclusively — Subaru ECU firmware works in float32 and
// our hook code inherits FPSCR.PR=0 from the host context. If we
// later need to defend against a host that flipped PR=1, the FADD
// chain will need an LDS Rm, FPSCR prologue to clear PR; v1.x
// assumes PR is already 0.
//
// All Float-arithmetic opcodes share the shape 1111 nnnn mmmm OOOO
// where OOOO is the operation:
//   0000 FADD, 0001 FSUB, 0010 FMUL, 0011 FDIV
// FRn is the destination AND the second source: FRn = FRn op FRm.

enum class FReg : std::uint8_t {
    FR0  = 0,  FR1  = 1,  FR2  = 2,  FR3  = 3,
    FR4  = 4,  FR5  = 5,  FR6  = 6,  FR7  = 7,
    FR8  = 8,  FR9  = 9,  FR10 = 10, FR11 = 11,
    FR12 = 12, FR13 = 13, FR14 = 14, FR15 = 15,
};

// FADD FRm, FRn — FRn = FRn + FRm. Encoding: 1111 nnnn mmmm 0000.
[[nodiscard]] constexpr std::uint16_t enc_fadd(FReg frm, FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF000U
        | (static_cast<std::uint16_t>(frn) << 8U)
        | (static_cast<std::uint16_t>(frm) << 4U));
}

// FSUB FRm, FRn — FRn = FRn - FRm. NOT commutative.
// Encoding: 1111 nnnn mmmm 0001.
[[nodiscard]] constexpr std::uint16_t enc_fsub(FReg frm, FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF001U
        | (static_cast<std::uint16_t>(frn) << 8U)
        | (static_cast<std::uint16_t>(frm) << 4U));
}

// FMUL FRm, FRn — FRn = FRn * FRm. Encoding: 1111 nnnn mmmm 0010.
[[nodiscard]] constexpr std::uint16_t enc_fmul(FReg frm, FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF002U
        | (static_cast<std::uint16_t>(frn) << 8U)
        | (static_cast<std::uint16_t>(frm) << 4U));
}

// FDIV FRm, FRn — FRn = FRn / FRm. NOT commutative.
// Encoding: 1111 nnnn mmmm 0011. Divide-by-zero raises a FPU
// exception per the SH-2A spec; for v1.x we mirror int divide_int's
// posture and trust the user to not author a zero-divisor (a
// divide-by-zero guard primitive is a later concern).
[[nodiscard]] constexpr std::uint16_t enc_fdiv(FReg frm, FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF003U
        | (static_cast<std::uint16_t>(frn) << 8U)
        | (static_cast<std::uint16_t>(frm) << 4U));
}

// LDS Rm, FPUL — FPUL = Rm (transfer GPR → FPU communications
// register). Used as the first step of "move int-bit pattern into
// FPU side." Encoding: 0100 mmmm 0101 1010.
[[nodiscard]] constexpr std::uint16_t enc_lds_r_fpul(Reg rm) noexcept {
    return static_cast<std::uint16_t>(
        0x405AU | (static_cast<std::uint16_t>(rm) << 8U));
}

// STS FPUL, Rn — Rn = FPUL (transfer FPU communications register →
// GPR). Used as the first step of "move FPU result back into the
// int-side store path." Encoding: 0000 nnnn 0101 1010.
[[nodiscard]] constexpr std::uint16_t enc_sts_fpul_r(Reg rn) noexcept {
    return static_cast<std::uint16_t>(
        0x005AU | (static_cast<std::uint16_t>(rn) << 8U));
}

// FSTS FPUL, FRn — FRn = FPUL. Bit-cast: copies the 32 bits of
// FPUL into FRn verbatim, no float conversion. Used to land an
// integer-typed bit pattern (just loaded into FPUL via LDS) into a
// floating-point register so FADD/FSUB/FMUL/FDIV can operate on it.
// Encoding: 1111 nnnn 0000 1101.
[[nodiscard]] constexpr std::uint16_t enc_fsts_fpul_freg(FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF00DU | (static_cast<std::uint16_t>(frn) << 8U));
}

// FLDS FRm, FPUL — FPUL = FRm. Mirror of fsts: bit-cast FRm into
// FPUL so STS FPUL, Rn can pull the bits out to a GPR for the
// memory store. Encoding: 1111 mmmm 0001 1101.
[[nodiscard]] constexpr std::uint16_t enc_flds_freg_fpul(FReg frm) noexcept {
    return static_cast<std::uint16_t>(
        0xF01DU | (static_cast<std::uint16_t>(frm) << 8U));
}

// FLOAT FPUL, FRn — FRn = (float32)(int32)FPUL. **Conversion**, not
// a bit-cast: reads FPUL as a signed 32-bit integer, converts to
// IEEE 754 binary32, stores in FRn. Used by `divide_int` to bring
// each integer operand onto the FPU register file so FDIV can run.
// Precision: int32 values with |x| > 2^24 may lose low-order bits
// in the conversion. Encoding: 1111 nnnn 0010 1101.
[[nodiscard]] constexpr std::uint16_t enc_float_fpul_freg(FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF02DU | (static_cast<std::uint16_t>(frn) << 8U));
}

// FTRC FRm, FPUL — FPUL = (int32)trunc(FRm). **Conversion**, not a
// bit-cast: truncates FRm toward zero (matching C int-division
// rounding) and stores the result in FPUL as a signed 32-bit int.
// Used to bring the float-side FDIV result back into the integer
// register file. Saturating on overflow per the SH-2A spec — values
// outside int32 range produce INT_MIN / INT_MAX, no trap.
// Encoding: 1111 mmmm 0011 1101.
[[nodiscard]] constexpr std::uint16_t enc_ftrc_freg_fpul(FReg frm) noexcept {
    return static_cast<std::uint16_t>(
        0xF03DU | (static_cast<std::uint16_t>(frm) << 8U));
}

// FCMP/EQ FRm, FRn — T = (FRn == FRm). IEEE 754 ordered compare: NaN
// operands produce T=0. Commutative — operand swap is identical.
// Encoding: 1111 nnnn mmmm 0100.
[[nodiscard]] constexpr std::uint16_t enc_fcmp_eq(FReg frm, FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF004U
        | (static_cast<std::uint16_t>(frn) << 8U)
        | (static_cast<std::uint16_t>(frm) << 4U));
}

// FCMP/GT FRm, FRn — T = (FRn > FRm), signed IEEE 754 ordered. NaN
// operands produce T=0. NOT commutative — callers pick the Rm/Rn
// mapping (mirror the integer `enc_cmp_gt` convention: `fcmp_gt(FR1,
// FR0)` after loading op1→FR0 / op2→FR1 yields T=(op1 > op2)).
// Encoding: 1111 nnnn mmmm 0101.
[[nodiscard]] constexpr std::uint16_t enc_fcmp_gt(FReg frm, FReg frn) noexcept {
    return static_cast<std::uint16_t>(
        0xF005U
        | (static_cast<std::uint16_t>(frn) << 8U)
        | (static_cast<std::uint16_t>(frm) << 4U));
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
