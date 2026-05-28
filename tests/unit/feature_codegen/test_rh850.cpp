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

#include "../../../src/feature_codegen/src/rh850.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("Rh850Backend::compile returns NotImplemented for LoadHookInput sources",
          "[rh850][compile][error]") {
    // RH850's slice is LoadConstant only; LoadHookInput is a follow-up
    // bundle. Verify the error message is specific so users get a
    // clear "this isn't ready yet" rather than a confusing parse error.
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
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
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

// ---- select_backend -----------------------------------------------------

TEST_CASE("select_backend(\"VB\") returns an Rh850 backend",
          "[rh850][select]") {
    auto b = cg::select_backend("VB");
    REQUIRE(b.has_value());
    REQUIRE((*b)->arch() == cg::Arch::Rh850);
}
