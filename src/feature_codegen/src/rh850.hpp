// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Internal header — RH850 instruction encoders used by Rh850Backend.
// Mirror of sh2a.hpp's role: not exposed via `st/feature_codegen.hpp`;
// lives alongside the impl. Encodings sourced from the public Renesas
// RH850 architecture documentation (R01US0165, "RH850G3K Software
// Architecture User's Manual") — fact-only, no expression copied.
//
// =====================================================================
// VERIFICATION STATUS — read before flashing any RH850 output
// =====================================================================
//
// RH850 codegen has NOT been validated against a real VB WRX ECU. The
// encodings in this header were chosen against the public Renesas
// reference, but RH850 has multiple cores (G3K / G3M / G3MH / G4MH) and
// minor variants disagree on specific opcode bits. Until the bench rig
// has demonstrated a successful RH850 patch round-trip (compile → flash
// → read-back → verify), any RH850 PatchObject produced by this layer
// is treated as "best effort" and must be gated behind explicit user
// confirmation before reaching `st::flash` against a real ECU.
//
// To verify after the bench rig is online:
//   1. Compile a known-trivial graph (one LoadConstant → StoreHookOutput).
//   2. Disassemble the emitted bytes with a public RH850 disassembler
//      and confirm each instruction matches the intended mnemonic.
//   3. Patch a junkyard VB ECU, read back the patched region, confirm
//      byte-identical.
//   4. Boot the ECU, confirm normal operation (the hook should be
//      effectively a no-op since the store is to a scratch RAM slot
//      that nothing else reads).
//
// =====================================================================
// CONVENTIONS
// =====================================================================
//
// RH850 is little-endian. A 32-bit instruction consisting of two
// 16-bit halfwords [hw1][hw2] appears in memory as the byte sequence
// [hw1.lo][hw1.hi][hw2.lo][hw2.hi]. The emitters here return raw
// 16-bit halfwords; the call site is responsible for writing them
// little-endian into the byte stream.
//
// Register conventions (per the RH850 ABI, public):
//   r0   — hard-wired zero
//   r1   — assembler scratch (reserved)
//   r2   — RTOS-reserved
//   r3   — sp (stack pointer)
//   r4   — gp (global pointer)
//   r5   — tp (text pointer)
//   r6..r29 — general-purpose
//   r30  — ep (element pointer)
//   r31  — lp (link register; holds return address)
//
// Our codegen uses r10..r19 for scratch — outside the ABI-reserved
// range, leaving plenty of headroom if a hook needs more registers
// later. r31 (lp) is the return-address register; `jmp [lp]` is the
// canonical return.
//
// =====================================================================
// INSTRUCTION FORMATS USED
// =====================================================================
//
// Format I (16-bit reg-reg):
//   Layout [15:11][10:5][4:0] = [reg2][opcode(6)][reg1]
//   Mnemonic shape: `op reg1, reg2` (reg2 is dest for arithmetic;
//   reg1 is the target for jmp).
//
// Format VI (32-bit imm + reg + reg):
//   First halfword:  [reg2(5)][opcode(6)][reg1(5)]
//   Second halfword: imm16 (raw bits)
//   Mnemonic shape: `op imm16, reg1, reg2` — reg2 = reg1 op imm16.
//
// Format VII (32-bit memory with disp16):
//   First halfword:  [reg2(5)][opcode(6)][reg1(5)]
//   Second halfword: disp16 — for word-sized ops the LSB of disp16
//                    is set to 1 to discriminate against half-word
//                    forms that share the opcode (RH850 G3 mem-access
//                    encoding convention).
//   Mnemonic shape: `st.w reg2, disp[reg1]` — mem[reg1+disp] = reg2.

#ifndef ST_FEATURE_CODEGEN_RH850_HPP
#define ST_FEATURE_CODEGEN_RH850_HPP

#include <cstdint>

namespace st::feature::codegen::rh850 {

// 32 general-purpose registers. Tagged-enum (vs raw uint8_t) so
// call sites can't confuse register indices with displacement values.
enum class Reg : std::uint8_t {
    R0 = 0,
    R1 = 1,
    R2 = 2,
    R3 = 3,
    R4 = 4,
    R5 = 5,
    R6 = 6,
    R7 = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
    R16 = 16,
    R17 = 17,
    R18 = 18,
    R19 = 19,
    R20 = 20,
    R21 = 21,
    R22 = 22,
    R23 = 23,
    R24 = 24,
    R25 = 25,
    R26 = 26,
    R27 = 27,
    R28 = 28,
    R29 = 29,
    R30 = 30,
    R31 = 31,
};

// Convenient aliases for ABI-named registers used by the codegen.
inline constexpr Reg kZero = Reg::R0;  // hard-wired zero
inline constexpr Reg kLp = Reg::R31;   // link register / return address

// --- Format I (16-bit reg-reg) ---------------------------------------

// MOV reg1, reg2 — reg2 = reg1 (full 32-bit copy). Format I, opcode
// 0b000000. NOP is the canonical reg1=r0, reg2=r0 form of this
// instruction → 0x0000.
[[nodiscard]] constexpr std::uint16_t enc_mov_reg(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b000000U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// NOP — encoded as `mov r0, r0` = 0x0000. Used for alignment and as
// the placeholder for branch delay slots when needed.
[[nodiscard]] constexpr std::uint16_t enc_nop() noexcept {
    return enc_mov_reg(Reg::R0, Reg::R0);
}

// JMP [reg1] — unconditional indirect jump to address held in reg1.
// Format I, opcode 0b000110, reg2 field = 0 (unused). `jmp [lp]` is
// the canonical "return from subroutine" sequence the codegen emits
// as the patch epilogue.
[[nodiscard]] constexpr std::uint16_t enc_jmp_reg(Reg reg1) noexcept {
    constexpr std::uint16_t kOpcode = 0b000110U;
    return static_cast<std::uint16_t>(
        (kOpcode << 5U) | static_cast<std::uint16_t>(reg1));
}

// ADD reg1, reg2 — reg2 = reg2 + reg1 (destructive two-operand form;
// `reg1` is the source, `reg2` is both source and destination). Format
// I, opcode 0b001110. CallPrimitive lowerings materialize both operands
// into scratch regs (r10 + r11), then ADD r11, r10 leaves the sum in
// r10 for the ST.W to the output slot.
[[nodiscard]] constexpr std::uint16_t enc_add_reg(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b001110U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// SUB reg1, reg2 — reg2 = reg2 - reg1 (destructive two-operand form).
// Format I, opcode 0b001101. Note operand order: in `subtract_int(a,
// b)` we want `a - b`, so the lowering loads `a` into reg2 and `b`
// into reg1, then SUB reg1, reg2 → reg2 = a - b.
[[nodiscard]] constexpr std::uint16_t enc_sub_reg(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b001101U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// AND reg1, reg2 — reg2 = reg2 & reg1 (bitwise AND, destructive).
// Format I, opcode 0b001010. Because the codegen normalizes Bool
// values to 0 or 1 in 32-bit slots (see coerce_constant_to_u32 +
// MOVT in SH-2A), bitwise AND on two normalized Bools is exactly
// logical AND. Used to lower `and_bool`.
[[nodiscard]] constexpr std::uint16_t enc_and_reg(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b001010U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// OR reg1, reg2 — reg2 = reg2 | reg1 (bitwise OR, destructive).
// Format I, opcode 0b001000. Same 0/1-normalization argument as AND:
// bitwise OR on normalized Bools = logical OR. Lowers `or_bool`.
[[nodiscard]] constexpr std::uint16_t enc_or_reg(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b001000U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// XOR reg1, reg2 — reg2 = reg2 ^ reg1 (bitwise XOR, destructive).
// Format I, opcode 0b001001. Used to lower `not_bool` as `x XOR 1`
// (correct for the 0/1-normalized representation; a bitwise NOT
// instruction would produce 0xFFFFFFFF/0xFFFFFFFE which is wrong for
// the bool invariant).
[[nodiscard]] constexpr std::uint16_t enc_xor_reg(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b001001U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// --- Format VI (32-bit imm + reg + reg) ------------------------------

// Each Format VI emitter returns the FIRST 16-bit halfword. The caller
// emits the second halfword (the raw imm16) separately. This split is
// deliberate: it keeps the halfword endian-emission in one place
// (the FragmentEmitter) rather than threading byte-order through each
// encoder.

// MOVHI imm16, reg1, reg2 — reg2 = reg1 + (imm16 << 16). The standard
// "load high half of a 32-bit constant" instruction. Combined with a
// subsequent MOVEA, materializes any 32-bit immediate. Format VI,
// opcode 0b110010.
[[nodiscard]] constexpr std::uint16_t enc_movhi_hw1(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b110010U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// MOVEA imm16, reg1, reg2 — reg2 = reg1 + sign_extend(imm16). When
// reg1 holds the high half of a 32-bit constant (from MOVHI), MOVEA
// adds the low half — with a +1 adjustment to the MOVHI immediate
// when bit 15 of the low half is set, to compensate for MOVEA's
// sign-extension. Format VI, opcode 0b110001.
[[nodiscard]] constexpr std::uint16_t enc_movea_hw1(Reg reg1, Reg reg2) noexcept {
    constexpr std::uint16_t kOpcode = 0b110001U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1));
}

// --- Format VII (32-bit memory access with disp16) -------------------

// ST.W reg2, disp[reg1] — mem[reg1 + sign_extend(disp16 & 0xFFFE)] = reg2.
// Format VII, opcode 0b111101. The LSB of the disp16 halfword is set
// to 1 to indicate word-sized access (per RH850 G3 mem-access
// disambiguation between half-word and word forms sharing the opcode).
// Caller's `disp` must be a multiple of 2; the LSB-as-size-bit
// convention means odd displacements aren't representable.
//
// Returns the first halfword; caller emits the second halfword as
// `(disp & 0xFFFE) | 0x0001`.
[[nodiscard]] constexpr std::uint16_t enc_st_w_hw1(Reg reg2_src, Reg reg1_base) noexcept {
    constexpr std::uint16_t kOpcode = 0b111101U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2_src) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1_base));
}

// Compose the second halfword for ST.W. Combines a 16-bit displacement
// (even values only — LSB is reused as the word-size discriminator bit)
// with the size bit. RH850 sign-extends disp16, so the addressable
// range is -32768..-2 and 0..32766. Caller is expected to range-check
// before calling; an odd disp value passed here is masked to even by
// the `& 0xFFFE` and the LSB is forced to 1 unconditionally.
[[nodiscard]] constexpr std::uint16_t enc_st_w_hw2(std::uint16_t disp) noexcept {
    return static_cast<std::uint16_t>((disp & 0xFFFEU) | 0x0001U);
}

// LD.W disp[reg1], reg2 — reg2 = mem[reg1 + sign_extend(disp16 & 0xFFFE)].
// Format VII, opcode 0b111100. Symmetric to ST.W. Used by
// emit_rh850_load_hook_store to read the LoadHookInput source value
// before storing it to the output RAM slot.
//
// Same disp-LSB convention as ST.W: LSB = 1 → word access.
[[nodiscard]] constexpr std::uint16_t enc_ld_w_hw1(Reg reg1_base, Reg reg2_dst) noexcept {
    constexpr std::uint16_t kOpcode = 0b111100U;
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(reg2_dst) << 11U) |
        (kOpcode << 5U) |
        static_cast<std::uint16_t>(reg1_base));
}

[[nodiscard]] constexpr std::uint16_t enc_ld_w_hw2(std::uint16_t disp) noexcept {
    return enc_st_w_hw2(disp); // same shape: low bit set for word access
}

// --- Helpers ---------------------------------------------------------

// Split a 32-bit constant into MOVHI/MOVEA immediate pair.
// Sign-extension compensation: if bit 15 of the low half is set,
// MOVEA's sign-extend would subtract 0x10000 from the assembled
// value, so we pre-add 1 to the high half to cancel that out.
//
//   imm = 0xABCD1234 → hi=0xABCD, lo=0x1234   (lo positive, no adj)
//   imm = 0xABCD8765 → hi=0xABCE, lo=0x8765   (lo negative, hi+1)
//
// Verify: 0xABCE0000 + sign_extend(0x8765)
//       = 0xABCE0000 + 0xFFFF8765
//       = 0xABCD8765  ✓
struct ImmSplit {
    std::uint16_t hi; // goes into MOVHI imm16
    std::uint16_t lo; // goes into MOVEA imm16
};

[[nodiscard]] constexpr ImmSplit split_imm32(std::uint32_t imm) noexcept {
    std::uint16_t const lo = static_cast<std::uint16_t>(imm & 0xFFFFU);
    std::uint16_t hi = static_cast<std::uint16_t>((imm >> 16U) & 0xFFFFU);
    if ((lo & 0x8000U) != 0U) {
        hi = static_cast<std::uint16_t>(hi + 1U);
    }
    return ImmSplit{hi, lo};
}

// Layout constants for all current RH850 emission shapes are exposed
// on the public API surface in `st/feature_codegen.hpp`:
//
//   kStoreSequenceSize / kJmpOffset           — LoadConstant→Store
//   kLoadStoreSequenceSize / kLoadStoreJmpOffset — LoadHookInput→Store
//   kBinaryIntArithSize{CC,CH,HC,HH}          — CallPrimitive add/sub
//
// Per-byte layout for LoadConstant→Store:
//
//   offset  bytes              instruction
//   0       [movhi hw1][hi]    MOVHI hi, r0, r10   ; r10 = hi << 16
//   4       [movea hw1][lo]    MOVEA lo, r10, r10  ; r10 = full 32-bit const
//   8       [movhi hw1][hi]    MOVHI hi, r0, r11   ; r11 = dst_hi << 16
//   12      [movea hw1][lo]    MOVEA lo, r11, r11  ; r11 = full dst address
//   16      [st.w hw1][disp]   ST.W r10, 0[r11]    ; mem[r11] = r10
//   20      [jmp hw1][nop]     JMP [lp] + NOP pad  ; return + align
//
// JMP is 16-bit (Format I); padding the patch to a 4-byte boundary
// with a trailing NOP keeps the splice address easy to reason about
// and aligns the next instruction following the patch (RH850 fetch
// units like 4-byte alignment for fetches that cross a half-line).

} // namespace st::feature::codegen::rh850

#endif // ST_FEATURE_CODEGEN_RH850_HPP
