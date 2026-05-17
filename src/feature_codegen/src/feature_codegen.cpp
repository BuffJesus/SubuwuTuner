// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_codegen.hpp"

#include "sh2a.hpp"
#include "st/core/error.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
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

// FragmentEmitter — incremental SH-2A patch builder. Body fragments
// register their PC-relative literals into a shared deferred pool
// and emit MOV.L @(d, PC), Rn instructions with placeholder disp
// bytes; finalize() appends RTS + NOP delay slot + pool-alignment
// padding + the consolidated literal pool, and back-patches every
// MOV.L's 8-bit disp once the literal pool's final offset is known.
//
// Also supports forward-reference branches via Label tokens. BT/BRA
// are emitted with placeholder disp bytes; place_label() marks the
// target offset within the body; finalize() re-encodes each branch
// with the computed PC-relative disp.
//
// This lets single-fragment patches (LoadConst→Store etc.) and
// multi-fragment patches (nested add_int trees) share the same
// epilogue + pool machinery without each emit site re-deriving
// disp values manually. select uses BT/BRA backpatching for its
// then/else control flow.
class FragmentEmitter {
  public:
    // Opaque label token for forward-reference branches. Returned by
    // create_label(), placed via place_label(), referenced by bt() /
    // bra(). Multiple branches may target the same label.
    struct Label {
        std::size_t id;
    };

    [[nodiscard]] Label create_label() {
        labels_.push_back(std::nullopt);
        return Label{labels_.size() - 1};
    }

    // Mark `label` at the current body offset. Must be called before
    // finalize().
    void place_label(Label const &label) {
        labels_[label.id] = body_.size();
    }

    // MOV.L @(d, PC), Rn — disp filled in at finalize() time.
    void mov_l_disp_pc(sh2a::Reg rn, std::uint32_t pool_literal) {
        backpatches_.push_back({body_.size(), pool_.size()});
        pool_.push_back(pool_literal);
        emit_be16(body_, sh2a::enc_mov_l_disp_pc(rn, 0));
    }
    void mov_l_at_reg_reg(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_mov_l_at_reg_reg(rm, rn));
    }
    void mov_l_reg_at_reg(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_mov_l_reg_at_reg(rm, rn));
    }
    void add(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_add(rm, rn));
    }
    void sub(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_sub(rm, rn));
    }
    void mul_l(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_mul_l(rm, rn));
    }
    void sts_macl(sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_sts_macl(rn));
    }
    void cmp_eq(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_cmp_eq(rm, rn));
    }
    void cmp_gt(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_cmp_gt(rm, rn));
    }
    void movt(sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_movt(rn));
    }
    // Trailing underscore on `and_`/`or_` because `and`/`or` are
    // alternate spellings of the boolean operators in C++.
    void and_(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_and(rm, rn));
    }
    void or_(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_or(rm, rn));
    }
    void tst(sh2a::Reg rm, sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_tst(rm, rn));
    }
    // BT label — branch if T=1. Placeholder bytes emitted now; disp
    // back-patched at finalize() once the label is known.
    void bt(Label const &target) {
        branch_backpatches_.push_back(
            {body_.size(), target.id, BranchKind::Bt});
        emit_be16(body_, sh2a::enc_bt(0));
    }
    // BRA label — unconditional branch (delay slot follows). Same
    // backpatch semantics as bt().
    void bra(Label const &target) {
        branch_backpatches_.push_back(
            {body_.size(), target.id, BranchKind::Bra});
        emit_be16(body_, sh2a::enc_bra(0));
    }
    void nop() { emit_be16(body_, sh2a::enc_nop()); }

    // Append RTS + delay-slot NOP + pool-alignment NOP(s) + literal
    // pool. Patch each MOV.L's disp byte and each branch's disp
    // bytes. Returns the finished byte buffer; the emitter is
    // single-use after this call.
    std::vector<std::uint8_t> finalize() {
        // Branches must be resolved before we append RTS/pool —
        // their targets are body offsets, which are stable across
        // the epilogue/pool append. Re-encode each branch with the
        // computed PC-relative disp (in 16-bit-word units).
        for (auto const &bp : branch_backpatches_) {
            auto const target_opt = labels_[bp.label_id];
            // target_opt is required to be set by emit time —
            // unplaced labels are a codegen bug. Refuse rather than
            // emit garbage.
            std::size_t const target = target_opt.value();
            // PC-relative formula: target = (this_pc + 4) + disp*2.
            // disp_words = (target - this_pc - 4) / 2.
            std::int32_t const disp_words =
                (static_cast<std::int32_t>(target)
                 - static_cast<std::int32_t>(bp.body_offset) - 4)
                / 2;
            std::uint16_t enc = 0;
            if (bp.kind == BranchKind::Bt) {
                enc = sh2a::enc_bt(static_cast<std::int8_t>(disp_words));
            } else {
                enc = sh2a::enc_bra(static_cast<std::int16_t>(disp_words));
            }
            body_[bp.body_offset]     = static_cast<std::uint8_t>(enc >> 8);
            body_[bp.body_offset + 1] = static_cast<std::uint8_t>(enc & 0xFF);
        }

        emit_be16(body_, sh2a::enc_rts());
        emit_be16(body_, sh2a::enc_nop());  // delay slot
        while (body_.size() % 4 != 0) {
            emit_be16(body_, sh2a::enc_nop());  // pool-alignment pad
        }
        std::size_t const pool_start = body_.size();
        for (auto const &bp : backpatches_) {
            std::size_t const literal_offset =
                pool_start + bp.pool_index * 4;
            std::size_t const aligned =
                (bp.body_offset + 4) & ~std::size_t{3};
            std::size_t const disp = (literal_offset - aligned) / 4;
            // The disp lives in the low byte of the 16-bit MOV.L
            // instruction; body_[bp.body_offset] is the high byte
            // (0xD0 | Rn), body_[bp.body_offset + 1] is the disp.
            body_[bp.body_offset + 1] = static_cast<std::uint8_t>(disp);
        }
        for (auto val : pool_) {
            emit_be32(body_, val);
        }
        return std::move(body_);
    }

    [[nodiscard]] std::size_t body_size() const noexcept {
        return body_.size();
    }

  private:
    struct Backpatch {
        std::size_t body_offset;  // byte offset of the MOV.L instruction
        std::size_t pool_index;   // index into `pool_`
    };
    enum class BranchKind : std::uint8_t { Bt, Bra };
    struct BranchBackpatch {
        std::size_t body_offset;  // byte offset of the branch instruction
        std::size_t label_id;     // index into `labels_`
        BranchKind  kind;
    };
    std::vector<std::uint8_t>                 body_;
    std::vector<std::uint32_t>                pool_;
    std::vector<Backpatch>                    backpatches_;
    std::vector<std::optional<std::size_t>>   labels_;
    std::vector<BranchBackpatch>              branch_backpatches_;
};

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
    FragmentEmitter fe;
    fe.mov_l_disp_pc(sh2a::Reg::R0, constant_value);
    fe.mov_l_disp_pc(sh2a::Reg::R1, destination_address);
    fe.mov_l_reg_at_reg(sh2a::Reg::R0, sh2a::Reg::R1);
    auto bytes = fe.finalize();
    code.insert(code.end(), bytes.begin(), bytes.end());
}

// Emit the "load from hook-input address, dereference, store to RAM
// slot" sequence. source_address holds the firmware variable backing
// a hook-input pin; destination_address is the RAM slot allocated for
// the hook-output pin the Store targets.
//
//   offset  bytes        instruction
//   0       D002         MOV.L @(2, PC), R0  ; R0 = source pointer
//   2       6002         MOV.L @R0, R0       ; R0 = mem[R0] (the value)
//   4       D102         MOV.L @(2, PC), R1  ; R1 = destination pointer
//   6       2102         MOV.L R0, @R1       ; mem[R1] = R0
//   8       000B         RTS
//   10      0009         NOP                 ; delay slot
//   12      <src>        literal: source address (big-endian)
//   16      <dst>        literal: destination address (big-endian)
//
// 6 instructions × 2 bytes = 12 bytes, which is already 4-aligned, so
// no padding NOP is needed (unlike the const→store shape).
//
// PC-relative formula: target = ((this_pc + 4) & ~3) + disp*4.
//   PC=0: (4 & ~3) = 4; for target 12, disp = (12-4)/4 = 2.
//   PC=4: (8 & ~3) = 8; for target 16, disp = (16-8)/4 = 2.
void emit_load_hook_store_sequence(std::vector<std::uint8_t> &code,
                                    std::uint32_t source_address,
                                    std::uint32_t destination_address) {
    FragmentEmitter fe;
    fe.mov_l_disp_pc(sh2a::Reg::R0, source_address);
    fe.mov_l_at_reg_reg(sh2a::Reg::R0, sh2a::Reg::R0);
    fe.mov_l_disp_pc(sh2a::Reg::R1, destination_address);
    fe.mov_l_reg_at_reg(sh2a::Reg::R0, sh2a::Reg::R1);
    auto bytes = fe.finalize();
    code.insert(code.end(), bytes.begin(), bytes.end());
}

// One operand of an ADD-style binary primitive. Either an immediate
// constant from a LoadConstant, or a firmware-address pointer from a
// LoadHookInput. The emitter dereferences the latter inline.
struct PrimitiveOperand {
    enum class Kind : std::uint8_t { Constant, HookInputPointer };
    Kind          kind{Kind::Constant};
    std::uint32_t value{0};  // constant value, or pin address
};

// Emit a self-contained "compute add, store result" patch. The
// emitted shape varies in size (28 or 32 bytes) depending on whether
// each operand needs an indirect deref:
//
//   1 instr per Constant operand  (MOV.L @(d, PC), Rn)
//   2 instr per HookInput operand (MOV.L @(d, PC), Rn; MOV.L @Rn, Rn)
//   1 instr ADD R0, R1
//   1 instr MOV.L @(d, PC), R2  (destination pointer)
//   1 instr MOV.L R1, @R2       (store)
//   1 instr RTS
//   1 instr NOP (delay slot)
//   0..1 instr NOP padding to 4-byte-align the literal pool
//   3 longwords pool: operand1 datum, operand2 datum, destination addr
//
// Disp values are computed per the SH-2A PC-relative formula:
//   target = ((this_pc + 4) & ~3) + disp*4
// which means a MOV.L at offset N reading from pool offset P uses
// disp = (P - ((N + 4) & ~3)) / 4.
// Load an operand into the given register, dereferencing through a
// pointer if the operand is a HookInputPointer (or SSA RAM slot).
// Shared by all binary-primitive fragment emitters.
void load_operand_into(FragmentEmitter &fe, PrimitiveOperand op,
                        sh2a::Reg reg) {
    fe.mov_l_disp_pc(reg, op.value);
    if (op.kind == PrimitiveOperand::Kind::HookInputPointer) {
        fe.mov_l_at_reg_reg(reg, reg);
    }
}

// Store the convention-result register (R1) to `destination_address`.
// Used as the tail of every binary-primitive fragment.
void emit_store_r1_to(FragmentEmitter &fe,
                       std::uint32_t destination_address) {
    fe.mov_l_disp_pc(sh2a::Reg::R2, destination_address);
    fe.mov_l_reg_at_reg(sh2a::Reg::R1, sh2a::Reg::R2);
}

// Body fragment for `add_int(op1, op2)` → store. ADD is commutative
// so the operand-to-register mapping is the natural one (op1 → R0,
// op2 → R1; ADD R0, R1 leaves the sum in R1).
void emit_add_fragment(FragmentEmitter &fe,
                        PrimitiveOperand op1,
                        PrimitiveOperand op2,
                        std::uint32_t    destination_address) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.add(sh2a::Reg::R0, sh2a::Reg::R1);
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `subtract_int(op1, op2)` → store. SUB is NOT
// commutative — the user expects `op1 - op2`. SH-2A `SUB Rm, Rn`
// computes Rn = Rn - Rm, so we put the minuend (op1) in R1 and the
// subtrahend (op2) in R0; the result lands in R1 by convention.
void emit_sub_fragment(FragmentEmitter &fe,
                        PrimitiveOperand op1,
                        PrimitiveOperand op2,
                        std::uint32_t    destination_address) {
    load_operand_into(fe, op2, sh2a::Reg::R0);  // subtrahend
    load_operand_into(fe, op1, sh2a::Reg::R1);  // minuend
    fe.sub(sh2a::Reg::R0, sh2a::Reg::R1);       // R1 = R1 - R0 = op1 - op2
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `multiply_int(op1, op2)` → store. MUL.L is
// commutative on the values but writes its result to the MACL system
// register, NOT a general-purpose register; we follow up with
// STS MACL, R1 to extract the low 32 bits. Truncating to int32
// (matching int32 + int32 → int32 semantics; high 32 bits in MACH
// are discarded — overflow wraps two's-complement, same as add).
void emit_mul_fragment(FragmentEmitter &fe,
                        PrimitiveOperand op1,
                        PrimitiveOperand op2,
                        std::uint32_t    destination_address) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.mul_l(sh2a::Reg::R0, sh2a::Reg::R1);     // MACL = R0 * R1
    fe.sts_macl(sh2a::Reg::R1);                  // R1 = MACL
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `and_bool(op1, op2)` → store. AND is commutative
// so the natural mapping works (op1 → R0, op2 → R1; AND R0, R1
// leaves the bitwise-AND in R1). On canonical 0/1 operands the bits
// align so the result is canonical 0/1 logical AND.
void emit_and_fragment(FragmentEmitter &fe, PrimitiveOperand op1,
                        PrimitiveOperand op2, std::uint32_t dst) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.and_(sh2a::Reg::R0, sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Body fragment for `or_bool(op1, op2)` → store. Same shape as
// and_bool with the OR opcode.
void emit_or_fragment(FragmentEmitter &fe, PrimitiveOperand op1,
                       PrimitiveOperand op2, std::uint32_t dst) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.or_(sh2a::Reg::R0, sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Body fragment for `not_bool(op)` → store. Unary primitive — only
// one operand. TST Rn, Rn sets T iff Rn==0; MOVT R1 then materializes
// that as 1 (when op was 0) or 0 (when op was non-zero). One
// instruction shorter than going through a constant + XOR.
void emit_not_fragment(FragmentEmitter &fe, PrimitiveOperand op,
                        std::uint32_t dst) {
    load_operand_into(fe, op, sh2a::Reg::R0);
    fe.tst(sh2a::Reg::R0, sh2a::Reg::R0);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Body fragment for `select_int(cond, true_val, false_val)` → store.
// First primitive that needs control flow. Layout:
//
//   load cond → R0
//   TST R0, R0           ; T = 1 if cond == 0 (false)
//   BT use_false         ; if T, jump to false branch (no delay slot)
//   load true_val → R1
//   BRA done             ; jump over the false branch (has delay slot)
//   NOP                  ; delay slot
//   use_false:
//   load false_val → R1
//   done:
//   load dest → R2
//   MOV.L R1, @R2
//
// Both branch disp values are back-patched by FragmentEmitter::finalize
// once the body offsets are known. Result type for select is the
// shared type of true_val/false_val (Int in this slice).
void emit_select_int_fragment(FragmentEmitter &fe,
                               PrimitiveOperand cond,
                               PrimitiveOperand true_val,
                               PrimitiveOperand false_val,
                               std::uint32_t    dst) {
    auto use_false = fe.create_label();
    auto done      = fe.create_label();

    load_operand_into(fe, cond, sh2a::Reg::R0);
    fe.tst(sh2a::Reg::R0, sh2a::Reg::R0);
    fe.bt(use_false);
    load_operand_into(fe, true_val, sh2a::Reg::R1);
    fe.bra(done);
    fe.nop();                                  // BRA delay slot
    fe.place_label(use_false);
    load_operand_into(fe, false_val, sh2a::Reg::R1);
    fe.place_label(done);
    emit_store_r1_to(fe, dst);
}

// Body fragment for a comparison primitive. All three variants
// (compare_lt / compare_gt / compare_eq) share the same shape:
// load operands → CMP/X (sets T-bit) → MOVT R1 (R1 = T as 0/1) →
// store R1. The only thing that differs is the CMP opcode + the
// operand-to-register mapping. Bool widening = canonical 0/1.
void emit_cmp_lt_fragment(FragmentEmitter &fe, PrimitiveOperand op1,
                           PrimitiveOperand op2, std::uint32_t dst) {
    // T = (op1 < op2) = (op2 > op1) = (R1 > R0) ⇒ CMP/GT R0, R1.
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.cmp_gt(sh2a::Reg::R0, sh2a::Reg::R1);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

void emit_cmp_gt_fragment(FragmentEmitter &fe, PrimitiveOperand op1,
                           PrimitiveOperand op2, std::uint32_t dst) {
    // T = (op1 > op2) = (R0 > R1) ⇒ CMP/GT R1, R0.
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.cmp_gt(sh2a::Reg::R1, sh2a::Reg::R0);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

void emit_cmp_eq_fragment(FragmentEmitter &fe, PrimitiveOperand op1,
                           PrimitiveOperand op2, std::uint32_t dst) {
    // T = (op1 == op2). CMP/EQ is commutative; load order doesn't
    // affect correctness, only the operand-naming consistency.
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.cmp_eq(sh2a::Reg::R0, sh2a::Reg::R1);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Dispatch a CallPrimitive instruction to the right fragment emitter
// by symbol. Both the single-level and nested-emit paths route
// through this, so adding a new primitive only needs a new
// emit_X_fragment plus a clause here. Operand count is validated by
// validate_call_primitive before we get here, so each branch can
// index `operands` confidently.
[[nodiscard]] Status emit_primitive_fragment(
    FragmentEmitter &fe, std::string_view symbol,
    std::vector<PrimitiveOperand> const &operands, std::uint32_t dst) {
    if (symbol == "add_int") {
        emit_add_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "subtract_int") {
        emit_sub_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "multiply_int") {
        emit_mul_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_lt") {
        emit_cmp_lt_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_gt") {
        emit_cmp_gt_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_eq") {
        emit_cmp_eq_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "and_bool") {
        emit_and_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "or_bool") {
        emit_or_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "not_bool") {
        emit_not_fragment(fe, operands[0], dst);
        return ok();
    }
    if (symbol == "select_int") {
        emit_select_int_fragment(fe, operands[0], operands[1],
                                  operands[2], dst);
        return ok();
    }
    std::string msg{"SH-2A backend: CallPrimitive '"};
    msg.append(symbol);
    msg.append("' not yet implemented (slice supports add_int, "
               "subtract_int, multiply_int, compare_lt, compare_gt, "
               "compare_eq, and_bool, or_bool, not_bool, select_int)");
    return failure(ErrorCode::NotImplemented, std::move(msg));
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

// Look up a hook's input pin by name (matches a `HookSignal` in the
// hook's `inputs` array — the data-out-from-ECU side; not the
// graph-side "input pin" of the user's logic). Returns nullptr if
// the hook doesn't declare that pin.
[[nodiscard]] HookSignal const *find_hook_input(Hook const &h,
                                                 std::string_view name) noexcept {
    for (auto const &s : h.inputs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// Resolve a LoadHookInput instruction's source pin to its firmware
// address. Encapsulates the four error paths shared by direct
// LoadHookInput→Store chains and CallPrimitive operands: hook
// missing, hook != Store's hook, pin missing on the hook, pin has
// no `address`. Returns the address in `out` on success.
[[nodiscard]] Status resolve_hook_input_address(
    Definition const &def, ir::Instruction const &load_ins,
    Hook const *target_hook, std::uint32_t &out) {
    Hook const *src_hook = find_hook(def, load_ins.symbol);
    if (src_hook == nullptr) {
        std::string msg{"SH-2A backend: LoadHookInput hook '"};
        msg.append(load_ins.symbol);
        msg.append("' not declared in the loaded definition pack");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    if (src_hook != target_hook) {
        std::string msg{"SH-2A backend: cross-hook value flow from '"};
        msg.append(src_hook->id);
        msg.append("' to '");
        msg.append(target_hook->id);
        msg.append("' not yet implemented (slice requires Store and its "
                   "source LoadHookInput on the same hook)");
        return failure(ErrorCode::NotImplemented, std::move(msg));
    }
    HookSignal const *pin = find_hook_input(*src_hook, load_ins.pin_name);
    if (pin == nullptr) {
        std::string msg{"SH-2A backend: hook '"};
        msg.append(src_hook->id);
        msg.append("' does not declare input pin '");
        msg.append(load_ins.pin_name);
        msg.append("'");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    if (!pin->address.has_value()) {
        std::string msg{"SH-2A backend: hook input '"};
        msg.append(src_hook->id);
        msg.append(".");
        msg.append(pin->name);
        msg.append("' has no address — pack must declare the firmware "
                   "address backing this signal for codegen");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    out = static_cast<std::uint32_t>(*pin->address);
    return ok();
}

// Coerce a LoadConstant's stored double to the 32-bit machine word
// that the SH-2A literal pool holds. Type-dispatched:
//
//   Int   — range-check, two's-complement int32 encoding.
//   Float — narrow to IEEE 754 binary32, bit_cast to uint32. Both
//           the precision narrowing and any out-of-range values
//           (Inf, NaN) propagate per the standard cast rules; the
//           pack/graph author is consenting to single-precision by
//           authoring a float pin.
//   Bool  — NotImplemented; widening (0/1 vs other booleans) is a
//           policy decision that lands with a future bundle.
[[nodiscard]] Result<std::uint32_t> coerce_constant_to_u32(
    ir::Instruction const &load_ins) {
    if (!load_ins.constant_value.has_value()) {
        return failure(ErrorCode::ParseError,
                       "SH-2A backend: LoadConstant has no "
                       "constant_value (lower() invariant violation)");
    }
    double const v = *load_ins.constant_value;
    switch (load_ins.result_type) {
        case PinType::Int: {
            if (v < -2147483648.0 || v > 2147483647.0) {
                std::string msg{"SH-2A backend: LoadConstant value "};
                msg.append(std::to_string(v));
                msg.append(" out of int32 range");
                return failure(ErrorCode::ParseError, std::move(msg));
            }
            auto const i32 = static_cast<std::int32_t>(v);
            std::uint32_t u32 = 0;
            std::memcpy(&u32, &i32, sizeof u32);
            return u32;
        }
        case PinType::Float: {
            auto const f = static_cast<float>(v);
            return std::bit_cast<std::uint32_t>(f);
        }
        case PinType::Bool:
            // Canonical Bool widening: 0 = false, 1 = true in a
            // 32-bit word. Matches the MOVT output of comparison
            // primitives, so a Bool value flowing through registers
            // / memory keeps a uniform representation regardless of
            // whether it came from a LoadConstant or a compare_*.
            return v > 0.5 ? std::uint32_t{1} : std::uint32_t{0};
    }
    return failure(ErrorCode::NotImplemented,
                   "SH-2A backend: unknown PinType for LoadConstant");
}

// Locate the producing Instruction for `value_id`. Returns nullptr
// if no instruction in the module produces it. Caller is responsible
// for verifying the op kind is one the slice supports.
[[nodiscard]] ir::Instruction const *find_producer(ir::Module const &m,
                                                    ir::ValueId value_id) noexcept {
    for (auto const &ins : m.instructions) {
        if (ins.result_id == value_id) return &ins;
    }
    return nullptr;
}

// Build a PrimitiveOperand from a producing instruction. Checks that
// the producer's result_type matches the primitive's declared
// operand_type (e.g. add_int requires Int operands, and_bool requires
// Bool operands); cross-hook flow rejected; pre-allocated SSA RAM
// slots for nested CallPrimitive results read from `slots`.
[[nodiscard]] Result<PrimitiveOperand> operand_from_producer(
    Definition const &def, ir::Module const &m, ir::ValueId value_id,
    Hook const *target_hook, PinType expected_operand_type,
    std::unordered_map<ir::ValueId, std::uint32_t> const &slots) {
    ir::Instruction const *prod = find_producer(m, value_id);
    if (prod == nullptr) {
        return failure(ErrorCode::ParseError,
                       "SH-2A backend: primitive operand does not "
                       "resolve to any producing instruction");
    }
    if (prod->result_type != expected_operand_type) {
        std::string msg{"SH-2A backend: primitive operand of type "};
        msg.append(pin_type_name(prod->result_type));
        msg.append(" does not match expected operand type ");
        msg.append(pin_type_name(expected_operand_type));
        return failure(ErrorCode::NotImplemented, std::move(msg));
    }
    if (prod->op == ir::Op::LoadConstant) {
        auto r = coerce_constant_to_u32(*prod);
        if (!r.has_value()) return failure(r.error());
        return PrimitiveOperand{PrimitiveOperand::Kind::Constant, *r};
    }
    if (prod->op == ir::Op::LoadHookInput) {
        std::uint32_t addr = 0;
        if (auto s = resolve_hook_input_address(def, *prod, target_hook, addr);
            !s.has_value()) {
            return failure(s.error());
        }
        return PrimitiveOperand{PrimitiveOperand::Kind::HookInputPointer,
                                  addr};
    }
    if (prod->op == ir::Op::CallPrimitive) {
        // Read the operand value from the slot allocated for this
        // nested primitive's result. Mechanically identical to
        // HookInputPointer at the SH-2A level (load address, deref);
        // semantically distinct (firmware variable vs scratch RAM slot).
        auto it = slots.find(value_id);
        if (it == slots.end()) {
            return failure(ErrorCode::ParseError,
                           "SH-2A backend: nested primitive operand has "
                           "no RAM slot (walk should have allocated one)");
        }
        return PrimitiveOperand{PrimitiveOperand::Kind::HookInputPointer,
                                  it->second};
    }
    return failure(ErrorCode::NotImplemented,
                   "SH-2A backend: primitive operand has unsupported "
                   "producer op");
}

// Per-primitive shape: how many operands it takes, what type each
// operand must be, and what result type it produces. The codegen
// rejects mismatches between this table and the IR's actual
// instruction. operand_types holds 3 slots; only the first `arity`
// are read. The 3-slot upper bound matches the widest primitive
// (select), so adding new ternary primitives doesn't require
// growing the array.
struct PrimitiveShape {
    std::size_t            arity;
    std::array<PinType, 3> operand_types;
    PinType                result_type;
};

[[nodiscard]] PrimitiveShape const *primitive_shape(
    std::string_view symbol) noexcept {
    // Arithmetic:    2 Int → Int.
    // Comparison:    2 Int → Bool.
    // Boolean logic: 2 (or 1) Bool → Bool.
    // Select:        3 operands — first is Bool (condition); other
    //                two share the result type.
    // Unused trailing slots are filled with the prior slot's type;
    // they're never read at arity-checked dispatch time.
    static constexpr struct Entry {
        std::string_view name;
        PrimitiveShape   shape;
    } kTable[] = {
        {"add_int",      {2, {PinType::Int,  PinType::Int,  PinType::Int},  PinType::Int}},
        {"subtract_int", {2, {PinType::Int,  PinType::Int,  PinType::Int},  PinType::Int}},
        {"multiply_int", {2, {PinType::Int,  PinType::Int,  PinType::Int},  PinType::Int}},
        {"compare_lt",   {2, {PinType::Int,  PinType::Int,  PinType::Int},  PinType::Bool}},
        {"compare_gt",   {2, {PinType::Int,  PinType::Int,  PinType::Int},  PinType::Bool}},
        {"compare_eq",   {2, {PinType::Int,  PinType::Int,  PinType::Int},  PinType::Bool}},
        {"and_bool",     {2, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"or_bool",      {2, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"not_bool",     {1, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"select_int",   {3, {PinType::Bool, PinType::Int,  PinType::Int},  PinType::Int}},
    };
    for (auto const &e : kTable) {
        if (e.name == symbol) return &e.shape;
    }
    return nullptr;
}

// Validate a CallPrimitive at structural level: id is recognized,
// operand count matches the primitive's arity, result type matches
// the primitive's declared shape. Used by both the topo walk and
// the per-primitive emit.
[[nodiscard]] Status validate_call_primitive(ir::Instruction const &prim) {
    PrimitiveShape const *shape = primitive_shape(prim.symbol);
    if (shape == nullptr) {
        std::string msg{"SH-2A backend: CallPrimitive '"};
        msg.append(prim.symbol);
        msg.append("' not yet implemented (slice supports add_int, "
                   "subtract_int, multiply_int, compare_lt, "
                   "compare_gt, compare_eq, and_bool, or_bool, "
                   "not_bool, select_int)");
        return failure(ErrorCode::NotImplemented, std::move(msg));
    }
    if (prim.operands.size() != shape->arity) {
        std::string msg{"SH-2A backend: "};
        msg.append(prim.symbol);
        msg.append(" requires exactly ");
        msg.append(std::to_string(shape->arity));
        msg.append(" operands, got ");
        msg.append(std::to_string(prim.operands.size()));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    if (prim.result_type != shape->result_type) {
        std::string msg{"SH-2A backend: "};
        msg.append(prim.symbol);
        msg.append(" expects result type ");
        msg.append(pin_type_name(shape->result_type));
        msg.append(", got ");
        msg.append(pin_type_name(prim.result_type));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    return ok();
}

// Emit a Store rooted at a nested CallPrimitive tree. Walks the tree
// bottom-up: each interior primitive (non-root) gets a 4-byte RAM
// slot for its result, then is emitted as a body fragment that
// stores to that slot. The root primitive's body fragment stores to
// the Store's destination instead of a slot. All fragments share one
// FragmentEmitter, so the literal pool and RTS epilogue are
// consolidated.
//
// Supports arbitrary depth as long as every interior node is a
// recognized CallPrimitive (add_int in this slice). Leaves are
// LoadConstant or LoadHookInput, validated by operand_from_producer.
[[nodiscard]] Status emit_nested_call_primitive(
    Definition const &def, ir::Module const &m,
    ir::Instruction const &root_prim, Hook const *hook,
    RamAllocator &ram, std::vector<RamClaim> &claims,
    std::vector<std::uint8_t> &out_code, std::uint32_t dst_addr) {

    // Topological walk: recurse into CallPrimitive operands first so
    // intermediate slots exist by the time the consumer emits.
    std::unordered_map<ir::ValueId, std::uint32_t> slots;
    std::vector<ir::Instruction const *>           emit_order;

    auto const walk = [&](auto &self_ref,
                           ir::Instruction const *prim,
                           bool is_root) -> Status {
        if (auto s = validate_call_primitive(*prim); !s.has_value()) {
            return failure(s.error());
        }
        for (auto operand_id : prim->operands) {
            ir::Instruction const *prod = find_producer(m, operand_id);
            if (prod == nullptr) {
                return failure(ErrorCode::ParseError,
                               "SH-2A backend: nested operand does not "
                               "resolve to any producing instruction");
            }
            if (prod->op == ir::Op::CallPrimitive) {
                if (auto s = self_ref(self_ref, prod, /*is_root=*/false);
                    !s.has_value()) {
                    return failure(s.error());
                }
            }
        }
        if (!is_root) {
            auto claim_r = ram.claim(4, 4);
            if (!claim_r.has_value()) {
                std::string msg{"SH-2A backend: hook '"};
                msg.append(hook->id);
                msg.append("' free_ram exhausted while allocating slot "
                           "for nested primitive result");
                return failure(ErrorCode::OutOfRange, std::move(msg));
            }
            slots[prim->result_id] =
                static_cast<std::uint32_t>(claim_r->address);
            claims.push_back(*claim_r);
        }
        emit_order.push_back(prim);
        return ok();
    };
    if (auto s = walk(walk, &root_prim, /*is_root=*/true); !s.has_value()) {
        return failure(s.error());
    }

    FragmentEmitter fe;
    for (ir::Instruction const *prim : emit_order) {
        PrimitiveShape const *shape = primitive_shape(prim->symbol);
        if (shape == nullptr) {
            // Unreachable — validate_call_primitive checked earlier.
            return failure(ErrorCode::ParseError,
                           "SH-2A backend: primitive symbol lookup "
                           "regressed after validation");
        }
        std::vector<PrimitiveOperand> operands;
        operands.reserve(shape->arity);
        for (std::size_t i = 0; i < shape->arity; ++i) {
            auto op = operand_from_producer(
                def, m, prim->operands[i], hook,
                shape->operand_types[i], slots);
            if (!op.has_value()) return failure(op.error());
            operands.push_back(*op);
        }
        std::uint32_t const this_dest =
            (prim == &root_prim) ? dst_addr : slots.at(prim->result_id);
        if (auto s = emit_primitive_fragment(fe, prim->symbol, operands,
                                              this_dest);
            !s.has_value()) {
            return failure(s.error());
        }
    }
    auto bytes = fe.finalize();
    out_code.insert(out_code.end(), bytes.begin(), bytes.end());
    return ok();
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
    // First pass: every Op is now structurally supported (each one
    // has at least one emission path that goes through it). Per-op
    // semantic checks — supported primitive id, operand sources,
    // type compatibility — happen below at Store-emit time.
    for (auto const &ins : m.instructions) {
        switch (ins.op) {
            case ir::Op::LoadConstant:
            case ir::Op::LoadHookInput:
            case ir::Op::CallPrimitive:
            case ir::Op::StoreHookOutput:
                break;
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
        // Find the source instruction. A Store consuming a
        // non-existent ValueId is an IR invariant violation.
        ir::Instruction const *src = find_producer(m, ins.operands[0]);
        if (src == nullptr) {
            return failure(ErrorCode::ParseError,
                           "SH-2A backend: StoreHookOutput operand "
                           "does not resolve to any producing instruction "
                           "(invariant: lower() should assign result_id "
                           "to every value-producing op)");
        }

        // Slice rule: Store source must be LoadConstant, LoadHookInput,
        // or a supported CallPrimitive. Anything else (deeper nesting,
        // unsupported primitive kind) gets a specific NotImplemented.
        if (src->op != ir::Op::LoadConstant
            && src->op != ir::Op::LoadHookInput
            && src->op != ir::Op::CallPrimitive) {
            return failure(ErrorCode::NotImplemented,
                           "SH-2A backend: StoreHookOutput source must "
                           "be a LoadConstant, LoadHookInput, or "
                           "CallPrimitive in this slice");
        }

        // All three PinTypes (Int, Float, Bool) flow through the
        // same 4-byte MOV.L paths. The codegen doesn't reinterpret
        // the bytes — the firmware does. Bool widening (0/1
        // canonical) happens inside coerce_constant_to_u32 and is
        // produced by MOVT after comparison primitives, keeping the
        // representation uniform.
        if (src->op == ir::Op::LoadConstant
            && !src->constant_value.has_value()) {
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

        if (src->op == ir::Op::LoadConstant) {
            auto u32 = coerce_constant_to_u32(*src);
            if (!u32.has_value()) return failure(u32.error());
            emit_load_const_store_sequence(work.code, *u32, dst_addr);
        } else if (src->op == ir::Op::LoadHookInput) {
            std::uint32_t pin_addr = 0;
            if (auto s = resolve_hook_input_address(def, *src, hook, pin_addr);
                !s.has_value()) {
                return failure(s.error());
            }
            emit_load_hook_store_sequence(work.code, pin_addr, dst_addr);
        } else {
            // CallPrimitive. Structural validation (recognized symbol,
            // operand count, result type) is shared with the nested
            // path; emission is dispatched by symbol through
            // emit_primitive_fragment.
            if (auto s = validate_call_primitive(*src); !s.has_value()) {
                return failure(s.error());
            }
            // Detect nested CallPrimitive operands. Single-level
            // calls use the existing single-fragment emit; nested
            // trees go through the multi-fragment path that allocates
            // SSA RAM slots. Loop covers both unary (arity 1) and
            // binary (arity 2) primitives.
            auto const producer_is_primitive = [&](ir::ValueId vid) {
                ir::Instruction const *prod = find_producer(m, vid);
                return prod != nullptr && prod->op == ir::Op::CallPrimitive;
            };
            bool nested = false;
            for (auto operand_id : src->operands) {
                if (producer_is_primitive(operand_id)) {
                    nested = true;
                    break;
                }
            }
            if (nested) {
                if (auto s = emit_nested_call_primitive(
                        def, m, *src, hook, work.ram,
                        work.claims, work.code, dst_addr);
                    !s.has_value()) {
                    return failure(s.error());
                }
            } else {
                std::unordered_map<ir::ValueId, std::uint32_t> empty_slots;
                PrimitiveShape const *shape = primitive_shape(src->symbol);
                if (shape == nullptr) {
                    // Unreachable — validate_call_primitive checked earlier.
                    return failure(ErrorCode::ParseError,
                                   "SH-2A backend: primitive symbol "
                                   "lookup regressed after validation");
                }
                std::vector<PrimitiveOperand> operands;
                operands.reserve(shape->arity);
                for (std::size_t i = 0; i < shape->arity; ++i) {
                    auto op = operand_from_producer(
                        def, m, src->operands[i], hook,
                        shape->operand_types[i], empty_slots);
                    if (!op.has_value()) return failure(op.error());
                    operands.push_back(*op);
                }
                FragmentEmitter fe;
                if (auto s = emit_primitive_fragment(fe, src->symbol,
                                                      operands, dst_addr);
                    !s.has_value()) {
                    return failure(s.error());
                }
                auto bytes = fe.finalize();
                work.code.insert(work.code.end(), bytes.begin(), bytes.end());
            }
        }
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
