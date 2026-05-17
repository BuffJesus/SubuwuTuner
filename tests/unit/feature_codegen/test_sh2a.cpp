// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <catch2/catch_test_macros.hpp>

#include "st/defs.hpp"
#include "st/feature_codegen.hpp"
#include "st/feature_ir.hpp"

#include <cstdint>
#include <string_view>

namespace cg = st::feature::codegen;
namespace ir = st::feature::ir;

namespace {

// A definition pack with one hook, `after_fuel_calc`, that has a
// usable ecu_address + free_ram region. Just enough for the codegen
// tests — no axes/scalings/tables/identifications, since the SH-2A
// emit path only consults `hooks()`.
constexpr std::string_view kPackOneHookToml = R"toml(
[pack]
schema_version = 1
id             = "test-pack-codegen"
endianness     = "big"

[[hook]]
id              = "after_fuel_calc"
ecu_address     = 0x000ABCD0
free_ram        = { base = 0x40000000, length = 256 }
inputs = [
  { name = "rpm",  label = "Engine RPM", type = "int", unit = "rpm", address = 0x000F1234 },
  { name = "load", label = "Engine load", type = "int", unit = "%",   address = 0x000F1238 },
  { name = "no_addr_pin", type = "int" },
]
outputs = [
  { name = "commanded_pw_override", label = "Override fuel PW", type = "int", unit = "ms" },
  { name = "second_override",        label = "Second override",  type = "int", unit = "ms" },
]
)toml";

// Pack with a hook that has no ecu_address — used to verify the
// "must declare splice point" error path.
constexpr std::string_view kPackNoEcuAddrToml = R"toml(
[pack]
schema_version = 1
id             = "no-ecu-addr"
endianness     = "big"

[[hook]]
id        = "after_fuel_calc"
free_ram  = { base = 0x40000000, length = 256 }
outputs = [
  { name = "commanded_pw_override", type = "int" },
]
)toml";

// Pack with a hook that has no free_ram region.
constexpr std::string_view kPackNoFreeRamToml = R"toml(
[pack]
schema_version = 1
id             = "no-free-ram"
endianness     = "big"

[[hook]]
id              = "after_fuel_calc"
ecu_address     = 0x000ABCD0
outputs = [
  { name = "commanded_pw_override", type = "int" },
]
)toml";

// LoadConstant instruction factory — concise constructor for tests.
ir::Instruction load_const_int(ir::ValueId result_id, std::int64_t value) {
    ir::Instruction ins{};
    ins.op             = ir::Op::LoadConstant;
    ins.result_type    = st::feature::PinType::Int;
    ins.result_id      = result_id;
    ins.constant_value = static_cast<double>(value);
    return ins;
}

ir::Instruction load_const_float(ir::ValueId result_id, double value) {
    ir::Instruction ins{};
    ins.op             = ir::Op::LoadConstant;
    ins.result_type    = st::feature::PinType::Float;
    ins.result_id      = result_id;
    ins.constant_value = value;
    return ins;
}

ir::Instruction load_hook_input(ir::ValueId result_id,
                                 std::string hook_id,
                                 std::string pin_name,
                                 st::feature::PinType type
                                     = st::feature::PinType::Int) {
    ir::Instruction ins{};
    ins.op          = ir::Op::LoadHookInput;
    ins.result_type = type;
    ins.result_id   = result_id;
    ins.symbol      = std::move(hook_id);
    ins.pin_name    = std::move(pin_name);
    return ins;
}

ir::Instruction call_primitive(ir::ValueId result_id,
                                std::string symbol,
                                std::vector<ir::ValueId> operands,
                                st::feature::PinType type
                                    = st::feature::PinType::Int) {
    ir::Instruction ins{};
    ins.op          = ir::Op::CallPrimitive;
    ins.result_type = type;
    ins.result_id   = result_id;
    ins.symbol      = std::move(symbol);
    ins.pin_name    = "out";  // single-output primitives
    ins.operands    = std::move(operands);
    return ins;
}

ir::Instruction store(std::string symbol, std::string pin_name,
                      ir::ValueId operand) {
    ir::Instruction ins{};
    ins.op       = ir::Op::StoreHookOutput;
    ins.symbol   = std::move(symbol);
    ins.pin_name = std::move(pin_name);
    ins.operands = {operand};
    return ins;
}

// Read a big-endian 16-bit value from the byte vector at `offset`.
[[nodiscard]] std::uint16_t be16_at(std::vector<std::uint8_t> const &code,
                                     std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(code.at(offset))     << 8U)
        | static_cast<std::uint16_t>(code.at(offset + 1)));
}

[[nodiscard]] std::uint32_t be32_at(std::vector<std::uint8_t> const &code,
                                     std::size_t offset) {
    return (static_cast<std::uint32_t>(code.at(offset))     << 24U)
         | (static_cast<std::uint32_t>(code.at(offset + 1)) << 16U)
         | (static_cast<std::uint32_t>(code.at(offset + 2)) << 8U)
         |  static_cast<std::uint32_t>(code.at(offset + 3));
}

} // namespace

TEST_CASE("Sh2aBackend: empty module compiles to an empty PatchObject",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->arch == cg::Arch::Sh2a);
    REQUIRE(r->hooks.empty());
}

TEST_CASE("Sh2aBackend: LoadConstant with no consumer is dead code",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.empty());  // no Store ⇒ no emitted hook
}

TEST_CASE("Sh2aBackend: LoadConstant + Store emits 20-byte sequence",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->arch == cg::Arch::Sh2a);
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.symbol == "after_fuel_calc");
    REQUIRE(hp.splice_address == 0x000ABCD0);
    REQUIRE(hp.code.size() == cg::sh2a::kStoreSequenceSize);

    // Verify the SH-2A opcode bytes against the documented layout.
    REQUIRE(be16_at(hp.code, 0)  == 0xD002);  // MOV.L @(2, PC), R0
    REQUIRE(be16_at(hp.code, 2)  == 0xD103);  // MOV.L @(3, PC), R1
    REQUIRE(be16_at(hp.code, 4)  == 0x2102);  // MOV.L R0, @R1
    REQUIRE(be16_at(hp.code, 6)  == 0x000B);  // RTS
    REQUIRE(be16_at(hp.code, 8)  == 0x0009);  // NOP (delay slot)
    REQUIRE(be16_at(hp.code, 10) == 0x0009);  // NOP (pool alignment pad)

    // Literal pool: constant 42, then destination address.
    REQUIRE(be32_at(hp.code, 12) == 42U);
    REQUIRE(be32_at(hp.code, 16) == 0x40000000U);  // free_ram_base

    // One RAM claim for the output slot.
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(hp.ram_claims[0].address == 0x40000000);
    REQUIRE(hp.ram_claims[0].size == 4);
}

TEST_CASE("Sh2aBackend: signed-int constants encode as two's complement",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, -1));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    // -1 as a 32-bit two's complement value is 0xFFFFFFFF.
    REQUIRE(be32_at(r->hooks[0].code, 12) == 0xFFFFFFFFU);
}

TEST_CASE("Sh2aBackend: two stores to the same pin share a RAM slot",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 11));
    m.instructions.push_back(load_const_int(2, 22));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 2));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == 2 * cg::sh2a::kStoreSequenceSize);
    // Two emit sequences but only one RAM claim — both Stores write to
    // the same address.
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(be32_at(hp.code, 12) == 11U);
    REQUIRE(be32_at(hp.code, 16) == 0x40000000U);
    REQUIRE(be32_at(hp.code, 32) == 22U);  // 12 + 20 = 32
    REQUIRE(be32_at(hp.code, 36) == 0x40000000U);
}

TEST_CASE("Sh2aBackend: distinct pins get distinct RAM slots",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 11));
    m.instructions.push_back(load_const_int(2, 22));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    m.instructions.push_back(store("after_fuel_calc",
                                    "second_override", 2));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.ram_claims.size() == 2);
    REQUIRE(hp.ram_claims[0].address == 0x40000000);
    REQUIRE(hp.ram_claims[1].address == 0x40000004);
    // Second store points at the second slot.
    REQUIRE(be32_at(hp.code, 36) == 0x40000004U);
}

TEST_CASE("Sh2aBackend: Float LoadConstant emits IEEE 754 bit pattern",
          "[feature_codegen][sh2a][float]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_float(1, 1.5));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == cg::sh2a::kStoreSequenceSize);
    // 1.5f as IEEE 754 binary32 = 0x3FC00000 (sign 0, exp 127, mantissa 0.5).
    REQUIRE(be32_at(hp.code, 12) == 0x3FC00000U);
    REQUIRE(be32_at(hp.code, 16) == 0x40000000U);  // dest = free_ram_base
}

TEST_CASE("Sh2aBackend: LoadHookInput with no consumer is dead code",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    ir::Instruction ins{};
    ins.op          = ir::Op::LoadHookInput;
    ins.result_id   = 1;
    ins.result_type = st::feature::PinType::Int;
    ins.symbol      = "after_fuel_calc";
    ins.pin_name    = "rpm";
    m.instructions.push_back(ins);
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.empty());
}

TEST_CASE("Sh2aBackend: CallPrimitive with no consumer is dead code",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    ir::Instruction ins{};
    ins.op          = ir::Op::CallPrimitive;
    ins.symbol      = "add_int";
    ins.result_id   = 1;
    ins.result_type = st::feature::PinType::Int;
    m.instructions.push_back(ins);
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.empty());
}

TEST_CASE("Sh2aBackend: missing hook id returns InvalidArgument",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    m.instructions.push_back(store("ghost_hook",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Sh2aBackend: hook without ecu_address returns InvalidArgument",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackNoEcuAddrToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Sh2aBackend: hook without free_ram returns InvalidArgument",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackNoFreeRamToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Sh2aBackend: out-of-int32-range constant returns ParseError",
          "[feature_codegen][sh2a]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    ir::Instruction lc{};
    lc.op             = ir::Op::LoadConstant;
    lc.result_type    = st::feature::PinType::Int;
    lc.result_id      = 1;
    lc.constant_value = 5e9;  // > INT32_MAX
    m.instructions.push_back(lc);
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

// ---- LoadHookInput slice ------------------------------------------------

TEST_CASE("Sh2aBackend: LoadHookInput + Store emits 20-byte sequence",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(1, "after_fuel_calc", "rpm"));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->arch == cg::Arch::Sh2a);
    REQUIRE(r->hooks.size() == 1);

    auto const &hp = r->hooks[0];
    REQUIRE(hp.symbol == "after_fuel_calc");
    REQUIRE(hp.splice_address == 0x000ABCD0);
    REQUIRE(hp.code.size() == cg::sh2a::kLoadStoreSequenceSize);

    // Verify the SH-2A opcode bytes against the load-deref-store
    // layout documented in feature_codegen.cpp.
    REQUIRE(be16_at(hp.code, 0)  == 0xD002);  // MOV.L @(2, PC), R0
    REQUIRE(be16_at(hp.code, 2)  == 0x6002);  // MOV.L @R0, R0
    REQUIRE(be16_at(hp.code, 4)  == 0xD102);  // MOV.L @(2, PC), R1
    REQUIRE(be16_at(hp.code, 6)  == 0x2102);  // MOV.L R0, @R1
    REQUIRE(be16_at(hp.code, 8)  == 0x000B);  // RTS
    REQUIRE(be16_at(hp.code, 10) == 0x0009);  // NOP (delay slot)

    // Literal pool: source address, then destination address.
    REQUIRE(be32_at(hp.code, 12) == 0x000F1234U);  // rpm pin address
    REQUIRE(be32_at(hp.code, 16) == 0x40000000U);  // free_ram_base

    // One RAM claim for the output slot. No claim for the SSA value
    // itself — it flows through R0 register.
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(hp.ram_claims[0].address == 0x40000000);
}

TEST_CASE("Sh2aBackend: distinct LoadHookInputs target distinct addresses",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(1, "after_fuel_calc", "rpm"));
    m.instructions.push_back(load_hook_input(2, "after_fuel_calc", "load"));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    m.instructions.push_back(store("after_fuel_calc",
                                    "second_override", 2));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == 2 * cg::sh2a::kLoadStoreSequenceSize);
    REQUIRE(hp.ram_claims.size() == 2);
    // First chain: rpm → first output slot.
    REQUIRE(be32_at(hp.code, 12) == 0x000F1234U);
    REQUIRE(be32_at(hp.code, 16) == 0x40000000U);
    // Second chain: load → second output slot at base+4.
    REQUIRE(be32_at(hp.code, 32) == 0x000F1238U);
    REQUIRE(be32_at(hp.code, 36) == 0x40000004U);
}

TEST_CASE("Sh2aBackend: mixed LoadConstant + LoadHookInput in one module",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    m.instructions.push_back(load_hook_input(2, "after_fuel_calc", "rpm"));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    m.instructions.push_back(store("after_fuel_calc",
                                    "second_override", 2));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    // Both shapes are 20 bytes, so total = 40.
    REQUIRE(hp.code.size() == 40);
    // First chain (LoadConstant → Store) literal pool at 12,16.
    REQUIRE(be32_at(hp.code, 12) == 42U);
    REQUIRE(be32_at(hp.code, 16) == 0x40000000U);
    // First instruction of the second chain (offset 20) is the
    // LoadHookInput "MOV.L @(2, PC), R0" preamble — 0xD002.
    REQUIRE(be16_at(hp.code, 20) == 0xD002);
    REQUIRE(be16_at(hp.code, 22) == 0x6002);  // dereference
    // Second chain literal pool at 32,36.
    REQUIRE(be32_at(hp.code, 32) == 0x000F1234U);
    REQUIRE(be32_at(hp.code, 36) == 0x40000004U);
}

TEST_CASE("Sh2aBackend: LoadHookInput Float emits same MOV.L shape",
          "[feature_codegen][sh2a][float][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(
        1, "after_fuel_calc", "rpm", st::feature::PinType::Float));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    // MOV.L is type-agnostic — 4 bytes is 4 bytes. The bytes through
    // R0 are the same whether the upstream pin is Float or Int.
    REQUIRE(r->hooks[0].code.size() == cg::sh2a::kLoadStoreSequenceSize);
    REQUIRE(be32_at(r->hooks[0].code, 12) == 0x000F1234U);  // rpm pin addr
}

TEST_CASE("Sh2aBackend: LoadHookInput referencing unknown hook fails",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(1, "ghost_hook", "rpm"));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Sh2aBackend: LoadHookInput referencing unknown pin fails",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(
        1, "after_fuel_calc", "ghost_pin"));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("Sh2aBackend: LoadHookInput pin without address fails",
          "[feature_codegen][sh2a][load_hook_input]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(
        1, "after_fuel_calc", "no_addr_pin"));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---- CallPrimitive add_int slice ----------------------------------------

TEST_CASE("Sh2aBackend: add_int(const,const) → Store emits 28-byte sequence",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 42));
    m.instructions.push_back(load_const_int(2, 8));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 3));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == 28);

    // 7 body instructions + 1 pool-pad NOP + 3 longwords.
    REQUIRE(be16_at(hp.code, 0)  == 0xD003);  // MOV.L @(3, PC), R0
    REQUIRE(be16_at(hp.code, 2)  == 0xD104);  // MOV.L @(4, PC), R1
    REQUIRE(be16_at(hp.code, 4)  == 0x310C);  // ADD R0, R1
    REQUIRE(be16_at(hp.code, 6)  == 0xD204);  // MOV.L @(4, PC), R2
    REQUIRE(be16_at(hp.code, 8)  == 0x2212);  // MOV.L R1, @R2
    REQUIRE(be16_at(hp.code, 10) == 0x000B);  // RTS
    REQUIRE(be16_at(hp.code, 12) == 0x0009);  // NOP delay
    REQUIRE(be16_at(hp.code, 14) == 0x0009);  // NOP pool pad

    REQUIRE(be32_at(hp.code, 16) == 42U);
    REQUIRE(be32_at(hp.code, 20) == 8U);
    REQUIRE(be32_at(hp.code, 24) == 0x40000000U);

    // Output slot lives in free_ram. No intermediate spill (R0/R1
    // carry the operands across the ADD).
    REQUIRE(hp.ram_claims.size() == 1);
    REQUIRE(hp.ram_claims[0].address == 0x40000000);
}

TEST_CASE("Sh2aBackend: add_int(hook,hook) → Store emits 32-byte sequence",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_hook_input(1, "after_fuel_calc", "rpm"));
    m.instructions.push_back(load_hook_input(2, "after_fuel_calc", "load"));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 3));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == 32);

    // 9 body instructions + 1 pool-pad NOP + 3 longwords.
    REQUIRE(be16_at(hp.code, 0)  == 0xD004);  // MOV.L @(4, PC), R0 (rpm addr)
    REQUIRE(be16_at(hp.code, 2)  == 0x6002);  // MOV.L @R0, R0      (deref)
    REQUIRE(be16_at(hp.code, 4)  == 0xD104);  // MOV.L @(4, PC), R1 (load addr)
    REQUIRE(be16_at(hp.code, 6)  == 0x6112);  // MOV.L @R1, R1      (deref)
    REQUIRE(be16_at(hp.code, 8)  == 0x310C);  // ADD R0, R1
    REQUIRE(be16_at(hp.code, 10) == 0xD204);  // MOV.L @(4, PC), R2 (dest)
    REQUIRE(be16_at(hp.code, 12) == 0x2212);  // MOV.L R1, @R2
    REQUIRE(be16_at(hp.code, 14) == 0x000B);  // RTS
    REQUIRE(be16_at(hp.code, 16) == 0x0009);  // NOP delay
    REQUIRE(be16_at(hp.code, 18) == 0x0009);  // NOP pool pad

    REQUIRE(be32_at(hp.code, 20) == 0x000F1234U);  // rpm pin address
    REQUIRE(be32_at(hp.code, 24) == 0x000F1238U);  // load pin address
    REQUIRE(be32_at(hp.code, 28) == 0x40000000U);  // free_ram_base
}

TEST_CASE("Sh2aBackend: add_int(const,hook) → Store emits 28-byte sequence",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 100));
    m.instructions.push_back(load_hook_input(2, "after_fuel_calc", "rpm"));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 3));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == 28);

    // 8 body instructions + no pool-pad (16-byte body is already
    // 4-aligned) + 3 longwords. Disp values depend on each MOV.L's
    // own pc-rel alignment: at pc=8, ((8+4)&~3)=12 so disp=(24-12)/4=3.
    REQUIRE(be16_at(hp.code, 0)  == 0xD003);  // MOV.L @(3, PC), R0  (const)
    REQUIRE(be16_at(hp.code, 2)  == 0xD104);  // MOV.L @(4, PC), R1  (rpm addr)
    REQUIRE(be16_at(hp.code, 4)  == 0x6112);  // MOV.L @R1, R1       (deref)
    REQUIRE(be16_at(hp.code, 6)  == 0x310C);  // ADD R0, R1
    REQUIRE(be16_at(hp.code, 8)  == 0xD203);  // MOV.L @(3, PC), R2  (dest)
    REQUIRE(be16_at(hp.code, 10) == 0x2212);  // MOV.L R1, @R2
    REQUIRE(be16_at(hp.code, 12) == 0x000B);  // RTS
    REQUIRE(be16_at(hp.code, 14) == 0x0009);  // NOP delay

    REQUIRE(be32_at(hp.code, 16) == 100U);
    REQUIRE(be32_at(hp.code, 20) == 0x000F1234U);
    REQUIRE(be32_at(hp.code, 24) == 0x40000000U);
}

TEST_CASE("Sh2aBackend: add_int operand-order matters for emit layout",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    // hook first, const second — same shape size (28 bytes) but the
    // deref happens on op1, not op2.
    m.instructions.push_back(load_hook_input(1, "after_fuel_calc", "rpm"));
    m.instructions.push_back(load_const_int(2, 7));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 3));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.code.size() == 28);
    REQUIRE(be16_at(hp.code, 0) == 0xD003);  // MOV.L @(3, PC), R0  (rpm addr)
    REQUIRE(be16_at(hp.code, 2) == 0x6002);  // MOV.L @R0, R0       (deref)
    // pc=4: ((4+4)&~3) = 8; pool_op2 = 20; disp = (20-8)/4 = 3.
    REQUIRE(be16_at(hp.code, 4) == 0xD103);  // MOV.L @(3, PC), R1  (const)
    REQUIRE(be16_at(hp.code, 6) == 0x310C);  // ADD R0, R1
}

TEST_CASE("Sh2aBackend: unknown primitive id returns NotImplemented",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 1));
    m.instructions.push_back(load_const_int(2, 2));
    m.instructions.push_back(call_primitive(3, "multiply_int", {1, 2}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 3));

    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}

TEST_CASE("Sh2aBackend: add_int with wrong operand count returns ParseError",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 1));
    m.instructions.push_back(call_primitive(2, "add_int", {1}));  // only 1 operand
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 2));

    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("Sh2aBackend: add_int with Float operand returns NotImplemented",
          "[feature_codegen][sh2a][call_primitive]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_float(1, 1.5));
    m.instructions.push_back(load_const_int(2, 2));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 3));

    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}

// ---- Float LoadConstant slice -------------------------------------------

TEST_CASE("Sh2aBackend: Float LoadConstant special values",
          "[feature_codegen][sh2a][float]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    // 0.0  -> 0x00000000
    // -1.0 -> 0xBF800000
    // 2.0  -> 0x40000000
    struct Case {
        double        input;
        std::uint32_t expected_bits;
    };
    auto const cases = std::vector<Case>{
        {  0.0,  0x00000000U },
        { -1.0,  0xBF800000U },
        {  2.0,  0x40000000U },
        {  3.14f, 0x4048F5C3U },  // exact 3.14f bit pattern
    };

    for (auto const &c : cases) {
        cg::Sh2aBackend backend;
        ir::Module      m;
        m.instructions.push_back(load_const_float(1, c.input));
        m.instructions.push_back(store("after_fuel_calc",
                                        "commanded_pw_override", 1));
        auto r = backend.compile(m, *def);
        REQUIRE(r.has_value());
        REQUIRE(r->hooks.size() == 1);
        REQUIRE(be32_at(r->hooks[0].code, 12) == c.expected_bits);
    }
}

TEST_CASE("Sh2aBackend: Float LoadConstant narrows from double silently",
          "[feature_codegen][sh2a][float]") {
    // 0.1 has no exact binary representation. Double-stored 0.1 narrows
    // to the nearest float (0x3DCCCCCD). The codegen does NOT refuse —
    // the float pin author is consenting to single-precision.
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_float(1, 0.1));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(be32_at(r->hooks[0].code, 12) == 0x3DCCCCCDU);
}

TEST_CASE("Sh2aBackend: Bool LoadConstant returns NotImplemented",
          "[feature_codegen][sh2a][bool]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    ir::Instruction lc{};
    lc.op             = ir::Op::LoadConstant;
    lc.result_type    = st::feature::PinType::Bool;
    lc.result_id      = 1;
    lc.constant_value = 1.0;
    m.instructions.push_back(lc);
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 1));
    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}

// ---- Nested CallPrimitive ----------------------------------------------

TEST_CASE("Sh2aBackend: nested add(add(LC,LC), LC) → Store allocates a slot",
          "[feature_codegen][sh2a][nested]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 5));
    m.instructions.push_back(load_const_int(2, 3));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));   // = 8
    m.instructions.push_back(load_const_int(4, 10));
    m.instructions.push_back(call_primitive(5, "add_int", {3, 4}));   // = 18
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 5));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];

    // Two RAM claims (in allocation order):
    //   - claims[0]: output pin slot @ free_ram_base (allocated first
    //                by the Store-processing loop's pin_slots logic)
    //   - claims[1]: intermediate slot for %3 @ free_ram_base + 4
    //                (allocated by the nested walk for non-root prims)
    REQUIRE(hp.ram_claims.size() == 2);
    REQUIRE(hp.ram_claims[0].address == 0x40000000);  // output pin
    REQUIRE(hp.ram_claims[0].size == 4);
    REQUIRE(hp.ram_claims[1].address == 0x40000004);  // intermediate

    // Body layout (5 instrs inner + 6 instrs outer + RTS + NOP +
    // pool-alignment NOP = 26 bytes; pool of 6 longwords = 24 bytes;
    // total = 50 ... but wait, 22+2+2 = 26 isn't 4-aligned at 26,
    // pad adds 2 → body 28, pool 24 = 52.
    REQUIRE(hp.code.size() == 52);

    // Inner add fragment (offsets 0..9): op1=Const 5, op2=Const 3,
    // dest=intermediate slot @ 0x40000004.
    REQUIRE(be16_at(hp.code, 0)  == 0xD006);  // MOV.L @(6,PC), R0  (pool[0]=5 @ 28)
    REQUIRE(be16_at(hp.code, 2)  == 0xD107);  // MOV.L @(7,PC), R1  (pool[1]=3 @ 32)
    REQUIRE(be16_at(hp.code, 4)  == 0x310C);  // ADD R0, R1
    REQUIRE(be16_at(hp.code, 6)  == 0xD207);  // MOV.L @(7,PC), R2  (pool[2]=slot @ 36)
    REQUIRE(be16_at(hp.code, 8)  == 0x2212);  // MOV.L R1, @R2

    // Outer add fragment (offsets 10..21): op1=intermediate slot
    // (with deref), op2=Const 10, dest=output pin @ 0x40000000.
    REQUIRE(be16_at(hp.code, 10) == 0xD007);  // MOV.L @(7,PC), R0  (pool[3]=slot @ 40)
    REQUIRE(be16_at(hp.code, 12) == 0x6002);  // MOV.L @R0, R0       (deref)
    REQUIRE(be16_at(hp.code, 14) == 0xD107);  // MOV.L @(7,PC), R1  (pool[4]=10 @ 44)
    REQUIRE(be16_at(hp.code, 16) == 0x310C);  // ADD R0, R1
    REQUIRE(be16_at(hp.code, 18) == 0xD207);  // MOV.L @(7,PC), R2  (pool[5]=dest @ 48)
    REQUIRE(be16_at(hp.code, 20) == 0x2212);  // MOV.L R1, @R2

    // Epilogue.
    REQUIRE(be16_at(hp.code, 22) == 0x000B);  // RTS
    REQUIRE(be16_at(hp.code, 24) == 0x0009);  // NOP (delay slot)
    REQUIRE(be16_at(hp.code, 26) == 0x0009);  // NOP (pool-align pad)

    // Pool.
    REQUIRE(be32_at(hp.code, 28) == 5U);
    REQUIRE(be32_at(hp.code, 32) == 3U);
    REQUIRE(be32_at(hp.code, 36) == 0x40000004U);  // intermediate
    REQUIRE(be32_at(hp.code, 40) == 0x40000004U);  // intermediate (dup)
    REQUIRE(be32_at(hp.code, 44) == 10U);
    REQUIRE(be32_at(hp.code, 48) == 0x40000000U);  // output pin
}

TEST_CASE("Sh2aBackend: nested add(LC, add(LC, LC)) → Store (right-deep)",
          "[feature_codegen][sh2a][nested]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 100));
    m.instructions.push_back(load_const_int(2, 5));
    m.instructions.push_back(load_const_int(3, 3));
    m.instructions.push_back(call_primitive(4, "add_int", {2, 3}));   // inner = 8
    m.instructions.push_back(call_primitive(5, "add_int", {1, 4}));   // outer = 108
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 5));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    auto const &hp = r->hooks[0];
    REQUIRE(hp.ram_claims.size() == 2);
    REQUIRE(hp.code.size() == 52);  // same size as left-deep
}

TEST_CASE("Sh2aBackend: 3-level nested add tree → Store",
          "[feature_codegen][sh2a][nested]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    // ((1+2) + 4) + 8 — three levels of add, four LoadConstants.
    m.instructions.push_back(load_const_int(1, 1));
    m.instructions.push_back(load_const_int(2, 2));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));   // = 3
    m.instructions.push_back(load_const_int(4, 4));
    m.instructions.push_back(call_primitive(5, "add_int", {3, 4}));   // = 7
    m.instructions.push_back(load_const_int(6, 8));
    m.instructions.push_back(call_primitive(7, "add_int", {5, 6}));   // = 15
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 7));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    auto const &hp = r->hooks[0];
    // 3 intermediate slots (slots for %3, %5; root %7 writes to
    // output pin) + 1 output pin slot = 3 total claims.
    REQUIRE(hp.ram_claims.size() == 3);
    REQUIRE(hp.ram_claims[0].address == 0x40000000);  // output pin
    REQUIRE(hp.ram_claims[1].address == 0x40000004);  // intermediate %3
    REQUIRE(hp.ram_claims[2].address == 0x40000008);  // intermediate %5
}

TEST_CASE("Sh2aBackend: nested add with LoadHookInput leaf",
          "[feature_codegen][sh2a][nested]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    // ((rpm + 100) + load) — mixed Const + HookInput operands at
    // different nesting levels.
    m.instructions.push_back(load_hook_input(1, "after_fuel_calc", "rpm"));
    m.instructions.push_back(load_const_int(2, 100));
    m.instructions.push_back(call_primitive(3, "add_int", {1, 2}));
    m.instructions.push_back(load_hook_input(4, "after_fuel_calc", "load"));
    m.instructions.push_back(call_primitive(5, "add_int", {3, 4}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 5));

    auto r = backend.compile(m, *def);
    REQUIRE(r.has_value());
    REQUIRE(r->hooks.size() == 1);
    REQUIRE(r->hooks[0].ram_claims.size() == 2);
}

TEST_CASE("Sh2aBackend: nested primitive with unsupported id fails",
          "[feature_codegen][sh2a][nested]") {
    auto def = st::Definition::from_toml_string(kPackOneHookToml);
    REQUIRE(def.has_value());

    cg::Sh2aBackend backend;
    ir::Module      m;
    m.instructions.push_back(load_const_int(1, 1));
    m.instructions.push_back(load_const_int(2, 2));
    m.instructions.push_back(call_primitive(3, "multiply_int", {1, 2}));
    m.instructions.push_back(load_const_int(4, 10));
    m.instructions.push_back(call_primitive(5, "add_int", {3, 4}));
    m.instructions.push_back(store("after_fuel_calc",
                                    "commanded_pw_override", 5));

    auto r = backend.compile(m, *def);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
}
