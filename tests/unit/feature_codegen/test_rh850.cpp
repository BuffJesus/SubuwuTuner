// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unit tests for the RH850 codegen backend — the VB WRX counterpart
// to test_sh2a.cpp. Validates:
//
//   * Encoder primitives (Format I, VI, VII shapes)
//   * split_imm32 sign-extension compensation
//   * Rh850Backend::compile for the LoadConstant→StoreHookOutput
//     slice (the only slice this backend handles today)
//   * Same validation paths as SH-2A (missing hook, missing ecu_address,
//     missing free_ram, unsupported Op source)
//
// The encoder unit tests pin specific bit patterns we've committed to.
// The byte-level patch tests verify structural properties (length,
// position of the JMP epilogue) without re-asserting every opcode bit
// — that detail lives in the encoder tests.
//
// VERIFICATION CAVEAT (mirrored from src/feature_codegen/src/rh850.hpp):
// these tests confirm self-consistency between the encoder header and
// the backend's emission. They do NOT confirm the RH850 silicon
// interprets these bytes correctly. That requires HIL coverage against
// a real VB WRX ECU on the bench rig.

#include "st/defs.hpp"
#include "st/feature_codegen.hpp"
#include "st/feature_ir.hpp"

#include "rh850.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cg = st::feature::codegen;
namespace ir = st::feature::ir;

namespace {

// Reuses the same TOML pack shapes as test_sh2a.cpp — RH850 needs the
// same Definition fields (ecu_address, free_ram, writable_region).
constexpr std::string_view kPackOneHookToml = R"toml(
[pack]
schema_version = 1
id             = "test-pack-rh850"
endianness     = "little"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "after_fuel_calc"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
outputs = [
  { name = "commanded_pw_override", label = "Override fuel PW", type = "int", unit = "ms" },
]
)toml";

constexpr std::string_view kPackNoEcuAddrToml = R"toml(
[pack]
schema_version = 1
id             = "rh850-no-ecu-addr"
endianness     = "little"

[[hook]]
id        = "after_fuel_calc"
free_ram  = { base = 0x40000000, length = 256 }
outputs = [
  { name = "commanded_pw_override", type = "int" },
]
)toml";

constexpr std::string_view kPackNoFreeRamToml = R"toml(
[pack]
schema_version = 1
id             = "rh850-no-free-ram"
endianness     = "little"

[[hook]]
id              = "after_fuel_calc"
ecu_address     = 0x000ABCD0
outputs = [
  { name = "commanded_pw_override", type = "int" },
]
)toml";

// LoadConstant + StoreHookOutput shape — the only Module shape this
// backend handles today. Value is an int32 (matching coerce_constant_
// to_u32's Int-pin range check); callers wanting a specific 32-bit
// bit pattern pass the signed-equivalent (e.g. static_cast<int32_t>
// (0xDEADBEEF)) since the IR stores constants as double.
ir::Module make_const_store_module(std::int32_t value, std::string_view hook_id,
                                   std::string_view pin_name) {
    ir::Module m;

    ir::Instruction load{};
    load.op = ir::Op::LoadConstant;
    load.result_type = st::feature::PinType::Int;
    load.result_id = 1;
    load.constant_value = static_cast<double>(value);
    m.instructions.push_back(std::move(load));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Int;
    store.result_id = 0;
    store.symbol = std::string{hook_id};
    store.pin_name = std::string{pin_name};
    store.operands.push_back(1);
    m.instructions.push_back(std::move(store));

    return m;
}

st::Definition load_pack(std::string_view toml) {
    auto r = st::Definition::from_toml_string(toml);
    REQUIRE(r.has_value());
    return *std::move(r);
}

} // namespace

// ---- Encoder primitives ------------------------------------------------

TEST_CASE("rh850::enc_nop is 0x0000 (mov r0, r0)", "[rh850][encoder]") {
    REQUIRE(cg::rh850::enc_nop() == 0x0000);
}

TEST_CASE("rh850::enc_mov_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // mov r5, r10 → reg1 = 5, reg2 = 10, opcode = 0b000000
    // Bits: [10(5)][000000(6)][5(5)] = (10<<11) | 0 | 5 = 0x5005
    REQUIRE(cg::rh850::enc_mov_reg(cg::rh850::Reg::R5, cg::rh850::Reg::R10) == 0x5005);

    // mov r31, r0 → reg1 = 31, reg2 = 0
    // Bits: (0<<11) | 0 | 31 = 0x001F
    REQUIRE(cg::rh850::enc_mov_reg(cg::rh850::Reg::R31, cg::rh850::Reg::R0) == 0x001F);
}

TEST_CASE("rh850::enc_jmp_reg targets the chosen register",
          "[rh850][encoder]") {
    // jmp [lp] (r31): reg2 = 0, opcode = 0b000110, reg1 = 31
    // Bits: (0<<11) | (6<<5) | 31 = 0x00C0 | 0x001F = 0x00DF
    REQUIRE(cg::rh850::enc_jmp_reg(cg::rh850::Reg::R31) == 0x00DF);

    // jmp [r10]
    // Bits: (0<<11) | (6<<5) | 10 = 0x00C0 | 0x000A = 0x00CA
    REQUIRE(cg::rh850::enc_jmp_reg(cg::rh850::Reg::R10) == 0x00CA);
}

TEST_CASE("rh850::enc_movhi_hw1 places reg/opcode bits correctly",
          "[rh850][encoder]") {
    // movhi imm, r0, r10 → reg1 = 0, reg2 = 10, opcode = 0b110010 (=0x32)
    // Bits: (10<<11) | (0x32<<5) | 0 = 0x5000 | 0x640 = 0x5640
    REQUIRE(cg::rh850::enc_movhi_hw1(cg::rh850::Reg::R0, cg::rh850::Reg::R10) == 0x5640);
}

TEST_CASE("rh850::enc_movea_hw1 places reg/opcode bits correctly",
          "[rh850][encoder]") {
    // movea imm, r10, r10 → reg1 = 10, reg2 = 10, opcode = 0b110001 (=0x31)
    // Bits: (10<<11) | (0x31<<5) | 10 = 0x5000 | 0x620 | 10 = 0x562A
    REQUIRE(cg::rh850::enc_movea_hw1(cg::rh850::Reg::R10, cg::rh850::Reg::R10) == 0x562A);
}

TEST_CASE("rh850::enc_add_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // add r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b001110 (= 0x0E)
    // Bits: (10<<11) | (0x0E<<5) | 11 = 0x5000 | 0x1C0 | 11 = 0x51CB
    REQUIRE(cg::rh850::enc_add_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x51CB);

    // add r0, r0 (degenerate but legal)
    // Bits: 0 | (0x0E<<5) | 0 = 0x01C0
    REQUIRE(cg::rh850::enc_add_reg(cg::rh850::Reg::R0, cg::rh850::Reg::R0) == 0x01C0);
}

TEST_CASE("rh850::enc_sub_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // sub r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b001101 (= 0x0D)
    // Bits: (10<<11) | (0x0D<<5) | 11 = 0x5000 | 0x1A0 | 11 = 0x51AB
    REQUIRE(cg::rh850::enc_sub_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x51AB);
}

TEST_CASE("rh850::enc_and_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // and r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b001010 (= 0x0A)
    // Bits: (10<<11) | (0x0A<<5) | 11 = 0x5000 | 0x140 | 11 = 0x514B
    REQUIRE(cg::rh850::enc_and_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x514B);
}

TEST_CASE("rh850::enc_or_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // or r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b001000 (= 0x08)
    // Bits: (10<<11) | (0x08<<5) | 11 = 0x5000 | 0x100 | 11 = 0x510B
    REQUIRE(cg::rh850::enc_or_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x510B);
}

TEST_CASE("rh850::enc_xor_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // xor r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b001001 (= 0x09)
    // Bits: (10<<11) | (0x09<<5) | 11 = 0x5000 | 0x120 | 11 = 0x512B
    REQUIRE(cg::rh850::enc_xor_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x512B);
}

TEST_CASE("rh850::enc_not_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // not r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b000001 (= 0x01)
    // Bits: (10<<11) | (0x01<<5) | 11 = 0x5000 | 0x020 | 11 = 0x502B
    REQUIRE(cg::rh850::enc_not_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x502B);
}

TEST_CASE("rh850::enc_cmp_reg encodes the register fields correctly",
          "[rh850][encoder]") {
    // cmp r11, r10 → reg1 = 11, reg2 = 10, opcode = 0b001111 (= 0x0F)
    // Bits: (10<<11) | (0x0F<<5) | 11 = 0x5000 | 0x1E0 | 11 = 0x51EB
    REQUIRE(cg::rh850::enc_cmp_reg(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x51EB);
}

TEST_CASE("rh850::enc_setf_hw1 places reg/opcode/cond bits correctly",
          "[rh850][encoder]") {
    // SETF LT, r10 → reg2 = 10, opcode = 0b111111 (= 0x3F),
    // cond = LT (0b0110), encoded into bits[4:1] as 0b1100.
    // hw1 = (10<<11) | (0x3F<<5) | (0x6<<1) = 0x5000 | 0x7E0 | 0xC = 0x57EC
    REQUIRE(cg::rh850::enc_setf_hw1(cg::rh850::Cond::LT, cg::rh850::Reg::R10) == 0x57EC);

    // SETF GT, r10 → cond = 0b1111, encoded as 0b11110.
    REQUIRE(cg::rh850::enc_setf_hw1(cg::rh850::Cond::GT, cg::rh850::Reg::R10) == 0x57FE);

    // SETF Z (equal), r10 → cond = 0b0010, encoded as 0b00100.
    REQUIRE(cg::rh850::enc_setf_hw1(cg::rh850::Cond::Z, cg::rh850::Reg::R10) == 0x57E4);
}

TEST_CASE("rh850::enc_setf_hw2 is reserved-zero", "[rh850][encoder]") {
    REQUIRE(cg::rh850::enc_setf_hw2() == 0x0000);
}

TEST_CASE("rh850::enc_mul_hw1 places reg/opcode bits correctly",
          "[rh850][encoder]") {
    // mul r11, r10, ... → reg2 = 10, reg1 = 11, opcode = 0b111111 (= 0x3F)
    // hw1 bits: (10<<11) | (0x3F<<5) | 11 = 0x5000 | 0x7E0 | 11 = 0x57EB
    REQUIRE(cg::rh850::enc_mul_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10) == 0x57EB);
}

TEST_CASE("rh850::enc_mul_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder]") {
    // mul ..., ..., r13 → reg3 = 13, MUL subop = 0x0220.
    // hw2 bits: (13<<11) | 0x0220 = 0x6800 | 0x0220 = 0x6A20
    REQUIRE(cg::rh850::enc_mul_hw2(cg::rh850::Reg::R13) == 0x6A20);
}

TEST_CASE("rh850::enc_div_hw1 shares hw1 layout with MUL",
          "[rh850][encoder]") {
    // hw1 is the same Format-XI shape as MUL — only the hw2 subop differs.
    REQUIRE(cg::rh850::enc_div_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10) ==
            cg::rh850::enc_mul_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10));
}

TEST_CASE("rh850::enc_div_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder]") {
    // div ..., ..., r13 → reg3 = 13, DIV subop = 0x02C0.
    // hw2 bits: (13<<11) | 0x02C0 = 0x6800 | 0x02C0 = 0x6AC0
    REQUIRE(cg::rh850::enc_div_hw2(cg::rh850::Reg::R13) == 0x6AC0);
}

TEST_CASE("rh850::enc_addf_s_hw1 shares Format-XI hw1 shape with MUL",
          "[rh850][encoder][fpu]") {
    // Same hw1 shape: primary opcode 0b111111, reg fields where MUL puts them.
    REQUIRE(cg::rh850::enc_addf_s_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10) ==
            cg::rh850::enc_mul_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10));
}

TEST_CASE("rh850::enc_addf_s_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder][fpu]") {
    // addf.s ..., ..., r10 → reg3 = 10, ADDF.S subop = 0x0460.
    // hw2 bits: (10<<11) | 0x0460 = 0x5000 | 0x0460 = 0x5460
    REQUIRE(cg::rh850::enc_addf_s_hw2(cg::rh850::Reg::R10) == 0x5460);
}

TEST_CASE("rh850::enc_subf_s_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder][fpu]") {
    // subf.s ..., ..., r10 → reg3 = 10, SUBF.S subop = 0x0462.
    // hw2: (10<<11) | 0x0462 = 0x5462
    REQUIRE(cg::rh850::enc_subf_s_hw2(cg::rh850::Reg::R10) == 0x5462);
}

TEST_CASE("rh850::enc_mulf_s_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder][fpu]") {
    // mulf.s ..., ..., r10 → reg3 = 10, MULF.S subop = 0x0464.
    // hw2: (10<<11) | 0x0464 = 0x5464
    REQUIRE(cg::rh850::enc_mulf_s_hw2(cg::rh850::Reg::R10) == 0x5464);
}

TEST_CASE("rh850::enc_divf_s_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder][fpu]") {
    // divf.s ..., ..., r10 → reg3 = 10, DIVF.S subop = 0x046E.
    // hw2: (10<<11) | 0x046E = 0x546E
    REQUIRE(cg::rh850::enc_divf_s_hw2(cg::rh850::Reg::R10) == 0x546E);
}

TEST_CASE("rh850::enc_sqrtf_s_hw1 fixes reg1 to zero (Format F:II)",
          "[rh850][encoder][fpu]") {
    // sqrtf.s r10, ... → reg2 = 10, primary opcode 0b111111, reg1 forced 0.
    // hw1 bits: (10<<11) | (0x3F<<5) | 0 = 0x5000 | 0x07E0 = 0x57E0
    REQUIRE(cg::rh850::enc_sqrtf_s_hw1(cg::rh850::Reg::R10) == 0x57E0);
}

TEST_CASE("rh850::enc_sqrtf_s_hw2 places reg3 + subop bits correctly",
          "[rh850][encoder][fpu]") {
    // sqrtf.s ..., r10 → reg3 = 10, SQRTF.S subop = 0x044E.
    // hw2: (10<<11) | 0x044E = 0x544E
    REQUIRE(cg::rh850::enc_sqrtf_s_hw2(cg::rh850::Reg::R10) == 0x544E);
}

TEST_CASE("rh850::enc_cmpf_s_hw1 shares Format-F:I hw1 shape with ADDF.S",
          "[rh850][encoder][fpu]") {
    // CMPF.S reuses the standard reg1/reg2 slots — same primary opcode
    // pattern as ADDF.S; only the secondary opcode in hw2 differs.
    REQUIRE(cg::rh850::enc_cmpf_s_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10) ==
            cg::rh850::enc_addf_s_hw1(cg::rh850::Reg::R11, cg::rh850::Reg::R10));
}

TEST_CASE("rh850::enc_cmpf_s_hw2 places cccc + fff + base correctly",
          "[rh850][encoder][fpu]") {
    // cccc at bits[14:11], fff at bits[3:1], base 0x0420.
    // OLT (cccc=4), fff=0: (4<<11) | 0x0420 = 0x2420
    REQUIRE(cg::rh850::enc_cmpf_s_hw2(cg::rh850::FloatCond::OLT, 0) == 0x2420);
    // EQ (cccc=2), fff=0: (2<<11) | 0x0420 = 0x1420
    REQUIRE(cg::rh850::enc_cmpf_s_hw2(cg::rh850::FloatCond::EQ, 0) == 0x1420);
    // OLT (cccc=4), fff=3 (used in {3-bit slot}, shifted left 1): (4<<11) | (3<<1) | 0x0420 = 0x2426
    REQUIRE(cg::rh850::enc_cmpf_s_hw2(cg::rh850::FloatCond::OLT, 3) == 0x2426);
}

TEST_CASE("rh850::enc_trfsr_hw1 is the fixed 0x07E0 base", "[rh850][encoder][fpu]") {
    REQUIRE(cg::rh850::enc_trfsr_hw1() == 0x07E0);
}

TEST_CASE("rh850::enc_trfsr_hw2 places fff bits", "[rh850][encoder][fpu]") {
    // fff at bits[3:1], base 0x0400. fff=0 → 0x0400; fff=2 → 0x0404.
    REQUIRE(cg::rh850::enc_trfsr_hw2(0) == 0x0400);
    REQUIRE(cg::rh850::enc_trfsr_hw2(2) == 0x0404);
    REQUIRE(cg::rh850::enc_trfsr_hw2(7) == 0x040E);
}

TEST_CASE("rh850::enc_st_w_hw1 places reg/opcode bits correctly",
          "[rh850][encoder]") {
    // st.w r10, disp[r11] → reg2 = 10 (src), reg1 = 11 (base), opcode = 0b111101 (=0x3D)
    // Bits: (10<<11) | (0x3D<<5) | 11 = 0x5000 | 0x7A0 | 11 = 0x57AB
    REQUIRE(cg::rh850::enc_st_w_hw1(cg::rh850::Reg::R10, cg::rh850::Reg::R11) == 0x57AB);
}

TEST_CASE("rh850::enc_st_w_hw2 sets the word-size bit", "[rh850][encoder]") {
    // Zero displacement, word access → low bit set
    REQUIRE(cg::rh850::enc_st_w_hw2(0) == 0x0001);

    // Aligned displacement of 8, word access
    REQUIRE(cg::rh850::enc_st_w_hw2(8) == 0x0009);

    // Odd displacement gets masked to even before the size bit is OR'd
    REQUIRE(cg::rh850::enc_st_w_hw2(7) == 0x0007); // 6 | 1
}

TEST_CASE("rh850::split_imm32 does not adjust when low half is positive",
          "[rh850][encoder]") {
    // 0xABCD1234 → low = 0x1234 (bit 15 clear), hi unchanged
    auto const s = cg::rh850::split_imm32(0xABCD1234);
    REQUIRE(s.hi == 0xABCD);
    REQUIRE(s.lo == 0x1234);
}

TEST_CASE("rh850::split_imm32 adds 1 to hi when low half is negative",
          "[rh850][encoder]") {
    // 0xABCD8765 → low = 0x8765 (bit 15 set), hi pre-incremented
    auto const s = cg::rh850::split_imm32(0xABCD8765);
    REQUIRE(s.hi == 0xABCE);
    REQUIRE(s.lo == 0x8765);

    // Verify the round-trip: hi<<16 + sign_extend(lo) == original
    std::uint32_t const reassembled =
        (static_cast<std::uint32_t>(s.hi) << 16) +
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(s.lo)));
    REQUIRE(reassembled == 0xABCD8765);
}

TEST_CASE("rh850::split_imm32 handles the boundary 0x00008000",
          "[rh850][encoder]") {
    // 0x00008000 → low = 0x8000 (bit 15 set), hi = 0 → 1 after adj.
    // Reassembly: (1<<16) + sign_extend(0x8000) = 0x10000 + 0xFFFF8000 = 0x8000 ✓
    auto const s = cg::rh850::split_imm32(0x00008000);
    REQUIRE(s.hi == 0x0001);
    REQUIRE(s.lo == 0x8000);
}

// ---- Backend compile ---------------------------------------------------

TEST_CASE("Rh850Backend::compile emits 24 bytes for one LoadConstant→Store",
          "[rh850][compile]") {
    auto const def = load_pack(kPackOneHookToml);
    auto const m = make_const_store_module(0x12345678, "after_fuel_calc",
                                            "commanded_pw_override");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->arch == cg::Arch::Rh850);
    REQUIRE(r->hooks.size() == 1);

    auto const &h = r->hooks[0];
    REQUIRE(h.symbol == "after_fuel_calc");
    REQUIRE(h.splice_address == 0x000ABCD0);
    REQUIRE(h.code.size() == cg::rh850::kStoreSequenceSize);
    REQUIRE(h.ram_claims.size() == 1);
    REQUIRE(h.ram_claims[0].size == 4);
    REQUIRE(h.ram_claims[0].address == 0x40000000);
}

TEST_CASE("Rh850Backend::compile emits JMP [lp] at the documented offset",
          "[rh850][compile]") {
    auto const def = load_pack(kPackOneHookToml);
    auto const m = make_const_store_module(static_cast<std::int32_t>(0xDEADBEEF),
                                            "after_fuel_calc", "commanded_pw_override");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &code = r->hooks[0].code;
    REQUIRE(code.size() == cg::rh850::kStoreSequenceSize);

    // The JMP halfword sits at kJmpOffset, little-endian (low byte first).
    std::uint16_t const jmp_hw =
        static_cast<std::uint16_t>(code[cg::rh850::kJmpOffset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(code[cg::rh850::kJmpOffset + 1]) << 8);
    REQUIRE(jmp_hw == cg::rh850::enc_jmp_reg(cg::rh850::kLp));

    // Trailing pad NOP at offset+2.
    std::uint16_t const nop_hw =
        static_cast<std::uint16_t>(code[cg::rh850::kJmpOffset + 2]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(code[cg::rh850::kJmpOffset + 3]) << 8);
    REQUIRE(nop_hw == cg::rh850::enc_nop());
}

TEST_CASE("Rh850Backend::compile materializes the constant via MOVHI+MOVEA",
          "[rh850][compile]") {
    // Use a value with the low-half-negative case to exercise the
    // sign-extension compensation path.
    auto const def = load_pack(kPackOneHookToml);
    auto const m = make_const_store_module(static_cast<std::int32_t>(0xABCD8765),
                                           "after_fuel_calc", "commanded_pw_override");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    auto const &code = r->hooks[0].code;
    REQUIRE(code.size() >= 8);

    // Bytes 0-1: MOVHI hw1 (movhi imm, r0, r10) — little-endian.
    std::uint16_t const movhi_hw1 = static_cast<std::uint16_t>(code[0]) |
                                    static_cast<std::uint16_t>(
                                        static_cast<std::uint16_t>(code[1]) << 8);
    REQUIRE(movhi_hw1 == cg::rh850::enc_movhi_hw1(cg::rh850::kZero, cg::rh850::Reg::R10));

    // Bytes 2-3: MOVHI imm — expected to be hi adjusted (0xABCE)
    std::uint16_t const movhi_imm = static_cast<std::uint16_t>(code[2]) |
                                    static_cast<std::uint16_t>(
                                        static_cast<std::uint16_t>(code[3]) << 8);
    REQUIRE(movhi_imm == 0xABCE);

    // Bytes 4-5: MOVEA hw1 (movea imm, r10, r10)
    std::uint16_t const movea_hw1 = static_cast<std::uint16_t>(code[4]) |
                                    static_cast<std::uint16_t>(
                                        static_cast<std::uint16_t>(code[5]) << 8);
    REQUIRE(movea_hw1 == cg::rh850::enc_movea_hw1(cg::rh850::Reg::R10, cg::rh850::Reg::R10));

    // Bytes 6-7: MOVEA imm — expected to be the low half (0x8765)
    std::uint16_t const movea_imm = static_cast<std::uint16_t>(code[6]) |
                                    static_cast<std::uint16_t>(
                                        static_cast<std::uint16_t>(code[7]) << 8);
    REQUIRE(movea_imm == 0x8765);
}

TEST_CASE("Rh850Backend::compile shares the RAM slot for repeated writes to the same pin",
          "[rh850][compile]") {
    // Two LoadConstant → Store to the same pin should produce 48 bytes
    // (two sequences) and ONE RAM claim — the second store overwrites
    // the same slot at runtime.
    auto const def = load_pack(kPackOneHookToml);
    ir::Module m;

    // First load + store
    ir::Instruction lc1{};
    lc1.op = ir::Op::LoadConstant;
    lc1.result_type = st::feature::PinType::Int;
    lc1.result_id = 1;
    lc1.constant_value = 100.0;
    m.instructions.push_back(std::move(lc1));

    ir::Instruction st1{};
    st1.op = ir::Op::StoreHookOutput;
    st1.symbol = "after_fuel_calc";
    st1.pin_name = "commanded_pw_override";
    st1.operands.push_back(1);
    m.instructions.push_back(std::move(st1));

    // Second load + store, same pin
    ir::Instruction lc2{};
    lc2.op = ir::Op::LoadConstant;
    lc2.result_type = st::feature::PinType::Int;
    lc2.result_id = 2;
    lc2.constant_value = 200.0;
    m.instructions.push_back(std::move(lc2));

    ir::Instruction st2{};
    st2.op = ir::Op::StoreHookOutput;
    st2.symbol = "after_fuel_calc";
    st2.pin_name = "commanded_pw_override";
    st2.operands.push_back(2);
    m.instructions.push_back(std::move(st2));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    REQUIRE(r->hooks[0].code.size() == 2 * cg::rh850::kStoreSequenceSize);
    REQUIRE(r->hooks[0].ram_claims.size() == 1); // one slot shared
}

// ---- Validation paths --------------------------------------------------

TEST_CASE("Rh850Backend::compile refuses a Store to a hook missing ecu_address",
          "[rh850][compile][error]") {
    auto const def = load_pack(kPackNoEcuAddrToml);
    auto const m = make_const_store_module(42, "after_fuel_calc", "commanded_pw_override");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Rh850Backend::compile refuses a Store to a hook missing free_ram",
          "[rh850][compile][error]") {
    auto const def = load_pack(kPackNoFreeRamToml);
    auto const m = make_const_store_module(42, "after_fuel_calc", "commanded_pw_override");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Rh850Backend::compile refuses a Store to an undeclared hook",
          "[rh850][compile][error]") {
    auto const def = load_pack(kPackOneHookToml);
    auto const m = make_const_store_module(42, "nonexistent_hook", "commanded_pw_override");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Rh850Backend::compile rejects LoadHookInput with no declared input pin",
          "[rh850][compile][error]") {
    // kPackOneHookToml declares only outputs on its hook. LoadHookInput
    // for an undeclared input pin should fail with InvalidArgument from
    // resolve_hook_input_address — clear "pack didn't declare this pin"
    // rather than the old generic NotImplemented (now the slice is
    // implemented, see the success test below).
    auto const def = load_pack(kPackOneHookToml);
    ir::Module m;

    ir::Instruction lhi{};
    lhi.op = ir::Op::LoadHookInput;
    lhi.result_type = st::feature::PinType::Int;
    lhi.result_id = 1;
    lhi.symbol = "after_fuel_calc";
    lhi.pin_name = "rpm";
    m.instructions.push_back(std::move(lhi));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "after_fuel_calc";
    store.pin_name = "commanded_pw_override";
    store.operands.push_back(1);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Rh850Backend::compile emits LoadHookInput→StoreHookOutput slice",
          "[rh850][compile][load_hook_input]") {
    // A hook with one input (rpm at firmware address 0xFFFF8400) and one
    // output. Wire LoadHookInput → StoreHookOutput and verify the patch
    // is 28 bytes (kLoadStoreSequenceSize) with the expected JMP at
    // offset 24 (kLoadStoreJmpOffset).
    constexpr std::string_view kPackWithInputToml = R"toml(
[pack]
schema_version = 1
id             = "rh850-with-input"
endianness     = "little"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "load_hook_input_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "rpm", type = "int", address = 0xFFFF8400 },
]
outputs = [
  { name = "echoed_rpm", type = "int" },
]
)toml";

    auto const def = load_pack(kPackWithInputToml);
    ir::Module m;

    ir::Instruction lhi{};
    lhi.op = ir::Op::LoadHookInput;
    lhi.result_type = st::feature::PinType::Int;
    lhi.result_id = 1;
    lhi.symbol = "load_hook_input_test";
    lhi.pin_name = "rpm";
    m.instructions.push_back(std::move(lhi));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "load_hook_input_test";
    store.pin_name = "echoed_rpm";
    store.operands.push_back(1);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->arch == cg::Arch::Rh850);
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.symbol == "load_hook_input_test");
    REQUIRE(hp.code.size() == cg::rh850::kLoadStoreSequenceSize);

    // JMP [lp] at kLoadStoreJmpOffset, NOP pad at kLoadStoreJmpOffset+2.
    auto const jmp_lo = hp.code[cg::rh850::kLoadStoreJmpOffset];
    auto const jmp_hi = hp.code[cg::rh850::kLoadStoreJmpOffset + 1];
    auto const jmp_word = static_cast<std::uint16_t>(jmp_lo | (jmp_hi << 8));
    REQUIRE(jmp_word == cg::rh850::enc_jmp_reg(cg::rh850::kLp));

    auto const nop_lo = hp.code[cg::rh850::kLoadStoreJmpOffset + 2];
    auto const nop_hi = hp.code[cg::rh850::kLoadStoreJmpOffset + 3];
    auto const nop_word = static_cast<std::uint16_t>(nop_lo | (nop_hi << 8));
    REQUIRE(nop_word == cg::rh850::enc_nop());

    // Exactly one RAM claim for the output pin — 4 bytes, aligned to 4.
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(hp.ram_claims[0].size == 4);
    REQUIRE(hp.ram_claims[0].address % 4 == 0);
}

TEST_CASE("Rh850Backend::compile rejects a Store with no producing instruction",
          "[rh850][compile][error]") {
    auto const def = load_pack(kPackOneHookToml);
    ir::Module m;

    // StoreHookOutput referencing ValueId 99 that no instruction produces.
    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "after_fuel_calc";
    store.pin_name = "commanded_pw_override";
    store.operands.push_back(99);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

// ---- CallPrimitive (binary int arithmetic) ----------------------------

namespace {

// Pack with one hook that declares two int inputs (rpm, map) and one
// int output — enough for any add_int / subtract_int permutation that
// mixes Constant and HookInputPointer operands.
constexpr std::string_view kPackTwoInputsToml = R"toml(
[pack]
schema_version = 1
id             = "rh850-two-inputs"
endianness     = "little"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "binop_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "rpm", type = "int", address = 0xFFFF8400 },
  { name = "map", type = "int", address = 0xFFFF8410 },
]
outputs = [
  { name = "result", type = "int" },
]
)toml";

// Build a Module that compiles `result = symbol(a, b)` where each
// operand is either a LoadConstant (PinType::Int) or a LoadHookInput
// (pin name from the pack). The caller chooses which by passing nullopt
// (→ LoadConstant from `constant_value`) or a non-empty string
// (→ LoadHookInput from that pin name).
ir::Module make_binop_module(std::string_view symbol,
                             std::optional<std::int32_t> op1_const,
                             std::string_view op1_pin,
                             std::optional<std::int32_t> op2_const,
                             std::string_view op2_pin) {
    ir::Module m;

    ir::Instruction op1{};
    if (op1_const.has_value()) {
        op1.op = ir::Op::LoadConstant;
        op1.result_type = st::feature::PinType::Int;
        op1.result_id = 1;
        op1.constant_value = static_cast<double>(*op1_const);
    } else {
        op1.op = ir::Op::LoadHookInput;
        op1.result_type = st::feature::PinType::Int;
        op1.result_id = 1;
        op1.symbol = "binop_test";
        op1.pin_name = std::string{op1_pin};
    }
    m.instructions.push_back(std::move(op1));

    ir::Instruction op2{};
    if (op2_const.has_value()) {
        op2.op = ir::Op::LoadConstant;
        op2.result_type = st::feature::PinType::Int;
        op2.result_id = 2;
        op2.constant_value = static_cast<double>(*op2_const);
    } else {
        op2.op = ir::Op::LoadHookInput;
        op2.result_type = st::feature::PinType::Int;
        op2.result_id = 2;
        op2.symbol = "binop_test";
        op2.pin_name = std::string{op2_pin};
    }
    m.instructions.push_back(std::move(op2));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Int;
    call.result_id = 3;
    call.symbol = std::string{symbol};
    call.operands.push_back(1);
    call.operands.push_back(2);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Int;
    store.symbol = "binop_test";
    store.pin_name = "result";
    store.operands.push_back(3);
    m.instructions.push_back(std::move(store));

    return m;
}

} // namespace

TEST_CASE("Rh850Backend::compile emits add_int with two Constant operands",
          "[rh850][compile][call_primitive]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m = make_binop_module("add_int", 1234, "", 5678, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == cg::rh850::kBinaryIntArithSizeCC);
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(hp.ram_claims[0].size == 4);

    // Tail: JMP [lp] at size-4, NOP at size-2 (little-endian halfwords).
    auto const jmp_lo = hp.code[hp.code.size() - 4];
    auto const jmp_hi = hp.code[hp.code.size() - 3];
    auto const jmp_word = static_cast<std::uint16_t>(jmp_lo | (jmp_hi << 8));
    REQUIRE(jmp_word == cg::rh850::enc_jmp_reg(cg::rh850::kLp));
}

TEST_CASE("Rh850Backend::compile emits add_int with mixed Constant + HookInput",
          "[rh850][compile][call_primitive]") {
    auto const def = load_pack(kPackTwoInputsToml);

    // C + H — op1 constant, op2 hook input.
    auto const m_ch = make_binop_module("add_int", 100, "", std::nullopt, "rpm");
    cg::Rh850Backend backend;
    auto r_ch = backend.compile(m_ch, def);
    INFO("CH compile error: "
         << (r_ch.has_value() ? std::string{"(none)"} : r_ch.error().to_string()));
    REQUIRE(r_ch.has_value());
    REQUIRE(r_ch->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCH);

    // H + C — op1 hook input, op2 constant.
    auto const m_hc = make_binop_module("add_int", std::nullopt, "rpm", 50, "");
    auto r_hc = backend.compile(m_hc, def);
    REQUIRE(r_hc.has_value());
    REQUIRE(r_hc->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeHC);
}

TEST_CASE("Rh850Backend::compile emits add_int with two HookInput operands",
          "[rh850][compile][call_primitive]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m = make_binop_module("add_int", std::nullopt, "rpm", std::nullopt, "map");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeHH);

    // Patch must be 4-byte aligned regardless of operand kinds.
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits subtract_int (operand order: a - b)",
          "[rh850][compile][call_primitive]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m = make_binop_module("subtract_int", 200, "", 50, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);

    // Same size as add_int with the same operand kinds — the ADD/SUB
    // is a single 16-bit instruction in both cases. The opcode difference
    // is covered by the encoder unit tests above.
}

// (multiply_int / divide_int landed in the RH850 slice 2026-06-01 —
// coverage is the four compile-pass tests below.)

TEST_CASE("Rh850Backend::compile emits multiply_int with two Constant operands",
          "[rh850][compile][call_primitive][mul_div]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m = make_binop_module("multiply_int", 7, "", 6, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    // Same envelope as add_int — MUL is 4 bytes (no pad) where ADD is
    // 2 bytes + pad; net identical size across the four operand-kind
    // combinations.
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits multiply_int with mixed Constant + HookInput",
          "[rh850][compile][call_primitive][mul_div]") {
    auto const def = load_pack(kPackTwoInputsToml);

    auto const m_ch = make_binop_module("multiply_int", 3, "", std::nullopt, "rpm");
    cg::Rh850Backend backend;
    auto r_ch = backend.compile(m_ch, def);
    REQUIRE(r_ch.has_value());
    REQUIRE(r_ch->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCH);

    auto const m_hc = make_binop_module("multiply_int", std::nullopt, "rpm", 4, "");
    auto r_hc = backend.compile(m_hc, def);
    REQUIRE(r_hc.has_value());
    REQUIRE(r_hc->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeHC);
}

TEST_CASE("Rh850Backend::compile emits multiply_int with two HookInput operands",
          "[rh850][compile][call_primitive][mul_div]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m =
        make_binop_module("multiply_int", std::nullopt, "rpm", std::nullopt, "map");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeHH);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits divide_int (operand order: a / b)",
          "[rh850][compile][call_primitive][mul_div]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m = make_binop_module("divide_int", 100, "", 4, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);

    // Operand mapping matches subtract_int: load a → r10, b → r11.
    // DIV r11, r10, r13 then computes r10 = r10 / r11 = a / b.
}

// ---- Float arithmetic (Float in, Float out, FPU) -----------------------

namespace {

// Pack fixture for float-arith tests. Same shape as kPackTwoInputsToml
// but with Float inputs + output so add_float / subtract_float /
// multiply_float / divide_float are type-correct.
constexpr char const *kPackFloatInputsToml = R"toml(
[pack]
id = "float_test_pack"
display_name = "Float test pack"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "binop_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "mass_air_flow", type = "float", address = 0xFFFF8300 },
  { name = "intake_temp",   type = "float", address = 0xFFFF8310 },
]
outputs = [
  { name = "result", type = "float" },
]
)toml";

// Same shape as make_binop_module but Float-typed throughout. Constants
// pass as double; coerce_constant_to_u32's Float branch bit-casts to
// IEEE 754 single-precision.
ir::Module make_float_binop_module(std::string_view symbol,
                                   std::optional<double> op1_const,
                                   std::string_view op1_pin,
                                   std::optional<double> op2_const,
                                   std::string_view op2_pin) {
    ir::Module m;

    ir::Instruction op1{};
    if (op1_const.has_value()) {
        op1.op = ir::Op::LoadConstant;
        op1.result_type = st::feature::PinType::Float;
        op1.result_id = 1;
        op1.constant_value = *op1_const;
    } else {
        op1.op = ir::Op::LoadHookInput;
        op1.result_type = st::feature::PinType::Float;
        op1.result_id = 1;
        op1.symbol = "binop_test";
        op1.pin_name = std::string{op1_pin};
    }
    m.instructions.push_back(std::move(op1));

    ir::Instruction op2{};
    if (op2_const.has_value()) {
        op2.op = ir::Op::LoadConstant;
        op2.result_type = st::feature::PinType::Float;
        op2.result_id = 2;
        op2.constant_value = *op2_const;
    } else {
        op2.op = ir::Op::LoadHookInput;
        op2.result_type = st::feature::PinType::Float;
        op2.result_id = 2;
        op2.symbol = "binop_test";
        op2.pin_name = std::string{op2_pin};
    }
    m.instructions.push_back(std::move(op2));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Float;
    call.result_id = 3;
    call.symbol = std::string{symbol};
    call.operands.push_back(1);
    call.operands.push_back(2);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Float;
    store.symbol = "binop_test";
    store.pin_name = "result";
    store.operands.push_back(3);
    m.instructions.push_back(std::move(store));

    return m;
}

} // namespace

TEST_CASE("Rh850Backend::compile emits add_float with two Constant operands",
          "[rh850][compile][call_primitive][fpu]") {
    auto const def = load_pack(kPackFloatInputsToml);
    auto const m = make_float_binop_module("add_float", 1.5, "", 2.25, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    // FP arith fits the same envelope as int — Format F:I is 4 bytes
    // (no pad) where ADD is 2 + pad.
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits add_float with mixed Constant + HookInput",
          "[rh850][compile][call_primitive][fpu]") {
    auto const def = load_pack(kPackFloatInputsToml);

    auto const m_ch =
        make_float_binop_module("add_float", 0.5, "", std::nullopt, "mass_air_flow");
    cg::Rh850Backend backend;
    auto r_ch = backend.compile(m_ch, def);
    REQUIRE(r_ch.has_value());
    REQUIRE(r_ch->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCH);

    auto const m_hc =
        make_float_binop_module("add_float", std::nullopt, "mass_air_flow", 0.5, "");
    auto r_hc = backend.compile(m_hc, def);
    REQUIRE(r_hc.has_value());
    REQUIRE(r_hc->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeHC);
}

TEST_CASE("Rh850Backend::compile emits add_float with two HookInput operands",
          "[rh850][compile][call_primitive][fpu]") {
    auto const def = load_pack(kPackFloatInputsToml);
    auto const m = make_float_binop_module("add_float", std::nullopt, "mass_air_flow",
                                           std::nullopt, "intake_temp");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeHH);
}

TEST_CASE("Rh850Backend::compile emits subtract_float / multiply_float / divide_float "
          "(same shape)",
          "[rh850][compile][call_primitive][fpu]") {
    auto const def = load_pack(kPackFloatInputsToml);

    cg::Rh850Backend backend;
    for (auto const *symbol : {"subtract_float", "multiply_float", "divide_float"}) {
        INFO("symbol: " << symbol);
        auto const m = make_float_binop_module(symbol, 3.0, "", 2.0, "");
        auto r = backend.compile(m, def);
        REQUIRE(r.has_value());
        REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);
    }
}

// ---- Int compares (Int inputs, Bool output) ----------------------------

namespace {

// Pack fixture for int-compare tests. Same `rpm` / `map` Int inputs as
// kPackTwoInputsToml but the hook output is Bool so compare_*_int
// (Int × Int → Bool) is type-correct.
constexpr char const *kPackIntInBoolOutToml = R"toml(
[pack]
id = "compare_test_pack"
display_name = "Compare test pack"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "binop_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "rpm", type = "int", address = 0xFFFF8200 },
  { name = "map", type = "int", address = 0xFFFF8210 },
]
outputs = [
  { name = "result", type = "bool" },
]
)toml";

// Same as make_binop_module but the CallPrimitive + StoreHookOutput
// result type is Bool — for compare_*_int primitives that consume two
// Int operands and produce a Bool.
ir::Module make_int_compare_module(std::string_view symbol,
                                   std::optional<std::int32_t> op1_const,
                                   std::string_view op1_pin,
                                   std::optional<std::int32_t> op2_const,
                                   std::string_view op2_pin) {
    ir::Module m;

    ir::Instruction op1{};
    if (op1_const.has_value()) {
        op1.op = ir::Op::LoadConstant;
        op1.result_type = st::feature::PinType::Int;
        op1.result_id = 1;
        op1.constant_value = static_cast<double>(*op1_const);
    } else {
        op1.op = ir::Op::LoadHookInput;
        op1.result_type = st::feature::PinType::Int;
        op1.result_id = 1;
        op1.symbol = "binop_test";
        op1.pin_name = std::string{op1_pin};
    }
    m.instructions.push_back(std::move(op1));

    ir::Instruction op2{};
    if (op2_const.has_value()) {
        op2.op = ir::Op::LoadConstant;
        op2.result_type = st::feature::PinType::Int;
        op2.result_id = 2;
        op2.constant_value = static_cast<double>(*op2_const);
    } else {
        op2.op = ir::Op::LoadHookInput;
        op2.result_type = st::feature::PinType::Int;
        op2.result_id = 2;
        op2.symbol = "binop_test";
        op2.pin_name = std::string{op2_pin};
    }
    m.instructions.push_back(std::move(op2));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Bool; // Int × Int → Bool
    call.result_id = 3;
    call.symbol = std::string{symbol};
    call.operands.push_back(1);
    call.operands.push_back(2);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Bool;
    store.symbol = "binop_test";
    store.pin_name = "result";
    store.operands.push_back(3);
    m.instructions.push_back(std::move(store));

    return m;
}

} // namespace

TEST_CASE("Rh850Backend::compile emits compare_lt_int with two Constant operands",
          "[rh850][compile][call_primitive][int_compare]") {
    auto const def = load_pack(kPackIntInBoolOutToml);
    auto const m = make_int_compare_module("compare_lt_int", 100, "", 200, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == cg::rh850::kIntCompareSizeCC);
    REQUIRE(hp.code.size() % 4 == 0);
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(hp.ram_claims[0].size == 4);

    // Tail: JMP [lp] at size-4 (little-endian halfword).
    auto const jmp_lo = hp.code[hp.code.size() - 4];
    auto const jmp_hi = hp.code[hp.code.size() - 3];
    auto const jmp_word = static_cast<std::uint16_t>(jmp_lo | (jmp_hi << 8));
    REQUIRE(jmp_word == cg::rh850::enc_jmp_reg(cg::rh850::kLp));
}

TEST_CASE("Rh850Backend::compile emits compare_lt_int with mixed Constant + HookInput",
          "[rh850][compile][call_primitive][int_compare]") {
    auto const def = load_pack(kPackIntInBoolOutToml);

    auto const m_ch = make_int_compare_module("compare_lt_int", 100, "", std::nullopt, "rpm");
    cg::Rh850Backend backend;
    auto r_ch = backend.compile(m_ch, def);
    REQUIRE(r_ch.has_value());
    REQUIRE(r_ch->hooks[0].code.size() == cg::rh850::kIntCompareSizeCH);

    auto const m_hc = make_int_compare_module("compare_lt_int", std::nullopt, "rpm", 50, "");
    auto r_hc = backend.compile(m_hc, def);
    REQUIRE(r_hc.has_value());
    REQUIRE(r_hc->hooks[0].code.size() == cg::rh850::kIntCompareSizeHC);
}

TEST_CASE("Rh850Backend::compile emits compare_lt_int with two HookInput operands",
          "[rh850][compile][call_primitive][int_compare]") {
    auto const def = load_pack(kPackIntInBoolOutToml);
    auto const m = make_int_compare_module("compare_lt_int", std::nullopt, "rpm",
                                           std::nullopt, "map");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kIntCompareSizeHH);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits compare_gt_int / compare_eq_int (same shape)",
          "[rh850][compile][call_primitive][int_compare]") {
    auto const def = load_pack(kPackIntInBoolOutToml);

    auto const m_gt = make_int_compare_module("compare_gt_int", 5, "", 3, "");
    auto const m_eq = make_int_compare_module("compare_eq_int", 7, "", 7, "");

    cg::Rh850Backend backend;
    auto r_gt = backend.compile(m_gt, def);
    REQUIRE(r_gt.has_value());
    REQUIRE(r_gt->hooks[0].code.size() == cg::rh850::kIntCompareSizeCC);

    auto r_eq = backend.compile(m_eq, def);
    REQUIRE(r_eq.has_value());
    REQUIRE(r_eq->hooks[0].code.size() == cg::rh850::kIntCompareSizeCC);

    // The three compares share the byte-level fragment shape — only the
    // SETF condition code differs. Per-condition encoding is verified
    // in the encoder unit tests above.
}

// ---- Bool primitives ---------------------------------------------------

namespace {

// Same shape as kPackTwoInputsToml but with Bool inputs for the
// boolean primitive cases.
constexpr std::string_view kPackBoolInputsToml = R"toml(
[pack]
schema_version = 1
id             = "rh850-bool-inputs"
endianness     = "little"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "bool_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "flag_a", type = "bool", address = 0xFFFF8500 },
  { name = "flag_b", type = "bool", address = 0xFFFF8510 },
]
outputs = [
  { name = "result", type = "bool" },
]
)toml";

ir::Module make_bool_binop_module(std::string_view symbol, bool op1_const, bool op2_const) {
    ir::Module m;

    ir::Instruction op1{};
    op1.op = ir::Op::LoadConstant;
    op1.result_type = st::feature::PinType::Bool;
    op1.result_id = 1;
    op1.constant_value = op1_const ? 1.0 : 0.0;
    m.instructions.push_back(std::move(op1));

    ir::Instruction op2{};
    op2.op = ir::Op::LoadConstant;
    op2.result_type = st::feature::PinType::Bool;
    op2.result_id = 2;
    op2.constant_value = op2_const ? 1.0 : 0.0;
    m.instructions.push_back(std::move(op2));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Bool;
    call.result_id = 3;
    call.symbol = std::string{symbol};
    call.operands.push_back(1);
    call.operands.push_back(2);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Bool;
    store.symbol = "bool_test";
    store.pin_name = "result";
    store.operands.push_back(3);
    m.instructions.push_back(std::move(store));

    return m;
}

} // namespace

TEST_CASE("Rh850Backend::compile emits and_bool (binary bool)",
          "[rh850][compile][call_primitive][bool]") {
    auto const def = load_pack(kPackBoolInputsToml);
    auto const m = make_bool_binop_module("and_bool", true, false);

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("and_bool compile error: "
         << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    // Same shape as binary int arithmetic — 36 bytes for two constants.
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);
}

TEST_CASE("Rh850Backend::compile emits or_bool (binary bool)",
          "[rh850][compile][call_primitive][bool]") {
    auto const def = load_pack(kPackBoolInputsToml);
    auto const m = make_bool_binop_module("or_bool", true, false);

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);
}

TEST_CASE("Rh850Backend::compile emits not_bool (unary bool, XOR-with-1)",
          "[rh850][compile][call_primitive][bool]") {
    auto const def = load_pack(kPackBoolInputsToml);

    // Module: not_bool(LoadConstant(true)) → Store
    ir::Module m;

    ir::Instruction op{};
    op.op = ir::Op::LoadConstant;
    op.result_type = st::feature::PinType::Bool;
    op.result_id = 1;
    op.constant_value = 1.0;
    m.instructions.push_back(std::move(op));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Bool;
    call.result_id = 2;
    call.symbol = "not_bool";
    call.operands.push_back(1);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "bool_test";
    store.pin_name = "result";
    store.operands.push_back(2);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("not_bool compile error: "
         << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    // Same 36-byte shape as binary CC (one operand load + one constant
    // load + XOR + dst addr materialize + ST.W + JMP).
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kBinaryIntArithSizeCC);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

// (compare_lt_int landed in the RH850 slice 2026-06-01 — coverage is
// the four compile-pass tests in the int_compare section above.)

// ---- select_* primitives (branchless mask-merge) -----------------------

TEST_CASE("Rh850Backend::compile emits select_int (3-operand)",
          "[rh850][compile][call_primitive][select]") {
    // select_int(cond, true_val, false_val) → Int
    auto const def = load_pack(kPackBoolInputsToml);
    ir::Module m;

    ir::Instruction cond{};
    cond.op = ir::Op::LoadConstant;
    cond.result_type = st::feature::PinType::Bool;
    cond.result_id = 1;
    cond.constant_value = 1.0; // pick true_val
    m.instructions.push_back(std::move(cond));

    ir::Instruction tv{};
    tv.op = ir::Op::LoadConstant;
    tv.result_type = st::feature::PinType::Int;
    tv.result_id = 2;
    tv.constant_value = 100.0;
    m.instructions.push_back(std::move(tv));

    ir::Instruction fv{};
    fv.op = ir::Op::LoadConstant;
    fv.result_type = st::feature::PinType::Int;
    fv.result_id = 3;
    fv.constant_value = 200.0;
    m.instructions.push_back(std::move(fv));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Int;
    call.result_id = 4;
    call.symbol = "select_int";
    call.operands.push_back(1);
    call.operands.push_back(2);
    call.operands.push_back(3);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "bool_test";
    store.pin_name = "result";
    store.operands.push_back(4);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("select_int compile error: "
         << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    // Patch must be 4-byte aligned; spot-check the JMP at the end.
    auto const &code = r->hooks[0].code;
    REQUIRE(code.size() % 4 == 0);
    auto const jmp_lo = code[code.size() - 4];
    auto const jmp_hi = code[code.size() - 3];
    auto const jmp_word = static_cast<std::uint16_t>(jmp_lo | (jmp_hi << 8));
    REQUIRE(jmp_word == cg::rh850::enc_jmp_reg(cg::rh850::kLp));

    // Verify the patch includes a NOT instruction (Format I opcode 0x01)
    // — distinctive of the select pattern; not present in any of the
    // simpler primitives.
    bool seen_not = false;
    for (std::size_t i = 0; i + 1 < code.size(); i += 2) {
        std::uint16_t const halfword =
            static_cast<std::uint16_t>(code[i]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(code[i + 1]) << 8);
        // Any reg1, reg2 combination of NOT — match the opcode field
        // in bits 5..10 only.
        if (((halfword >> 5) & 0x3F) == 0x01) {
            seen_not = true;
            break;
        }
    }
    REQUIRE(seen_not);
}

TEST_CASE("Rh850Backend::compile emits select_bool (3-operand bool)",
          "[rh850][compile][call_primitive][select]") {
    auto const def = load_pack(kPackBoolInputsToml);
    ir::Module m;

    auto const mk_bool = [](ir::ValueId id, bool v) {
        ir::Instruction ins{};
        ins.op = ir::Op::LoadConstant;
        ins.result_type = st::feature::PinType::Bool;
        ins.result_id = id;
        ins.constant_value = v ? 1.0 : 0.0;
        return ins;
    };
    m.instructions.push_back(mk_bool(1, true));
    m.instructions.push_back(mk_bool(2, true));
    m.instructions.push_back(mk_bool(3, false));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Bool;
    call.result_id = 4;
    call.symbol = "select_bool";
    call.operands = {1, 2, 3};
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "bool_test";
    store.pin_name = "result";
    store.operands.push_back(4);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits select_float (3-operand float)",
          "[rh850][compile][call_primitive][select]") {
    // select_float shares the same byte-level fragment shape as
    // select_int — both manipulate 32-bit words via mask/AND/OR. The
    // codegen doesn't reinterpret the bytes; the firmware does.
    auto const def = load_pack(kPackBoolInputsToml);
    ir::Module m;

    ir::Instruction cond{};
    cond.op = ir::Op::LoadConstant;
    cond.result_type = st::feature::PinType::Bool;
    cond.result_id = 1;
    cond.constant_value = 0.0;
    m.instructions.push_back(std::move(cond));

    auto const mk_float = [](ir::ValueId id, double v) {
        ir::Instruction ins{};
        ins.op = ir::Op::LoadConstant;
        ins.result_type = st::feature::PinType::Float;
        ins.result_id = id;
        ins.constant_value = v;
        return ins;
    };
    m.instructions.push_back(mk_float(2, 1.5));
    m.instructions.push_back(mk_float(3, 2.5));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Float;
    call.result_id = 4;
    call.symbol = "select_float";
    call.operands = {1, 2, 3};
    m.instructions.push_back(std::move(call));

    // The bool_test hook's `result` is Bool, but we just need any
    // output pin with a free RAM slot — wire to commanded_pw_override
    // via a custom pack that has a Float-typed output.
    constexpr std::string_view kFloatOutPack = R"toml(
[pack]
schema_version = 1
id             = "rh850-float-out"
endianness     = "little"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "f_out"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
outputs = [
  { name = "out", type = "float" },
]
)toml";

    auto const f_def = load_pack(kFloatOutPack);
    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "f_out";
    store.pin_name = "out";
    store.operands.push_back(4);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, f_def);
    INFO("select_float error: " << (r.has_value() ? "(none)" : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile rejects unknown CallPrimitive symbol",
          "[rh850][compile][call_primitive][error]") {
    auto const def = load_pack(kPackTwoInputsToml);
    auto const m = make_binop_module("teleport_fuel", 1, "", 2, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}

TEST_CASE("Rh850Backend::compile rejects type-mismatched primitive operand",
          "[rh850][compile][call_primitive][error]") {
    // add_int expects Int operands; feed it a Bool LoadConstant and
    // confirm the type-check refuses with ParseError.
    auto const def = load_pack(kPackTwoInputsToml);
    ir::Module m;

    ir::Instruction op1{};
    op1.op = ir::Op::LoadConstant;
    op1.result_type = st::feature::PinType::Bool;
    op1.result_id = 1;
    op1.constant_value = 1.0;
    m.instructions.push_back(std::move(op1));

    ir::Instruction op2{};
    op2.op = ir::Op::LoadConstant;
    op2.result_type = st::feature::PinType::Int;
    op2.result_id = 2;
    op2.constant_value = 5.0;
    m.instructions.push_back(std::move(op2));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Int;
    call.result_id = 3;
    call.symbol = "add_int";
    call.operands.push_back(1);
    call.operands.push_back(2);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "binop_test";
    store.pin_name = "result";
    store.operands.push_back(3);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Rh850Backend::compile emits nested CallPrimitive — (a + b) + c",
          "[rh850][compile][call_primitive][nested]") {
    // (a + b) + c. The inner add_int's result feeds the outer add_int's
    // first operand. The nested driver allocates one RAM slot for the
    // inner result, emits the inner fragment storing into that slot
    // (no JMP tail), then the outer fragment reads the slot as a
    // HookInputPointer-equivalent operand and stores to the output
    // pin's slot with JMP+NOP terminator.
    auto const def = load_pack(kPackTwoInputsToml);
    ir::Module m;

    auto const make_const = [](ir::ValueId id, std::int32_t v) {
        ir::Instruction ins{};
        ins.op = ir::Op::LoadConstant;
        ins.result_type = st::feature::PinType::Int;
        ins.result_id = id;
        ins.constant_value = static_cast<double>(v);
        return ins;
    };
    m.instructions.push_back(make_const(1, 1));
    m.instructions.push_back(make_const(2, 2));
    m.instructions.push_back(make_const(3, 3));

    ir::Instruction inner{};
    inner.op = ir::Op::CallPrimitive;
    inner.result_type = st::feature::PinType::Int;
    inner.result_id = 4;
    inner.symbol = "add_int";
    inner.operands.push_back(1);
    inner.operands.push_back(2);
    m.instructions.push_back(std::move(inner));

    ir::Instruction outer{};
    outer.op = ir::Op::CallPrimitive;
    outer.result_type = st::feature::PinType::Int;
    outer.result_id = 5;
    outer.symbol = "add_int";
    outer.operands.push_back(4); // nested
    outer.operands.push_back(3);
    m.instructions.push_back(std::move(outer));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.symbol = "binop_test";
    store.pin_name = "result";
    store.operands.push_back(5);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    // Two fragments concatenated:
    //   inner add_int(1, 2) → slot:    kBinaryIntArithSizeCC - 4 (no tail)
    //   outer add_int(slot, 3) → dst:  kBinaryIntArithSizeHC (HookInput-shaped op1)
    REQUIRE(hp.code.size() == (cg::rh850::kBinaryIntArithSizeCC - 4) +
                                  cg::rh850::kBinaryIntArithSizeHC);
    REQUIRE(hp.code.size() % 4 == 0);

    // Two RAM claims: one for the output pin (4 bytes), one for the
    // inner add_int's intermediate result (4 bytes). Order is
    // pin-slot-first because the Store-walk allocates the output pin
    // before recursing into the source primitive.
    REQUIRE(hp.ram_claims.size() == 2);
    REQUIRE(hp.ram_claims[0].size == 4);
    REQUIRE(hp.ram_claims[1].size == 4);
    REQUIRE(hp.ram_claims[0].address != hp.ram_claims[1].address);

    // Tail: JMP[lp] in the last 4 bytes (root fragment terminator).
    auto const jmp_lo = hp.code[hp.code.size() - 4];
    auto const jmp_hi = hp.code[hp.code.size() - 3];
    auto const jmp_word = static_cast<std::uint16_t>(jmp_lo | (jmp_hi << 8));
    REQUIRE(jmp_word == cg::rh850::enc_jmp_reg(cg::rh850::kLp));
}

TEST_CASE("Rh850Backend::compile emits nested CallPrimitive across mixed primitives",
          "[rh850][compile][call_primitive][nested]") {
    // select_int(compare_lt_int(a, b), a, b) — the classic min(a, b).
    // Exercises:
    //   - nested primitive of a different result-type (Bool from
    //     compare_lt_int feeding the Bool cond input of select_int)
    //   - fan-out: `a` is read twice (by compare_lt and by select), but
    //     they're both LoadHookInput — no slot needed, just two
    //     identical resolves
    //   - the nested driver's symbol-agnostic dispatch through
    //     emit_rh850_primitive_fragment
    auto const def = load_pack(kPackTwoInputsToml);
    ir::Module m;

    auto const make_hook_input = [](ir::ValueId id, std::string_view pin) {
        ir::Instruction ins{};
        ins.op = ir::Op::LoadHookInput;
        ins.result_type = st::feature::PinType::Int;
        ins.result_id = id;
        ins.symbol = "binop_test";
        ins.pin_name = std::string{pin};
        return ins;
    };
    m.instructions.push_back(make_hook_input(1, "rpm"));
    m.instructions.push_back(make_hook_input(2, "map"));

    // cmp = compare_lt_int(rpm, map)  — Bool
    ir::Instruction cmp{};
    cmp.op = ir::Op::CallPrimitive;
    cmp.result_type = st::feature::PinType::Bool;
    cmp.result_id = 3;
    cmp.symbol = "compare_lt_int";
    cmp.operands.push_back(1);
    cmp.operands.push_back(2);
    m.instructions.push_back(std::move(cmp));

    // min = select_int(cmp, rpm, map)  — Int
    ir::Instruction sel{};
    sel.op = ir::Op::CallPrimitive;
    sel.result_type = st::feature::PinType::Int;
    sel.result_id = 4;
    sel.symbol = "select_int";
    sel.operands.push_back(3); // nested compare_lt_int
    sel.operands.push_back(1);
    sel.operands.push_back(2);
    m.instructions.push_back(std::move(sel));

    // Need an Int output pin for the result of select_int; the
    // kPackTwoInputsToml pack's `result` output is Int.
    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Int;
    store.symbol = "binop_test";
    store.pin_name = "result";
    store.operands.push_back(4);
    m.instructions.push_back(std::move(store));

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() % 4 == 0);

    // Two RAM claims: output pin slot + compare_lt_int intermediate slot.
    REQUIRE(hp.ram_claims.size() == 2);
    REQUIRE(hp.ram_claims[0].size == 4);
    REQUIRE(hp.ram_claims[1].size == 4);
}

// ---- Unary float primitives (sqrt_float, flex_fuel_scale) -------------

namespace {

// Pack fixture for unary-Float primitives. Single Float input, single
// Float output. Same shape as kPackFloatInputsToml but one input
// suffices.
constexpr char const *kPackFloatUnaryToml = R"toml(
[pack]
id = "float_unary_pack"
display_name = "Float unary test pack"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "unary_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "ethanol_pct", type = "float", address = 0xFFFF8400 },
]
outputs = [
  { name = "result", type = "float" },
]
)toml";

ir::Module make_float_unary_module(std::string_view symbol,
                                   std::optional<double> op_const,
                                   std::string_view op_pin) {
    ir::Module m;

    ir::Instruction op{};
    if (op_const.has_value()) {
        op.op = ir::Op::LoadConstant;
        op.result_type = st::feature::PinType::Float;
        op.result_id = 1;
        op.constant_value = *op_const;
    } else {
        op.op = ir::Op::LoadHookInput;
        op.result_type = st::feature::PinType::Float;
        op.result_id = 1;
        op.symbol = "unary_test";
        op.pin_name = std::string{op_pin};
    }
    m.instructions.push_back(std::move(op));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Float;
    call.result_id = 2;
    call.symbol = std::string{symbol};
    call.operands.push_back(1);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Float;
    store.symbol = "unary_test";
    store.pin_name = "result";
    store.operands.push_back(2);
    m.instructions.push_back(std::move(store));

    return m;
}

} // namespace

TEST_CASE("Rh850Backend::compile emits sqrt_float with a Constant operand",
          "[rh850][compile][call_primitive][fpu]") {
    auto const def = load_pack(kPackFloatUnaryToml);
    auto const m = make_float_unary_module("sqrt_float", 4.0, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kUnaryFloatSizeC);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits sqrt_float with a HookInput operand",
          "[rh850][compile][call_primitive][fpu]") {
    auto const def = load_pack(kPackFloatUnaryToml);
    auto const m = make_float_unary_module("sqrt_float", std::nullopt, "ethanol_pct");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kUnaryFloatSizeH);
}

TEST_CASE("Rh850Backend::compile emits flex_fuel_scale with a Constant operand",
          "[rh850][compile][call_primitive][fpu][flex_fuel]") {
    auto const def = load_pack(kPackFloatUnaryToml);
    // E15 → expected ≈ 1.049, but we're only verifying the byte envelope.
    auto const m = make_float_unary_module("flex_fuel_scale", 15.0, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kFlexFuelScaleSizeC);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits flex_fuel_scale with a HookInput operand",
          "[rh850][compile][call_primitive][fpu][flex_fuel]") {
    auto const def = load_pack(kPackFloatUnaryToml);
    auto const m = make_float_unary_module("flex_fuel_scale", std::nullopt, "ethanol_pct");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kFlexFuelScaleSizeH);
}

// ---- Float compares (Float inputs, Bool output) -----------------------

namespace {

// Pack fixture for float-compare tests. Two Float inputs, single Bool
// output — the compare_*_float result-type shape.
constexpr char const *kPackFloatInBoolOutToml = R"toml(
[pack]
id = "float_compare_pack"
display_name = "Float compare test pack"

[[writable_region]]
name    = "test-cal"
kind    = "calibration"
address = 0x000A0000
length  = 0x00010000

[[hook]]
id              = "fcmp_test"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "afr",        type = "float", address = 0xFFFF8500 },
  { name = "target_afr", type = "float", address = 0xFFFF8510 },
]
outputs = [
  { name = "result", type = "bool" },
]
)toml";

ir::Module make_float_compare_module(std::string_view symbol,
                                     std::optional<double> op1_const,
                                     std::string_view op1_pin,
                                     std::optional<double> op2_const,
                                     std::string_view op2_pin) {
    ir::Module m;

    ir::Instruction op1{};
    if (op1_const.has_value()) {
        op1.op = ir::Op::LoadConstant;
        op1.result_type = st::feature::PinType::Float;
        op1.result_id = 1;
        op1.constant_value = *op1_const;
    } else {
        op1.op = ir::Op::LoadHookInput;
        op1.result_type = st::feature::PinType::Float;
        op1.result_id = 1;
        op1.symbol = "fcmp_test";
        op1.pin_name = std::string{op1_pin};
    }
    m.instructions.push_back(std::move(op1));

    ir::Instruction op2{};
    if (op2_const.has_value()) {
        op2.op = ir::Op::LoadConstant;
        op2.result_type = st::feature::PinType::Float;
        op2.result_id = 2;
        op2.constant_value = *op2_const;
    } else {
        op2.op = ir::Op::LoadHookInput;
        op2.result_type = st::feature::PinType::Float;
        op2.result_id = 2;
        op2.symbol = "fcmp_test";
        op2.pin_name = std::string{op2_pin};
    }
    m.instructions.push_back(std::move(op2));

    ir::Instruction call{};
    call.op = ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Bool; // Float × Float → Bool
    call.result_id = 3;
    call.symbol = std::string{symbol};
    call.operands.push_back(1);
    call.operands.push_back(2);
    m.instructions.push_back(std::move(call));

    ir::Instruction store{};
    store.op = ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Bool;
    store.symbol = "fcmp_test";
    store.pin_name = "result";
    store.operands.push_back(3);
    m.instructions.push_back(std::move(store));

    return m;
}

} // namespace

TEST_CASE("Rh850Backend::compile emits compare_lt_float with two Constant operands",
          "[rh850][compile][call_primitive][fpu][float_compare]") {
    auto const def = load_pack(kPackFloatInBoolOutToml);
    auto const m = make_float_compare_module("compare_lt_float", 1.5, "", 2.0, "");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    INFO("compile error: " << (r.has_value() ? std::string{"(none)"} : r.error().to_string()));
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kFloatCompareSizeCC);
    REQUIRE(r->hooks[0].code.size() % 4 == 0);
}

TEST_CASE("Rh850Backend::compile emits compare_lt_float with mixed Constant + HookInput",
          "[rh850][compile][call_primitive][fpu][float_compare]") {
    auto const def = load_pack(kPackFloatInBoolOutToml);

    auto const m_ch = make_float_compare_module("compare_lt_float", 1.0, "", std::nullopt, "afr");
    cg::Rh850Backend backend;
    auto r_ch = backend.compile(m_ch, def);
    REQUIRE(r_ch.has_value());
    REQUIRE(r_ch->hooks[0].code.size() == cg::rh850::kFloatCompareSizeCH);

    auto const m_hc = make_float_compare_module("compare_lt_float", std::nullopt, "afr", 1.0, "");
    auto r_hc = backend.compile(m_hc, def);
    REQUIRE(r_hc.has_value());
    REQUIRE(r_hc->hooks[0].code.size() == cg::rh850::kFloatCompareSizeHC);
}

TEST_CASE("Rh850Backend::compile emits compare_lt_float with two HookInput operands",
          "[rh850][compile][call_primitive][fpu][float_compare]") {
    auto const def = load_pack(kPackFloatInBoolOutToml);
    auto const m = make_float_compare_module("compare_lt_float", std::nullopt, "afr",
                                             std::nullopt, "target_afr");

    cg::Rh850Backend backend;
    auto r = backend.compile(m, def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks[0].code.size() == cg::rh850::kFloatCompareSizeHH);
}

TEST_CASE("Rh850Backend::compile emits compare_gt_float / compare_eq_float (same shape)",
          "[rh850][compile][call_primitive][fpu][float_compare]") {
    auto const def = load_pack(kPackFloatInBoolOutToml);

    cg::Rh850Backend backend;
    for (auto const *symbol : {"compare_gt_float", "compare_eq_float"}) {
        INFO("symbol: " << symbol);
        auto const m = make_float_compare_module(symbol, 1.5, "", 2.0, "");
        auto r = backend.compile(m, def);
        REQUIRE(r.has_value());
        REQUIRE(r->hooks[0].code.size() == cg::rh850::kFloatCompareSizeCC);
    }
}

// ---- select_backend -----------------------------------------------------

TEST_CASE("select_backend(\"VB\") returns an Rh850 backend",
          "[rh850][select]") {
    auto b = cg::select_backend("VB");
    REQUIRE(b.has_value());
    REQUIRE((*b)->arch() == cg::Arch::Rh850);
}
