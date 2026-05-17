// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_codegen.hpp"

#include "sh2a.hpp"
#include "st/core/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace st::feature::codegen {

namespace {

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

} // namespace

char const *arch_name(Arch a) noexcept {
    switch (a) {
        case Arch::Sh2a:    return "sh2a";
        case Arch::Rh850:   return "rh850";
        case Arch::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view arch_name_sv(Arch a) noexcept { return arch_name(a); }

// ---- RamAllocator -------------------------------------------------------

RamAllocator::RamAllocator(std::size_t base, std::size_t length) noexcept
    : base_(base), length_(length), cursor_(base) {}

Result<RamClaim> RamAllocator::claim(std::size_t size,
                                      std::size_t alignment) noexcept {
    if (size == 0) {
        return failure(ErrorCode::InvalidArgument,
                       "RamAllocator::claim: size must be > 0");
    }
    // Normalize alignment: <2 means "no alignment" → 1. Otherwise it
    // must be a power of two so cursor rounding has a single-step
    // closed form.
    std::size_t const align = alignment < 2U ? 1U : alignment;
    if (align != 1U && !is_power_of_two(align)) {
        return failure(ErrorCode::InvalidArgument,
                       "RamAllocator::claim: alignment must be a power of two");
    }

    // Round cursor up to `align`. Overflow-guarded — if cursor + (align - 1)
    // would wrap past length_, refuse before we compute the masked address.
    std::size_t const mask     = align - 1U;
    std::size_t const slack    = (align - (cursor_ & mask)) & mask;
    if (slack > length_ || cursor_ + slack < cursor_) {
        return failure(ErrorCode::OutOfRange,
                       "RamAllocator::claim: alignment overflow");
    }
    std::size_t const aligned = cursor_ + slack;

    // Bounds check the claim itself.
    if (aligned + size < aligned) {
        return failure(ErrorCode::OutOfRange,
                       "RamAllocator::claim: size overflow");
    }
    if (aligned + size > base_ + length_) {
        return failure(ErrorCode::OutOfRange,
                       "RamAllocator::claim: region exhausted");
    }

    RamClaim out{};
    out.address   = aligned;
    out.size      = size;
    out.alignment = align;
    cursor_       = aligned + size;
    return out;
}

void RamAllocator::reset() noexcept { cursor_ = base_; }

// ---- Backend stubs ------------------------------------------------------

namespace {

// Append a 16-bit instruction to `code` in big-endian order. SH-2A is
// big-endian on the bus; the literal pool longwords are emitted the
// same way (high byte first).
void emit_be16(std::vector<std::uint8_t> &code, std::uint16_t v) {
    code.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    code.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

void emit_be32(std::vector<std::uint8_t> &code, std::uint32_t v) {
    code.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFU));
    code.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
    code.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    code.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

// Emit the canonical "load constant, store to RAM slot" sequence per
// the layout documented in sh2a.hpp. constant_value lands at literal-
// pool offset 0 (code offset 12); destination_address at offset 4
// (code offset 16). PC-relative displacements computed once, here.
//
//   offset  bytes        instruction
//   0       D002         MOV.L @(2, PC), R0  ; R0 = constant
//   2       D103         MOV.L @(3, PC), R1  ; R1 = destination
//   4       2102         MOV.L R0, @R1       ; mem[R1] = R0
//   6       000B         RTS
//   8       0009         NOP                 ; delay slot
//   10      0009         NOP                 ; pad to 4-byte boundary
//   12      <const>      literal: constant (big-endian)
//   16      <addr>       literal: destination address (big-endian)
//
// PC-relative formula for MOV.L @(disp, PC), Rn:
//   target = ((this_pc + 4) & ~3) + disp*4
// At PC=0: (4 & ~3) = 4; for target 12, disp = (12-4)/4 = 2.
// At PC=2: (6 & ~3) = 4; for target 16, disp = (16-4)/4 = 3.
void emit_load_const_store_sequence(std::vector<std::uint8_t> &code,
                                     std::uint32_t constant_value,
                                     std::uint32_t destination_address) {
    using namespace sh2a;
    emit_be16(code, enc_mov_l_disp_pc(Reg::R0, 2));      // load constant
    emit_be16(code, enc_mov_l_disp_pc(Reg::R1, 3));      // load destination
    emit_be16(code, enc_mov_l_reg_at_reg(Reg::R0, Reg::R1));  // store
    emit_be16(code, enc_rts());
    emit_be16(code, enc_nop());                          // delay slot
    emit_be16(code, enc_nop());                          // pool alignment pad
    emit_be32(code, constant_value);
    emit_be32(code, destination_address);
}

// Look up a hook by id in the loaded definition. Linear scan — pack
// hook counts are small (<<100 in practice).
[[nodiscard]] Hook const *find_hook(Definition const &def,
                                     std::string_view id) noexcept {
    for (auto const &h : def.hooks()) {
        if (h.id == id) return &h;
    }
    return nullptr;
}

// Locate the LoadConstant Instruction that produced `value_id`.
// Returns nullptr if `value_id` doesn't resolve to any LoadConstant —
// caller turns that into a NotImplemented error (the operand came
// from a non-LoadConstant op, which the slice doesn't support yet).
[[nodiscard]] ir::Instruction const *find_load_constant(
    ir::Module const &m, ir::ValueId value_id) noexcept {
    for (auto const &ins : m.instructions) {
        if (ins.op == ir::Op::LoadConstant && ins.result_id == value_id) {
            return &ins;
        }
    }
    return nullptr;
}

// Per-hook accumulator. We group emitted code by hook so the
// PatchObject's HookPatch list has one entry per hook the user wrote
// to, even if a Module touches several hooks (e.g. a launch-control
// .stmod writing both ignition-cut and rev-limit hooks).
struct HookWork {
    Hook const               *hook{nullptr};
    RamAllocator              ram{0, 0};
    std::vector<std::uint8_t> code;
    std::vector<RamClaim>     claims;
    // (output_pin_name) → claim address. One slot per output pin
    // referenced by Store instructions in this module — repeated
    // writes to the same pin share the slot so the firmware reads a
    // single live address.
    std::unordered_map<std::string, std::uint32_t> pin_slots;
};

} // namespace

Result<PatchObject> Sh2aBackend::compile(ir::Module const &m,
                                          Definition const &def) {
    // First pass: refuse anything outside the LoadConstant + Store-Int
    // shape with a specific per-Op message so callers know exactly
    // which feature is still pending. The shape rejection is structural
    // (which Op appeared), the per-Store checks below are semantic
    // (whether the LoadConstant operand is Int, etc.).
    for (auto const &ins : m.instructions) {
        switch (ins.op) {
            case ir::Op::LoadConstant:
            case ir::Op::StoreHookOutput:
                break;  // supported in this slice
            case ir::Op::LoadHookInput:
                return failure(ErrorCode::NotImplemented,
                               "SH-2A backend: LoadHookInput not yet "
                               "implemented (slice supports only "
                               "LoadConstant + StoreHookOutput)");
            case ir::Op::CallPrimitive:
                return failure(ErrorCode::NotImplemented,
                               "SH-2A backend: CallPrimitive not yet "
                               "implemented (slice supports only "
                               "LoadConstant + StoreHookOutput)");
        }
    }

    // Second pass: walk Stores. Each one drives the emit. Group by
    // hook so we produce one HookPatch per hook touched. We don't
    // emit anything for unconsumed LoadConstants — that's dead code
    // and the patch insertion layer doesn't need bytes for it.
    //
    // `ordered_hooks` keeps the iteration order deterministic
    // (first-touch order in the Module's instruction list) so test
    // expectations are stable.
    std::map<std::string, HookWork>             works;
    std::vector<std::string>                    ordered_hooks;

    for (auto const &ins : m.instructions) {
        if (ins.op != ir::Op::StoreHookOutput) continue;

        if (ins.operands.size() != 1) {
            return failure(ErrorCode::ParseError,
                           "SH-2A backend: StoreHookOutput must have "
                           "exactly one operand");
        }
        // Find the source instruction. Slice rule: it must be a
        // LoadConstant. Anything else (LoadHookInput / CallPrimitive)
        // was caught by the first-pass filter, but a Store consuming
        // a non-existent ValueId is its own error.
        ir::Instruction const *src =
            find_load_constant(m, ins.operands[0]);
        if (src == nullptr) {
            return failure(ErrorCode::NotImplemented,
                           "SH-2A backend: StoreHookOutput operand "
                           "does not resolve to a LoadConstant (only "
                           "direct LoadConstant operands supported in "
                           "this slice)");
        }

        // Int only in this slice. Float/Bool need narrowing /
        // truncation considerations we defer to follow-up bundles.
        if (src->result_type != PinType::Int) {
            std::string msg{
                "SH-2A backend: LoadConstant of type "};
            msg.append(pin_type_name(src->result_type));
            msg.append(" not yet implemented (slice supports Int "
                       "only — Float bit-cast and Bool widening "
                       "land in follow-up bundles)");
            return failure(ErrorCode::NotImplemented, std::move(msg));
        }
        if (!src->constant_value.has_value()) {
            return failure(ErrorCode::ParseError,
                           "SH-2A backend: LoadConstant has no "
                           "constant_value (lower() invariant violation)");
        }

        // Resolve the target hook from the Store's symbol.
        Hook const *hook = find_hook(def, ins.symbol);
        if (hook == nullptr) {
            std::string msg{"SH-2A backend: hook '"};
            msg.append(ins.symbol);
            msg.append("' not declared in the loaded definition pack");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        if (!hook->ecu_address.has_value()) {
            std::string msg{"SH-2A backend: hook '"};
            msg.append(ins.symbol);
            msg.append("' has no ecu_address — pack must declare the "
                       "splice point for codegen");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        if (!hook->free_ram_base.has_value()
            || !hook->free_ram_length.has_value()
            || *hook->free_ram_length == 0) {
            std::string msg{"SH-2A backend: hook '"};
            msg.append(ins.symbol);
            msg.append("' has no free_ram region — pack must declare "
                       "scratch RAM for codegen");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }

        // First-touch: create a per-hook work item with its own
        // RamAllocator over the hook's free_ram region.
        auto it = works.find(hook->id);
        if (it == works.end()) {
            HookWork w{};
            w.hook = hook;
            w.ram  = RamAllocator{*hook->free_ram_base, *hook->free_ram_length};
            ordered_hooks.push_back(hook->id);
            it = works.emplace(hook->id, std::move(w)).first;
        }
        HookWork &work = it->second;

        // One RAM slot per (hook, output_pin_name) pair. Two writes
        // to the same pin share the slot — the second emit overwrites
        // the first at runtime, which is the expected semantic.
        auto slot_it = work.pin_slots.find(ins.pin_name);
        std::uint32_t dst_addr = 0;
        if (slot_it == work.pin_slots.end()) {
            auto claim_r = work.ram.claim(4, 4);   // 4-byte longword
            if (!claim_r.has_value()) {
                std::string msg{"SH-2A backend: hook '"};
                msg.append(hook->id);
                msg.append("' free_ram exhausted while allocating "
                           "output slot for pin '");
                msg.append(ins.pin_name);
                msg.append("'");
                return failure(ErrorCode::OutOfRange, std::move(msg));
            }
            dst_addr = static_cast<std::uint32_t>(claim_r->address);
            work.claims.push_back(*claim_r);
            work.pin_slots.emplace(ins.pin_name, dst_addr);
        } else {
            dst_addr = slot_it->second;
        }

        // Encode the constant as a 32-bit signed value (Int slice).
        // Double → int32 cast is bounded by the IR's invariant that
        // an Int-typed LoadConstant's constant_value came from a
        // pin default that the editor/loader stored as a finite
        // integer-representable double; out-of-range values are a
        // pack/graph authoring bug we surface as ParseError so the
        // user fixes the .stmod, not the codegen.
        double const v = *src->constant_value;
        if (v < -2147483648.0 || v > 2147483647.0) {
            std::string msg{"SH-2A backend: LoadConstant value "};
            msg.append(std::to_string(v));
            msg.append(" out of int32 range");
            return failure(ErrorCode::ParseError, std::move(msg));
        }
        auto const i32 = static_cast<std::int32_t>(v);
        std::uint32_t u32 = 0;
        std::memcpy(&u32, &i32, sizeof u32);

        emit_load_const_store_sequence(work.code, u32, dst_addr);
    }

    // Build the PatchObject. Empty Module → empty hooks list, still
    // Ok — the patch-insertion layer treats an empty PatchObject as
    // a no-op flash.
    PatchObject obj{};
    obj.arch = Arch::Sh2a;
    obj.hooks.reserve(ordered_hooks.size());
    for (auto const &id : ordered_hooks) {
        auto const &w = works.at(id);
        HookPatch   hp{};
        hp.symbol         = w.hook->id;
        hp.splice_address = *w.hook->ecu_address;
        hp.code           = w.code;
        hp.ram_claims     = w.claims;
        obj.hooks.push_back(std::move(hp));
    }
    return obj;
}

Result<PatchObject> Rh850Backend::compile(ir::Module const & /*m*/,
                                           Definition const & /*def*/) {
    return failure(ErrorCode::NotImplemented,
                   "RH850 backend: instruction emission not yet implemented");
}

// ---- Backend selection --------------------------------------------------

namespace {

[[nodiscard]] bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char const ca = a[i] >= 'a' && a[i] <= 'z'
                            ? static_cast<char>(a[i] - 32)
                            : a[i];
        char const cb = b[i] >= 'a' && b[i] <= 'z'
                            ? static_cast<char>(b[i] - 32)
                            : b[i];
        if (ca != cb) return false;
    }
    return true;
}

} // namespace

Result<std::unique_ptr<IBackend>> select_backend(std::string_view platform) {
    if (ieq(platform, "VA")) {
        return std::unique_ptr<IBackend>{new Sh2aBackend{}};
    }
    if (ieq(platform, "VB")) {
        return std::unique_ptr<IBackend>{new Rh850Backend{}};
    }
    std::string msg{"select_backend: no codegen backend for platform '"};
    msg.append(platform);
    msg.append("' (recognized: VA → sh2a, VB → rh850)");
    return failure(ErrorCode::UnsupportedVersion, std::move(msg));
}

Result<std::unique_ptr<IBackend>> select_backend(Definition const &def) {
    return select_backend(def.pack().platform);
}

} // namespace st::feature::codegen
