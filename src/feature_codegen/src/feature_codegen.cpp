// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_codegen.hpp"

#include "st/core/error.hpp"

#include "rh850.hpp"
#include "sh2a.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace st::feature::codegen {

namespace {

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

} // namespace

char const *arch_name(Arch a) noexcept {
    switch (a) {
    case Arch::Sh2a:
        return "sh2a";
    case Arch::Rh850:
        return "rh850";
    case Arch::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string_view arch_name_sv(Arch a) noexcept {
    return arch_name(a);
}

// ---- RamAllocator -------------------------------------------------------

RamAllocator::RamAllocator(std::size_t base, std::size_t length) noexcept
    : base_(base), length_(length), cursor_(base) {}

Result<RamClaim> RamAllocator::claim(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) {
        return failure(ErrorCode::InvalidArgument, "RamAllocator::claim: size must be > 0");
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
    std::size_t const mask = align - 1U;
    std::size_t const slack = (align - (cursor_ & mask)) & mask;
    if (slack > length_ || cursor_ + slack < cursor_) {
        return failure(ErrorCode::OutOfRange, "RamAllocator::claim: alignment overflow");
    }
    std::size_t const aligned = cursor_ + slack;

    // Bounds check the claim itself.
    if (aligned + size < aligned) {
        return failure(ErrorCode::OutOfRange, "RamAllocator::claim: size overflow");
    }
    if (aligned + size > base_ + length_) {
        return failure(ErrorCode::OutOfRange, "RamAllocator::claim: region exhausted");
    }

    RamClaim out{};
    out.address = aligned;
    out.size = size;
    out.alignment = align;
    cursor_ = aligned + size;
    return out;
}

void RamAllocator::reset() noexcept {
    cursor_ = base_;
}

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
        branch_backpatches_.push_back({body_.size(), target.id, BranchKind::Bt});
        emit_be16(body_, sh2a::enc_bt(0));
    }
    // BRA label — unconditional branch (delay slot follows). Same
    // backpatch semantics as bt().
    void bra(Label const &target) {
        branch_backpatches_.push_back({body_.size(), target.id, BranchKind::Bra});
        emit_be16(body_, sh2a::enc_bra(0));
    }
    void nop() {
        emit_be16(body_, sh2a::enc_nop());
    }
    void fadd(sh2a::FReg frm, sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fadd(frm, frn));
    }
    void fsub(sh2a::FReg frm, sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fsub(frm, frn));
    }
    void fmul(sh2a::FReg frm, sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fmul(frm, frn));
    }
    void fdiv(sh2a::FReg frm, sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fdiv(frm, frn));
    }
    void fsqrt(sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fsqrt(frn));
    }
    void lds_r_fpul(sh2a::Reg rm) {
        emit_be16(body_, sh2a::enc_lds_r_fpul(rm));
    }
    void sts_fpul_r(sh2a::Reg rn) {
        emit_be16(body_, sh2a::enc_sts_fpul_r(rn));
    }
    void fsts_fpul_freg(sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fsts_fpul_freg(frn));
    }
    void flds_freg_fpul(sh2a::FReg frm) {
        emit_be16(body_, sh2a::enc_flds_freg_fpul(frm));
    }
    void float_fpul_freg(sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_float_fpul_freg(frn));
    }
    void ftrc_freg_fpul(sh2a::FReg frm) {
        emit_be16(body_, sh2a::enc_ftrc_freg_fpul(frm));
    }
    void fcmp_eq(sh2a::FReg frm, sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fcmp_eq(frm, frn));
    }
    void fcmp_gt(sh2a::FReg frm, sh2a::FReg frn) {
        emit_be16(body_, sh2a::enc_fcmp_gt(frm, frn));
    }

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
            std::int32_t const disp_words = (static_cast<std::int32_t>(target) -
                                             static_cast<std::int32_t>(bp.body_offset) - 4) /
                                            2;
            std::uint16_t enc = 0;
            if (bp.kind == BranchKind::Bt) {
                enc = sh2a::enc_bt(static_cast<std::int8_t>(disp_words));
            } else {
                enc = sh2a::enc_bra(static_cast<std::int16_t>(disp_words));
            }
            body_[bp.body_offset] = static_cast<std::uint8_t>(enc >> 8);
            body_[bp.body_offset + 1] = static_cast<std::uint8_t>(enc & 0xFF);
        }

        emit_be16(body_, sh2a::enc_rts());
        emit_be16(body_, sh2a::enc_nop()); // delay slot
        while (body_.size() % 4 != 0) {
            emit_be16(body_, sh2a::enc_nop()); // pool-alignment pad
        }
        std::size_t const pool_start = body_.size();
        for (auto const &bp : backpatches_) {
            std::size_t const literal_offset = pool_start + bp.pool_index * 4;
            std::size_t const aligned = (bp.body_offset + 4) & ~std::size_t{3};
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
        std::size_t body_offset; // byte offset of the MOV.L instruction
        std::size_t pool_index;  // index into `pool_`
    };
    enum class BranchKind : std::uint8_t { Bt, Bra };
    struct BranchBackpatch {
        std::size_t body_offset; // byte offset of the branch instruction
        std::size_t label_id;    // index into `labels_`
        BranchKind kind;
    };
    std::vector<std::uint8_t> body_;
    std::vector<std::uint32_t> pool_;
    std::vector<Backpatch> backpatches_;
    std::vector<std::optional<std::size_t>> labels_;
    std::vector<BranchBackpatch> branch_backpatches_;
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
void emit_load_const_store_sequence(std::vector<std::uint8_t> &code, std::uint32_t constant_value,
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
void emit_load_hook_store_sequence(std::vector<std::uint8_t> &code, std::uint32_t source_address,
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
    Kind kind{Kind::Constant};
    std::uint32_t value{0}; // constant value, or pin address
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
void load_operand_into(FragmentEmitter &fe, PrimitiveOperand op, sh2a::Reg reg) {
    fe.mov_l_disp_pc(reg, op.value);
    if (op.kind == PrimitiveOperand::Kind::HookInputPointer) {
        fe.mov_l_at_reg_reg(reg, reg);
    }
}

// Store the convention-result register (R1) to `destination_address`.
// Used as the tail of every binary-primitive fragment.
void emit_store_r1_to(FragmentEmitter &fe, std::uint32_t destination_address) {
    fe.mov_l_disp_pc(sh2a::Reg::R2, destination_address);
    fe.mov_l_reg_at_reg(sh2a::Reg::R1, sh2a::Reg::R2);
}

// Float-operand loader. Reuses the integer load path to land the
// float's bit pattern in R0 (4 bytes, via PC-relative pool fetch +
// optional dereference for HookInput / SSA slots), then transfers
// to the target FRn via FPUL: LDS R0, FPUL ; FSTS FPUL, FRn.
//
// Three instructions per Constant operand, four per HookInput /
// SSA-slot operand. Mirrors `load_operand_into` for Int + Bool.
void load_operand_into_fr(FragmentEmitter &fe, PrimitiveOperand op, sh2a::FReg frn) {
    fe.mov_l_disp_pc(sh2a::Reg::R0, op.value);
    if (op.kind == PrimitiveOperand::Kind::HookInputPointer) {
        fe.mov_l_at_reg_reg(sh2a::Reg::R0, sh2a::Reg::R0);
    }
    fe.lds_r_fpul(sh2a::Reg::R0);
    fe.fsts_fpul_freg(frn);
}

// Float-result store. Mirror of `emit_store_r1_to`: pull FRm's bit
// pattern out via FPUL into R1, then take the existing int store
// tail. The destination-pointer pool entry lands at the same place
// regardless of int/float — the bits in memory are byte-identical.
void emit_store_fr_to(FragmentEmitter &fe, sh2a::FReg frm, std::uint32_t destination_address) {
    fe.flds_freg_fpul(frm);
    fe.sts_fpul_r(sh2a::Reg::R1);
    emit_store_r1_to(fe, destination_address);
}

// Int-to-float operand loader for the FPU bridge used by
// `divide_int`. Same shape as `load_operand_into_fr` for the GPR
// load + FPUL transfer, but the final step is `FLOAT FPUL, FRn`
// (conversion) instead of `FSTS FPUL, FRn` (bit-cast). Result: FRn
// holds (float32)(int32)<operand> rather than the operand's
// integer bit pattern reinterpreted as IEEE 754 (which would
// produce nonsense for any non-trivial operand).
//
// Precision: int32 values with |x| > 2^24 lose low-order bits in
// the float32 representation. For Subaru ECU table cells (typical
// range ±32768 or ±2^16) this is well within precision; flag the
// hazard at the primitive level if the user authors an integer
// table whose range exceeds 2^24.
void load_int_operand_into_fr_via_float(FragmentEmitter &fe, PrimitiveOperand op, sh2a::FReg frn) {
    fe.mov_l_disp_pc(sh2a::Reg::R0, op.value);
    if (op.kind == PrimitiveOperand::Kind::HookInputPointer) {
        fe.mov_l_at_reg_reg(sh2a::Reg::R0, sh2a::Reg::R0);
    }
    fe.lds_r_fpul(sh2a::Reg::R0);
    fe.float_fpul_freg(frn);
}

// Int-result store from the float register file via FTRC (truncate
// toward zero). Mirror of `emit_store_fr_to` but applies the
// truncate-and-convert opcode instead of FLDS's bit-cast, so the
// 32-bit word landing in memory is a signed int32 representing the
// integer part of FRm. Used by `divide_int`'s store tail.
//
// Overflow: FTRC saturates to INT_MIN / INT_MAX on out-of-range
// float values, no trap. This differs from C UB (e.g. INT_MIN / -1
// would be UB in C; here it lands as INT_MAX).
void emit_store_fr_truncated_to_int(FragmentEmitter &fe, sh2a::FReg frm,
                                    std::uint32_t destination_address) {
    fe.ftrc_freg_fpul(frm);
    fe.sts_fpul_r(sh2a::Reg::R1);
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `add_int(op1, op2)` → store. ADD is commutative
// so the operand-to-register mapping is the natural one (op1 → R0,
// op2 → R1; ADD R0, R1 leaves the sum in R1).
void emit_add_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                       std::uint32_t destination_address) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.add(sh2a::Reg::R0, sh2a::Reg::R1);
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `subtract_int(op1, op2)` → store. SUB is NOT
// commutative — the user expects `op1 - op2`. SH-2A `SUB Rm, Rn`
// computes Rn = Rn - Rm, so we put the minuend (op1) in R1 and the
// subtrahend (op2) in R0; the result lands in R1 by convention.
void emit_sub_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                       std::uint32_t destination_address) {
    load_operand_into(fe, op2, sh2a::Reg::R0); // subtrahend
    load_operand_into(fe, op1, sh2a::Reg::R1); // minuend
    fe.sub(sh2a::Reg::R0, sh2a::Reg::R1);      // R1 = R1 - R0 = op1 - op2
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `multiply_int(op1, op2)` → store. MUL.L is
// commutative on the values but writes its result to the MACL system
// register, NOT a general-purpose register; we follow up with
// STS MACL, R1 to extract the low 32 bits. Truncating to int32
// (matching int32 + int32 → int32 semantics; high 32 bits in MACH
// are discarded — overflow wraps two's-complement, same as add).
void emit_mul_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                       std::uint32_t destination_address) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.mul_l(sh2a::Reg::R0, sh2a::Reg::R1); // MACL = R0 * R1
    fe.sts_macl(sh2a::Reg::R1);             // R1 = MACL
    emit_store_r1_to(fe, destination_address);
}

// Body fragment for `and_bool(op1, op2)` → store. AND is commutative
// so the natural mapping works (op1 → R0, op2 → R1; AND R0, R1
// leaves the bitwise-AND in R1). On canonical 0/1 operands the bits
// align so the result is canonical 0/1 logical AND.
void emit_and_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                       std::uint32_t dst) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.and_(sh2a::Reg::R0, sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Body fragment for `or_bool(op1, op2)` → store. Same shape as
// and_bool with the OR opcode.
void emit_or_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                      std::uint32_t dst) {
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.or_(sh2a::Reg::R0, sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Body fragment for `not_bool(op)` → store. Unary primitive — only
// one operand. TST Rn, Rn sets T iff Rn==0; MOVT R1 then materializes
// that as 1 (when op was 0) or 0 (when op was non-zero). One
// instruction shorter than going through a constant + XOR.
void emit_not_fragment(FragmentEmitter &fe, PrimitiveOperand op, std::uint32_t dst) {
    load_operand_into(fe, op, sh2a::Reg::R0);
    fe.tst(sh2a::Reg::R0, sh2a::Reg::R0);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Body fragment for `select(cond, true_val, false_val)` → store.
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
// once the body offsets are known.
//
// **Type-agnostic at the register level.** true_val and false_val are
// always 4-byte words flowing through R1 — whether they came from an
// Int constant, a canonical 0/1 Bool, or an IEEE 754 binary32 bit
// pattern is irrelevant to the move/store sequence. The same code
// path emits select_int, select_bool, and select_float; the
// PrimitiveShape table records the type so the IR-side type checker
// catches mismatches before we get here. Float values DON'T go
// through the FPU here because there's no arithmetic — we're just
// shuffling a 32-bit word from a literal/RAM source to a RAM
// destination.
void emit_select_fragment(FragmentEmitter &fe, PrimitiveOperand cond, PrimitiveOperand true_val,
                          PrimitiveOperand false_val, std::uint32_t dst) {
    auto use_false = fe.create_label();
    auto done = fe.create_label();

    load_operand_into(fe, cond, sh2a::Reg::R0);
    fe.tst(sh2a::Reg::R0, sh2a::Reg::R0);
    fe.bt(use_false);
    load_operand_into(fe, true_val, sh2a::Reg::R1);
    fe.bra(done);
    fe.nop(); // BRA delay slot
    fe.place_label(use_false);
    load_operand_into(fe, false_val, sh2a::Reg::R1);
    fe.place_label(done);
    emit_store_r1_to(fe, dst);
}

// Body fragment for a comparison primitive. All three variants
// (compare_lt_int / compare_gt_int / compare_eq_int) share the same shape:
// load operands → CMP/X (sets T-bit) → MOVT R1 (R1 = T as 0/1) →
// store R1. The only thing that differs is the CMP opcode + the
// operand-to-register mapping. Bool widening = canonical 0/1.
void emit_cmp_lt_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                          std::uint32_t dst) {
    // T = (op1 < op2) = (op2 > op1) = (R1 > R0) ⇒ CMP/GT R0, R1.
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.cmp_gt(sh2a::Reg::R0, sh2a::Reg::R1);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

void emit_cmp_gt_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                          std::uint32_t dst) {
    // T = (op1 > op2) = (R0 > R1) ⇒ CMP/GT R1, R0.
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.cmp_gt(sh2a::Reg::R1, sh2a::Reg::R0);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

void emit_cmp_eq_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                          std::uint32_t dst) {
    // T = (op1 == op2). CMP/EQ is commutative; load order doesn't
    // affect correctness, only the operand-naming consistency.
    load_operand_into(fe, op1, sh2a::Reg::R0);
    load_operand_into(fe, op2, sh2a::Reg::R1);
    fe.cmp_eq(sh2a::Reg::R0, sh2a::Reg::R1);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// ---- Float arithmetic ---------------------------------------------
//
// Shared shape per primitive:
//   load op1 → R0 → FPUL → FR0
//   load op2 → R0 → FPUL → FR1   (R0 is scratch — re-used each load)
//   F* FRm, FRn                  (FR1 = FR1 op FR0; sub/div pick the
//                                  Rm/Rn mapping to match op1-op2 /
//                                  op1÷op2 semantics)
//   FLDS FR1, FPUL ; STS FPUL, R1
//   MOV.L @(d,PC), R2 ; MOV.L R1, @R2  (the int-side store tail)
//
// Add and multiply are commutative so the natural FR0=op1, FR1=op2,
// `FOP FR0, FR1` works. Sub and div are not — SH-2A's `FSUB FRm, FRn`
// computes FRn = FRn − FRm, which means FRn must hold the minuend; we
// load op1 → FR1 and op2 → FR0 so `FSUB FR0, FR1` yields op1 − op2.
// Identical reordering for `FDIV FR0, FR1` → op1 ÷ op2.

void emit_add_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                             std::uint32_t dst) {
    load_operand_into_fr(fe, op1, sh2a::FReg::FR0);
    load_operand_into_fr(fe, op2, sh2a::FReg::FR1);
    fe.fadd(sh2a::FReg::FR0, sh2a::FReg::FR1); // FR1 = FR1 + FR0
    emit_store_fr_to(fe, sh2a::FReg::FR1, dst);
}

void emit_sub_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                             std::uint32_t dst) {
    // FSUB FRm, FRn ⇒ FRn = FRn − FRm. Put minuend (op1) in FR1.
    load_operand_into_fr(fe, op2, sh2a::FReg::FR0); // subtrahend
    load_operand_into_fr(fe, op1, sh2a::FReg::FR1); // minuend
    fe.fsub(sh2a::FReg::FR0, sh2a::FReg::FR1);      // FR1 = FR1 − FR0
    emit_store_fr_to(fe, sh2a::FReg::FR1, dst);
}

void emit_mul_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                             std::uint32_t dst) {
    load_operand_into_fr(fe, op1, sh2a::FReg::FR0);
    load_operand_into_fr(fe, op2, sh2a::FReg::FR1);
    fe.fmul(sh2a::FReg::FR0, sh2a::FReg::FR1); // FR1 = FR1 * FR0
    emit_store_fr_to(fe, sh2a::FReg::FR1, dst);
}

void emit_div_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                             std::uint32_t dst) {
    // FDIV FRm, FRn ⇒ FRn = FRn / FRm. Put dividend (op1) in FR1.
    // Divide-by-zero produces a FPU exception per the SH-2A spec;
    // when authoring features that may divide by user-supplied values,
    // gate with a compare → select. v1.x doesn't insert a guard.
    load_operand_into_fr(fe, op2, sh2a::FReg::FR0); // divisor
    load_operand_into_fr(fe, op1, sh2a::FReg::FR1); // dividend
    fe.fdiv(sh2a::FReg::FR0, sh2a::FReg::FR1);      // FR1 = FR1 / FR0
    emit_store_fr_to(fe, sh2a::FReg::FR1, dst);
}

// Body fragment for `sqrt_float(op)` → store. Single-operand FPU op.
// FSQRT FRn ⇒ FRn = sqrt(FRn). Negative-operand handling: FPU raises
// Invalid Operation. Mirrors the divide_float posture — caller is
// trusted not to pass negatives, or to gate via compare → select.
//
// Motivating use case (docs/16 §"Where the next IR primitive lands"):
// Bernoulli-based differential-fuel-pressure compensation, where the
// injector pulse-width correction factor is sqrt(actual_rail_pressure /
// commanded_rail_pressure). Same shape COBB ships as "Differential
// Fuel Pressure Compensation" — see fixtures/samples/fa24-bernoulli-
// comp.stmod for an FA24-swap-themed worked example.
void emit_sqrt_float_fragment(FragmentEmitter &fe, PrimitiveOperand op, std::uint32_t dst) {
    load_operand_into_fr(fe, op, sh2a::FReg::FR0);
    fe.fsqrt(sh2a::FReg::FR0); // FR0 = sqrt(FR0)
    emit_store_fr_to(fe, sh2a::FReg::FR0, dst);
}

// Body fragment for `flex_fuel_scale(ethanol_pct)` → store. Computes
// a linear fuel-multiplier curve approximating the well-known E0=1.00,
// E85≈1.28 stoich-shift baseline:
//
//   scale = 1.0 + ethanol_pct × (0.28 / 85.0)
//         = 1.0 + ethanol_pct × 0.003294...
//
// Outside the [0, 85] range the linear extrapolation continues — at
// E100 the formula gives ≈1.329, which is consistent with the AFR-shift
// chemistry but conservative compared to real-world flex-fuel maps
// (which often deliberately overshoot for cooling margin). Users
// authoring production flex-fuel calibrations should layer a compare
// → select for the safety upper-bound and gate on coolant temperature
// per the sample's commentary.
//
// IR contract: arity 1, Float in → Float out. Constants are baked in;
// no extra operands needed. A future "table-lookup" primitive will
// supersede this with caller-provided breakpoints + values, but for
// v1.x this baked curve unblocks the flex-fuel sample without needing
// to grow the operand-shape table beyond its 3-slot maximum.
//
// Two FPU ops + two literal-pool loads. Latency ~6 cycles on the FPU.
void emit_flex_fuel_scale_fragment(FragmentEmitter &fe, PrimitiveOperand op, std::uint32_t dst) {
    // Reciprocal-slope baked as float32: 0.28f / 85.0f.
    // Using float division at compile time would suffice, but the
    // explicit constant keeps the bit pattern deterministic across
    // host compilers + makes the value easy to audit in the literal
    // pool.
    constexpr float kSlope = 0.28F / 85.0F; // 0.003294117...
    constexpr float kIntercept = 1.0F;

    std::uint32_t const slope_bits = std::bit_cast<std::uint32_t>(kSlope);
    std::uint32_t const intercept_bits = std::bit_cast<std::uint32_t>(kIntercept);

    PrimitiveOperand const slope_op{PrimitiveOperand::Kind::Constant, slope_bits};
    PrimitiveOperand const intercept_op{PrimitiveOperand::Kind::Constant, intercept_bits};

    // FR0 = ethanol_pct
    load_operand_into_fr(fe, op, sh2a::FReg::FR0);
    // FR1 = slope (literal)
    load_operand_into_fr(fe, slope_op, sh2a::FReg::FR1);
    // FR1 = FR1 * FR0 = ethanol_pct * slope
    fe.fmul(sh2a::FReg::FR0, sh2a::FReg::FR1);
    // FR0 = intercept (literal)
    load_operand_into_fr(fe, intercept_op, sh2a::FReg::FR0);
    // FR1 = FR1 + FR0 = ethanol_pct * slope + 1.0
    fe.fadd(sh2a::FReg::FR0, sh2a::FReg::FR1);
    // Store FR1 → dst.
    emit_store_fr_to(fe, sh2a::FReg::FR1, dst);
}

// Body fragment for `divide_int(op1, op2)` → store. SH-2A's iterative
// integer divide (DIV0S + 32× DIV1) is encoding-tricky and has corner
// cases (INT_MIN / -1 overflow, signed-vs-unsigned modes) that we'd
// have to validate against silicon to trust. The FPU bridge sidesteps
// all of that with a well-defined three-step path: convert both
// operands int→float (FLOAT), divide on the float side (FDIV),
// convert the result float→int (FTRC, truncate toward zero — matches
// C int division on the cases that fit in float32 mantissa).
//
// Trade-offs vs. a hypothetical DIV1-iterative implementation:
//   - **Precision:** int32 has 32 bits; float32 has a 24-bit
//     mantissa. Operands with |x| > 2^24 lose low-order bits in the
//     conversion. For Subaru ECU tables this is well within the
//     usable range (typical cells ±32768).
//   - **Overflow:** FTRC saturates (INT_MIN / INT_MAX). C integer
//     overflow is UB. We pick saturation as the saner default; gate
//     INT_MIN/-1 at the design layer if the user needs it.
//   - **Divide-by-zero:** FPU exception, same as `divide_float`.
//     Documented caveat; gate with compare → select if needed.
//   - **Speed:** ~5 FPU ops vs. ~35 GPR ops for DIV1. FPU is fast on
//     SH-2A when FPSCR.PR=0 (single precision); this is the same
//     assumption the existing Float arithmetic primitives make.
//
// FDIV FRm, FRn ⇒ FRn = FRn / FRm — put dividend (op1) in FR1.
void emit_div_int_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                           std::uint32_t dst) {
    load_int_operand_into_fr_via_float(fe, op2, sh2a::FReg::FR0); // divisor
    load_int_operand_into_fr_via_float(fe, op1, sh2a::FReg::FR1); // dividend
    fe.fdiv(sh2a::FReg::FR0, sh2a::FReg::FR1);                    // FR1 /= FR0
    emit_store_fr_truncated_to_int(fe, sh2a::FReg::FR1, dst);
}

// ---- Float compares (Float, Float) → Bool -------------------------
//
// All three variants share the shape: load op1→FR0, op2→FR1, FCMP/*
// (sets T-bit), MOVT R1 (R1 = T as 0/1), int-side store tail. The
// FPU-side FCMP/EQ and FCMP/GT are IEEE 754 ordered — NaN operands
// produce T=0. compare_lt uses FCMP/GT with the operand mapping
// inverted relative to compare_gt.

void emit_cmp_lt_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                                std::uint32_t dst) {
    // T = (op1 < op2) ⇔ (op2 > op1). Load op1→FR0, op2→FR1;
    // FCMP/GT FR0, FR1 ⇒ T = (FR1 > FR0).
    load_operand_into_fr(fe, op1, sh2a::FReg::FR0);
    load_operand_into_fr(fe, op2, sh2a::FReg::FR1);
    fe.fcmp_gt(sh2a::FReg::FR0, sh2a::FReg::FR1);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

void emit_cmp_gt_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                                std::uint32_t dst) {
    // T = (op1 > op2). Mirror the int convention: load op1→FR0,
    // op2→FR1; FCMP/GT FR1, FR0 ⇒ T = (FR0 > FR1) = (op1 > op2).
    load_operand_into_fr(fe, op1, sh2a::FReg::FR0);
    load_operand_into_fr(fe, op2, sh2a::FReg::FR1);
    fe.fcmp_gt(sh2a::FReg::FR1, sh2a::FReg::FR0);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

void emit_cmp_eq_float_fragment(FragmentEmitter &fe, PrimitiveOperand op1, PrimitiveOperand op2,
                                std::uint32_t dst) {
    // T = (op1 == op2). FCMP/EQ is commutative; load order
    // arbitrary. IEEE 754 quirk: -0.0 == 0.0, NaN compares
    // unequal to itself.
    load_operand_into_fr(fe, op1, sh2a::FReg::FR0);
    load_operand_into_fr(fe, op2, sh2a::FReg::FR1);
    fe.fcmp_eq(sh2a::FReg::FR0, sh2a::FReg::FR1);
    fe.movt(sh2a::Reg::R1);
    emit_store_r1_to(fe, dst);
}

// Dispatch a CallPrimitive instruction to the right fragment emitter
// by symbol. Both the single-level and nested-emit paths route
// through this, so adding a new primitive only needs a new
// emit_X_fragment plus a clause here. Operand count is validated by
// validate_call_primitive before we get here, so each branch can
// index `operands` confidently.
[[nodiscard]] Status emit_primitive_fragment(FragmentEmitter &fe, std::string_view symbol,
                                             std::vector<PrimitiveOperand> const &operands,
                                             std::uint32_t dst) {
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
    if (symbol == "divide_int") {
        emit_div_int_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_lt_int") {
        emit_cmp_lt_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_gt_int") {
        emit_cmp_gt_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_eq_int") {
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
    if (symbol == "select_int" || symbol == "select_bool" || symbol == "select_float") {
        emit_select_fragment(fe, operands[0], operands[1], operands[2], dst);
        return ok();
    }
    if (symbol == "add_float") {
        emit_add_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "subtract_float") {
        emit_sub_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "multiply_float") {
        emit_mul_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "divide_float") {
        emit_div_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "sqrt_float") {
        emit_sqrt_float_fragment(fe, operands[0], dst);
        return ok();
    }
    if (symbol == "flex_fuel_scale") {
        emit_flex_fuel_scale_fragment(fe, operands[0], dst);
        return ok();
    }
    if (symbol == "compare_lt_float") {
        emit_cmp_lt_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_gt_float") {
        emit_cmp_gt_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    if (symbol == "compare_eq_float") {
        emit_cmp_eq_float_fragment(fe, operands[0], operands[1], dst);
        return ok();
    }
    std::string msg{"SH-2A backend: CallPrimitive '"};
    msg.append(symbol);
    msg.append("' not yet implemented (slice supports add_int, "
               "subtract_int, multiply_int, divide_int, "
               "compare_lt_int, compare_gt_int, compare_eq_int, "
               "and_bool, or_bool, not_bool, select_int, "
               "select_bool, select_float, add_float, "
               "subtract_float, multiply_float, divide_float, "
               "sqrt_float, compare_lt_float, compare_gt_float, "
               "compare_eq_float, flex_fuel_scale)");
    return failure(ErrorCode::NotImplemented, std::move(msg));
}

// Look up a hook by id in the loaded definition. Linear scan — pack
// hook counts are small (<<100 in practice).
[[nodiscard]] Hook const *find_hook(Definition const &def, std::string_view id) noexcept {
    for (auto const &h : def.hooks()) {
        if (h.id == id)
            return &h;
    }
    return nullptr;
}

// Look up a hook's input pin by name (matches a `HookSignal` in the
// hook's `inputs` array — the data-out-from-ECU side; not the
// graph-side "input pin" of the user's logic). Returns nullptr if
// the hook doesn't declare that pin.
[[nodiscard]] HookSignal const *find_hook_input(Hook const &h, std::string_view name) noexcept {
    for (auto const &s : h.inputs) {
        if (s.name == name)
            return &s;
    }
    return nullptr;
}

// Resolve a LoadHookInput instruction's source pin to its firmware
// address. Encapsulates the three error paths shared by direct
// LoadHookInput→Store chains and CallPrimitive operands: hook
// missing, pin missing on the hook, pin has no `address`. The
// `target_hook` parameter is informational only — cross-hook reads
// are legal under a flat memory model. Returns the address in `out`
// on success.
//
// `target_hook` is the hook owning the Store this load feeds into
// (kept on the signature because earlier slices used it for a
// "same-hook only" sanity gate). It's not needed for correctness on
// SH-2A — any firmware address is reachable from any patch — but
// callers retain it for future per-hook checks (e.g. detecting that
// a sensor pin is updated AFTER the splice point's PC, which would
// make the read produce a stale value within a single ECU loop
// iteration). v1.x accepts any source-hook.
[[nodiscard]] Status resolve_hook_input_address(Definition const &def,
                                                ir::Instruction const &load_ins,
                                                Hook const * /*target_hook*/, std::uint32_t &out) {
    Hook const *src_hook = find_hook(def, load_ins.symbol);
    if (src_hook == nullptr) {
        std::string msg{"codegen: LoadHookInput hook '"};
        msg.append(load_ins.symbol);
        msg.append("' not declared in the loaded definition pack");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    HookSignal const *pin = find_hook_input(*src_hook, load_ins.pin_name);
    if (pin == nullptr) {
        std::string msg{"codegen: hook '"};
        msg.append(src_hook->id);
        msg.append("' does not declare input pin '");
        msg.append(load_ins.pin_name);
        msg.append("'");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    if (!pin->address.has_value()) {
        std::string msg{"codegen: hook input '"};
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
[[nodiscard]] Result<std::uint32_t> coerce_constant_to_u32(ir::Instruction const &load_ins) {
    if (!load_ins.constant_value.has_value()) {
        return failure(ErrorCode::ParseError, "codegen: LoadConstant has no "
                                              "constant_value (lower() invariant violation)");
    }
    double const v = *load_ins.constant_value;
    switch (load_ins.result_type) {
    case PinType::Int: {
        if (v < -2147483648.0 || v > 2147483647.0) {
            std::string msg{"codegen: LoadConstant value "};
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
    return failure(ErrorCode::NotImplemented, "codegen: unknown PinType for LoadConstant");
}

// Locate the producing Instruction for `value_id`. Returns nullptr
// if no instruction in the module produces it. Caller is responsible
// for verifying the op kind is one the slice supports.
[[nodiscard]] ir::Instruction const *find_producer(ir::Module const &m,
                                                   ir::ValueId value_id) noexcept {
    for (auto const &ins : m.instructions) {
        if (ins.result_id == value_id)
            return &ins;
    }
    return nullptr;
}

// Build a PrimitiveOperand from a producing instruction. Checks that
// the producer's result_type matches the primitive's declared
// operand_type (e.g. add_int requires Int operands, and_bool requires
// Bool operands); cross-hook flow rejected; pre-allocated SSA RAM
// slots for nested CallPrimitive results read from `slots`.
[[nodiscard]] Result<PrimitiveOperand>
operand_from_producer(Definition const &def, ir::Module const &m, ir::ValueId value_id,
                      Hook const *target_hook, PinType expected_operand_type,
                      std::unordered_map<ir::ValueId, std::uint32_t> const &slots) {
    ir::Instruction const *prod = find_producer(m, value_id);
    if (prod == nullptr) {
        return failure(ErrorCode::ParseError, "SH-2A backend: primitive operand does not "
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
        if (!r.has_value())
            return failure(r.error());
        return PrimitiveOperand{PrimitiveOperand::Kind::Constant, *r};
    }
    if (prod->op == ir::Op::LoadHookInput) {
        std::uint32_t addr = 0;
        if (auto s = resolve_hook_input_address(def, *prod, target_hook, addr); !s.has_value()) {
            return failure(s.error());
        }
        return PrimitiveOperand{PrimitiveOperand::Kind::HookInputPointer, addr};
    }
    if (prod->op == ir::Op::CallPrimitive) {
        // Read the operand value from the slot allocated for this
        // nested primitive's result. Mechanically identical to
        // HookInputPointer at the SH-2A level (load address, deref);
        // semantically distinct (firmware variable vs scratch RAM slot).
        auto it = slots.find(value_id);
        if (it == slots.end()) {
            return failure(ErrorCode::ParseError, "SH-2A backend: nested primitive operand has "
                                                  "no RAM slot (walk should have allocated one)");
        }
        return PrimitiveOperand{PrimitiveOperand::Kind::HookInputPointer, it->second};
    }
    return failure(ErrorCode::NotImplemented, "SH-2A backend: primitive operand has unsupported "
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
    std::size_t arity;
    std::array<PinType, 3> operand_types;
    PinType result_type;
};

[[nodiscard]] PrimitiveShape const *primitive_shape(std::string_view symbol) noexcept {
    // Arithmetic:    2 Int → Int.
    // Comparison:    2 Int → Bool.
    // Boolean logic: 2 (or 1) Bool → Bool.
    // Select:        3 operands — first is Bool (condition); other
    //                two share the result type.
    // Unused trailing slots are filled with the prior slot's type;
    // they're never read at arity-checked dispatch time.
    static constexpr struct Entry {
        std::string_view name;
        PrimitiveShape shape;
    } kTable[] = {
        {"add_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Int}},
        {"subtract_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Int}},
        {"multiply_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Int}},
        {"divide_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Int}},
        {"compare_lt_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Bool}},
        {"compare_gt_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Bool}},
        {"compare_eq_int", {2, {PinType::Int, PinType::Int, PinType::Int}, PinType::Bool}},
        {"and_bool", {2, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"or_bool", {2, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"not_bool", {1, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"select_int", {3, {PinType::Bool, PinType::Int, PinType::Int}, PinType::Int}},
        {"select_bool", {3, {PinType::Bool, PinType::Bool, PinType::Bool}, PinType::Bool}},
        {"add_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Float}},
        {"subtract_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Float}},
        {"multiply_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Float}},
        {"divide_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Float}},
        {"sqrt_float", {1, {PinType::Float, PinType::Float, PinType::Float}, PinType::Float}},
        // flex_fuel_scale — fixed E0=1.00, E85=1.28 linear curve; arity
        // 1 (ethanol_pct in, fuel scale out). Future bundle generalizes
        // to N-point lookup tables via a separate `curve_float` primitive
        // — for v1.x this fixed-curve form unblocks the flex-fuel sample
        // without growing the operand-shape table's 3-slot maximum.
        {"flex_fuel_scale",
         {1, {PinType::Float, PinType::Float, PinType::Float}, PinType::Float}},
        {"compare_lt_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Bool}},
        {"compare_gt_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Bool}},
        {"compare_eq_float", {2, {PinType::Float, PinType::Float, PinType::Float}, PinType::Bool}},
        {"select_float", {3, {PinType::Bool, PinType::Float, PinType::Float}, PinType::Float}},
    };
    for (auto const &e : kTable) {
        if (e.name == symbol)
            return &e.shape;
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
                   "subtract_int, multiply_int, divide_int, "
                   "compare_lt_int, compare_gt_int, compare_eq_int, "
                   "and_bool, or_bool, not_bool, select_int, "
                   "select_bool, select_float, add_float, "
                   "subtract_float, multiply_float, divide_float, "
                   "sqrt_float, compare_lt_float, compare_gt_float, "
                   "compare_eq_float, flex_fuel_scale)");
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
[[nodiscard]] Status emit_nested_call_primitive(Definition const &def, ir::Module const &m,
                                                ir::Instruction const &root_prim, Hook const *hook,
                                                RamAllocator &ram, std::vector<RamClaim> &claims,
                                                std::vector<std::uint8_t> &out_code,
                                                std::uint32_t dst_addr) {

    // Topological walk: recurse into CallPrimitive operands first so
    // intermediate slots exist by the time the consumer emits.
    //
    // Fan-out (shared SSA values): when one CallPrimitive's result
    // feeds multiple consumers down the tree, the walk would otherwise
    // visit it once per consumer — wasting a RAM slot per extra visit
    // and emitting the compute fragment redundantly. `visited` tracks
    // by result_id so each producer fragment is materialized exactly
    // once and all consumers share its single slot.
    std::unordered_map<ir::ValueId, std::uint32_t> slots;
    std::unordered_set<ir::ValueId> visited;
    std::vector<ir::Instruction const *> emit_order;

    auto const walk = [&](auto &self_ref, ir::Instruction const *prim, bool is_root) -> Status {
        // Already walked? (Only happens on shared SSA values, since
        // root primitives are entered exactly once from the caller.)
        if (!is_root && visited.find(prim->result_id) != visited.end()) {
            return ok();
        }
        if (auto s = validate_call_primitive(*prim); !s.has_value()) {
            return failure(s.error());
        }
        for (auto operand_id : prim->operands) {
            ir::Instruction const *prod = find_producer(m, operand_id);
            if (prod == nullptr) {
                return failure(ErrorCode::ParseError, "SH-2A backend: nested operand does not "
                                                      "resolve to any producing instruction");
            }
            if (prod->op == ir::Op::CallPrimitive) {
                if (auto s = self_ref(self_ref, prod, /*is_root=*/false); !s.has_value()) {
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
            slots[prim->result_id] = static_cast<std::uint32_t>(claim_r->address);
            claims.push_back(*claim_r);
            visited.insert(prim->result_id);
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
            return failure(ErrorCode::ParseError, "SH-2A backend: primitive symbol lookup "
                                                  "regressed after validation");
        }
        std::vector<PrimitiveOperand> operands;
        operands.reserve(shape->arity);
        for (std::size_t i = 0; i < shape->arity; ++i) {
            auto op = operand_from_producer(def, m, prim->operands[i], hook,
                                            shape->operand_types[i], slots);
            if (!op.has_value())
                return failure(op.error());
            operands.push_back(*op);
        }
        std::uint32_t const this_dest = (prim == &root_prim) ? dst_addr : slots.at(prim->result_id);
        if (auto s = emit_primitive_fragment(fe, prim->symbol, operands, this_dest);
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
    Hook const *hook{nullptr};
    RamAllocator ram{0, 0};
    std::vector<std::uint8_t> code;
    std::vector<RamClaim> claims;
    // (output_pin_name) → claim address. One slot per output pin
    // referenced by Store instructions in this module — repeated
    // writes to the same pin share the slot so the firmware reads a
    // single live address.
    std::unordered_map<std::string, std::uint32_t> pin_slots;
};

} // namespace

Result<PatchObject> Sh2aBackend::compile(ir::Module const &m, Definition const &def) {
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
    std::map<std::string, HookWork> works;
    std::vector<std::string> ordered_hooks;

    for (auto const &ins : m.instructions) {
        if (ins.op != ir::Op::StoreHookOutput)
            continue;

        if (ins.operands.size() != 1) {
            return failure(ErrorCode::ParseError, "SH-2A backend: StoreHookOutput must have "
                                                  "exactly one operand");
        }
        // Find the source instruction. A Store consuming a
        // non-existent ValueId is an IR invariant violation.
        ir::Instruction const *src = find_producer(m, ins.operands[0]);
        if (src == nullptr) {
            return failure(ErrorCode::ParseError, "SH-2A backend: StoreHookOutput operand "
                                                  "does not resolve to any producing instruction "
                                                  "(invariant: lower() should assign result_id "
                                                  "to every value-producing op)");
        }

        // Slice rule: Store source must be LoadConstant, LoadHookInput,
        // or a supported CallPrimitive. Anything else (deeper nesting,
        // unsupported primitive kind) gets a specific NotImplemented.
        if (src->op != ir::Op::LoadConstant && src->op != ir::Op::LoadHookInput &&
            src->op != ir::Op::CallPrimitive) {
            return failure(ErrorCode::NotImplemented, "SH-2A backend: StoreHookOutput source must "
                                                      "be a LoadConstant, LoadHookInput, or "
                                                      "CallPrimitive in this slice");
        }

        // All three PinTypes (Int, Float, Bool) flow through the
        // same 4-byte MOV.L paths. The codegen doesn't reinterpret
        // the bytes — the firmware does. Bool widening (0/1
        // canonical) happens inside coerce_constant_to_u32 and is
        // produced by MOVT after comparison primitives, keeping the
        // representation uniform.
        if (src->op == ir::Op::LoadConstant && !src->constant_value.has_value()) {
            return failure(ErrorCode::ParseError, "SH-2A backend: LoadConstant has no "
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
        if (!hook->free_ram_base.has_value() || !hook->free_ram_length.has_value() ||
            *hook->free_ram_length == 0) {
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
            w.ram = RamAllocator{*hook->free_ram_base, *hook->free_ram_length};
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
            auto claim_r = work.ram.claim(4, 4); // 4-byte longword
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
            if (!u32.has_value())
                return failure(u32.error());
            emit_load_const_store_sequence(work.code, *u32, dst_addr);
        } else if (src->op == ir::Op::LoadHookInput) {
            std::uint32_t pin_addr = 0;
            if (auto s = resolve_hook_input_address(def, *src, hook, pin_addr); !s.has_value()) {
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
                if (auto s = emit_nested_call_primitive(def, m, *src, hook, work.ram, work.claims,
                                                        work.code, dst_addr);
                    !s.has_value()) {
                    return failure(s.error());
                }
            } else {
                std::unordered_map<ir::ValueId, std::uint32_t> empty_slots;
                PrimitiveShape const *shape = primitive_shape(src->symbol);
                if (shape == nullptr) {
                    // Unreachable — validate_call_primitive checked earlier.
                    return failure(ErrorCode::ParseError, "SH-2A backend: primitive symbol "
                                                          "lookup regressed after validation");
                }
                std::vector<PrimitiveOperand> operands;
                operands.reserve(shape->arity);
                for (std::size_t i = 0; i < shape->arity; ++i) {
                    auto op = operand_from_producer(def, m, src->operands[i], hook,
                                                    shape->operand_types[i], empty_slots);
                    if (!op.has_value())
                        return failure(op.error());
                    operands.push_back(*op);
                }
                FragmentEmitter fe;
                if (auto s = emit_primitive_fragment(fe, src->symbol, operands, dst_addr);
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
        HookPatch hp{};
        hp.symbol = w.hook->id;
        hp.splice_address = *w.hook->ecu_address;
        hp.code = w.code;
        hp.ram_claims = w.claims;
        obj.hooks.push_back(std::move(hp));
    }
    // Address gate per docs/16 §Safety #6. Wired into the backend's
    // exit path so callers cannot accidentally bypass it. An empty
    // PatchObject passes the gate vacuously (no hooks to check), which
    // matches the "no-op flash" semantic above.
    if (auto s = gate_patch(obj, def); !s.has_value()) {
        return failure(s.error());
    }
    return obj;
}

namespace {

// Append a 16-bit halfword to `code` in little-endian byte order. RH850
// is little-endian on the bus.
void emit_le16(std::vector<std::uint8_t> &code, std::uint16_t v) {
    code.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    code.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

// Emit the canonical RH850 "load constant, store to RAM slot" sequence.
// Layout per the table in rh850.hpp above kStoreSequenceSize. 24 bytes
// total: 5 × 32-bit (movhi/movea/movhi/movea/st.w) + 2 × 16-bit
// (jmp [lp], nop pad).
void emit_rh850_load_const_store(std::vector<std::uint8_t> &code,
                                 std::uint32_t constant_value,
                                 std::uint32_t destination_address) {
    constexpr rh850::Reg kValReg = rh850::Reg::R10;
    constexpr rh850::Reg kDstReg = rh850::Reg::R11;

    auto const val_split = rh850::split_imm32(constant_value);
    auto const dst_split = rh850::split_imm32(destination_address);

    // MOVHI val_hi, r0, r10    — r10 = (val & 0xFFFF0000) (with sign-comp)
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kValReg));
    emit_le16(code, val_split.hi);
    // MOVEA val_lo, r10, r10   — r10 = full 32-bit constant
    emit_le16(code, rh850::enc_movea_hw1(kValReg, kValReg));
    emit_le16(code, val_split.lo);
    // MOVHI dst_hi, r0, r11
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kDstReg));
    emit_le16(code, dst_split.hi);
    // MOVEA dst_lo, r11, r11   — r11 = full destination address
    emit_le16(code, rh850::enc_movea_hw1(kDstReg, kDstReg));
    emit_le16(code, dst_split.lo);
    // ST.W r10, 0[r11]         — mem[r11] = r10
    emit_le16(code, rh850::enc_st_w_hw1(kValReg, kDstReg));
    emit_le16(code, rh850::enc_st_w_hw2(0));
    // JMP [lp]                 — return; followed by a NOP for 4-byte
    // alignment of whatever follows the patch.
    emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
    emit_le16(code, rh850::enc_nop());
}

// Materialize a single PrimitiveOperand into `target_reg` on RH850.
//
//   Constant         : MOVHI+MOVEA the literal directly into target_reg
//                      (8 bytes, no scratch needed).
//   HookInputPointer : MOVHI+MOVEA the address into scratch_reg, then
//                      LD.W 0[scratch_reg], target_reg (12 bytes).
//
// `target_reg` and `scratch_reg` must be distinct in the HookInputPointer
// case — otherwise the address materialization clobbers what we're about
// to load. The caller is responsible for pairing them.
void emit_rh850_load_primitive_operand(std::vector<std::uint8_t> &code,
                                       PrimitiveOperand const &op,
                                       rh850::Reg target_reg,
                                       rh850::Reg scratch_reg) {
    auto const split = rh850::split_imm32(op.value);
    if (op.kind == PrimitiveOperand::Kind::Constant) {
        emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, target_reg));
        emit_le16(code, split.hi);
        emit_le16(code, rh850::enc_movea_hw1(target_reg, target_reg));
        emit_le16(code, split.lo);
        return;
    }
    // HookInputPointer (or future SSA RAM-slot pointer). Materialize
    // the address into scratch_reg first, then dereference.
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, scratch_reg));
    emit_le16(code, split.hi);
    emit_le16(code, rh850::enc_movea_hw1(scratch_reg, scratch_reg));
    emit_le16(code, split.lo);
    emit_le16(code, rh850::enc_ld_w_hw1(scratch_reg, target_reg));
    emit_le16(code, rh850::enc_ld_w_hw2(0));
}

// Emit an RH850 binary-int-arithmetic CallPrimitive fragment.
// Shape:
//
//   load op1 → r10     (MOVHI+MOVEA, or MOVHI+MOVEA+LD.W via r12 scratch)
//   load op2 → r11     (same, scratch r12)
//   {ADD,SUB,AND,OR} r11, r10        (16-bit Format I) + NOP pad   — OR
//   {MUL,DIV} r11, r10, r13          (32-bit Format XI, no pad)
//   MOVHI dst_hi, r0, r12; MOVEA dst_lo, r12, r12   (r12 = dst_addr)
//   ST.W r10, 0[r12]   (mem[dst_addr] = result)
//   JMP [lp] + tail NOP
//
// Total size: 36/40/40/44 bytes depending on operand-kind combinations,
// always 4-aligned. Caller does not need to pad. The 2-byte difference
// between Format-I (ADD/etc.) and Format-XI (MUL/DIV) is absorbed by a
// matching change in alignment-pad emission, so all six primitives
// share the same kBinaryIntArithSize* envelope.
//
// Operand order convention: `op(a, b)` always loads a → r10, b → r11.
//  - SUB r11, r10 computes r10 = r10 - r11 = a - b.
//  - DIV r11, r10, r13 computes r10 = r10 / r11 = a / b (quotient);
//    r13 = a % b (remainder, discarded).
//  - MUL r11, r10, r13 computes r10 = (a * b) low 32 bits; r13 = high
//    (discarded — Int wraps two's-complement on overflow, same as add).
//
// Returns ParseError if `symbol` is not one of the four explicitly-
// dispatched binary primitives — this catches a future
// `rh850_validate_call_primitive` extension that forgets to add a
// matching dispatch arm here (previously fell through to OR, which
// would have silently miscompiled the new primitive).
[[nodiscard]] Status emit_rh850_binary_int_arith(std::vector<std::uint8_t> &code,
                                                 std::string_view symbol,
                                                 PrimitiveOperand const &op1,
                                                 PrimitiveOperand const &op2,
                                                 std::uint32_t dst_addr,
                                                 bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;   // op1 / result
    constexpr rh850::Reg kBReg = rh850::Reg::R11;   // op2
    constexpr rh850::Reg kScratch = rh850::Reg::R12; // address scratch

    emit_rh850_load_primitive_operand(code, op1, kAReg, kScratch);
    emit_rh850_load_primitive_operand(code, op2, kBReg, kScratch);

    // ADD/SUB/AND/OR are 16-bit Format I (2 bytes) — they need a
    // following NOP pad to restore 4-alignment for the dst-addr
    // MOVHI/MOVEA pair. MUL/DIV are 32-bit Format XI (4 bytes) — no
    // pad needed. `needs_align_nop` tracks which class the symbol
    // landed in so the size envelope stays identical across all six
    // arithmetic primitives.
    bool needs_align_nop = true;
    if (symbol == "add_int") {
        emit_le16(code, rh850::enc_add_reg(kBReg, kAReg));
    } else if (symbol == "subtract_int") {
        emit_le16(code, rh850::enc_sub_reg(kBReg, kAReg));
    } else if (symbol == "and_bool") {
        // Bool values are 0/1-normalized — bitwise AND is logical AND.
        emit_le16(code, rh850::enc_and_reg(kBReg, kAReg));
    } else if (symbol == "or_bool") {
        emit_le16(code, rh850::enc_or_reg(kBReg, kAReg));
    } else if (symbol == "multiply_int") {
        // MUL r11, r10, r13 — r10 = (r11 * r10) low 32 bits;
        // r13 holds high 32 bits (discarded — Int wraps two's-complement
        // on overflow, same convention as add_int).
        emit_le16(code, rh850::enc_mul_hw1(kBReg, kAReg));
        emit_le16(code, rh850::enc_mul_hw2(rh850::Reg::R13));
        needs_align_nop = false;
    } else if (symbol == "divide_int") {
        // DIV r11, r10, r13 — r10 = r10 / r11 = a / b (quotient);
        // r13 holds remainder (discarded). Signed-divide semantics
        // match int32_t a/b for the language-level operator.
        emit_le16(code, rh850::enc_div_hw1(kBReg, kAReg));
        emit_le16(code, rh850::enc_div_hw2(rh850::Reg::R13));
        needs_align_nop = false;
    } else {
        std::string msg{"RH850 backend: emit_rh850_binary_int_arith called with "
                        "unsupported symbol '"};
        msg.append(symbol);
        msg.append("' — validator/dispatch out of sync");
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    // Pad to keep the byte stream 4-aligned. ADD/SUB/AND/OR are 16-bit
    // (2 bytes) and need the pad; MUL/DIV are 32-bit (4 bytes) and
    // skip it. Either way the total stays identical to the documented
    // kBinaryIntArithSize* envelope.
    if (needs_align_nop) {
        emit_le16(code, rh850::enc_nop());
    }

    // Materialize dst_addr into r12, then ST.W r10, 0[r12].
    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
    return ok();
}

// Emit an RH850 single-precision FP arithmetic CallPrimitive fragment
// for `add_float`, `subtract_float`, `multiply_float`, `divide_float`.
// Shape:
//
//   load op1 → r10            (IEEE 754 bits as ordinary 32-bit value)
//   load op2 → r11            (same)
//   {ADDF,SUBF,MULF,DIVF}.S r11, r10, r10   (32-bit Format F:I — dest
//                              shares the kAReg slot so the surrounding
//                              ST.W r10 / JMP tail is byte-identical to
//                              the int-arith path)
//   MOVHI dst_hi, r0, r12; MOVEA dst_lo, r12, r12   (r12 = dst_addr)
//   ST.W r10, 0[r12]
//   JMP [lp] + tail NOP
//
// Total size: identical envelope to kBinaryIntArithSize* — the FPU op
// is 4 bytes (no alignment pad needed) where ADD/SUB is 2 + pad.
//
// Operand semantics, matching the int-arith convention:
//   add_float(a, b)      → ADDF.S r11, r10, r10  → r10 = r10 + r11
//   subtract_float(a, b) → SUBF.S r11, r10, r10  → r10 = r10 - r11
//   multiply_float(a, b) → MULF.S r11, r10, r10  → r10 = r10 * r11
//   divide_float(a, b)   → DIVF.S r11, r10, r10  → r10 = r10 / r11
//
// LoadConstant operands carry IEEE 754 bit patterns produced by
// coerce_constant_to_u32 (PinType::Float branch — std::bit_cast<u32>
// of the host float). LoadHookInput operands are pack-author's
// responsibility: the source ROM/RAM word at the input's address must
// hold a single-precision IEEE 754 bit pattern, not an integer.
//
// VERIFICATION: per the RH850 file header, FPU encoding has not been
// validated against a real VB WRX ECU. Two pre-conditions before
// shipping any float PatchObject to hardware:
//   1. Confirm the VB ECU's RH850 core has a single-precision FPU
//      (G3M/G3MH/G4MH yes, G3K no). Compiling against an FPU-less
//      core would land undefined-instruction traps in the patch.
//   2. Bench-rig round-trip on the chosen core variant.
[[nodiscard]] Status emit_rh850_binary_float_arith(std::vector<std::uint8_t> &code,
                                                   std::string_view symbol,
                                                   PrimitiveOperand const &op1,
                                                   PrimitiveOperand const &op2,
                                                   std::uint32_t dst_addr,
                                                   bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;    // op1 → reg2 in F:I, also reg3 (dest)
    constexpr rh850::Reg kBReg = rh850::Reg::R11;    // op2 → reg1 in F:I
    constexpr rh850::Reg kScratch = rh850::Reg::R12; // address scratch

    emit_rh850_load_primitive_operand(code, op1, kAReg, kScratch);
    emit_rh850_load_primitive_operand(code, op2, kBReg, kScratch);

    if (symbol == "add_float") {
        emit_le16(code, rh850::enc_addf_s_hw1(kBReg, kAReg));
        emit_le16(code, rh850::enc_addf_s_hw2(kAReg));
    } else if (symbol == "subtract_float") {
        emit_le16(code, rh850::enc_subf_s_hw1(kBReg, kAReg));
        emit_le16(code, rh850::enc_subf_s_hw2(kAReg));
    } else if (symbol == "multiply_float") {
        emit_le16(code, rh850::enc_mulf_s_hw1(kBReg, kAReg));
        emit_le16(code, rh850::enc_mulf_s_hw2(kAReg));
    } else if (symbol == "divide_float") {
        emit_le16(code, rh850::enc_divf_s_hw1(kBReg, kAReg));
        emit_le16(code, rh850::enc_divf_s_hw2(kAReg));
    } else {
        std::string msg{"RH850 backend: emit_rh850_binary_float_arith called with "
                        "unsupported symbol '"};
        msg.append(symbol);
        msg.append("' — validator/dispatch out of sync");
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    // No alignment pad — Format F:I is 32-bit (4 bytes), already aligned.

    // Materialize dst_addr into r12, then ST.W r10, 0[r12].
    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
    return ok();
}

// Emit an RH850 int-compare CallPrimitive fragment for
// `compare_lt_int` / `compare_gt_int` / `compare_eq_int`. Shape:
//
//   load op1 → r10     (a)
//   load op2 → r11     (b)
//   CMP  r11, r10      (16-bit Format I — flags = (r10 - r11) = (a - b))
//   SETF cccc, r10     (32-bit Format IV — r10 = (cond) ? 1 : 0)
//   NOP                (alignment pad: 2 + 4 + 2 = 8 ≡ 0 mod 4)
//   MOVHI dst_hi, r0, r12; MOVEA dst_lo, r12, r12   (r12 = dst_addr)
//   ST.W r10, 0[r12]
//   JMP [lp] + tail NOP
//
// Operand order mirrors the binary-arith fragment so the load order is
// invariant across "leaf-operand binary primitive" shapes: op1 → r10
// (a), op2 → r11 (b). CMP r11, r10 puts (a - b) into the flags; the
// signed-comparison condition codes (LT / GT / E) then read directly:
//   compare_lt_int(a, b) — true iff a < b iff (a - b) < 0   → SETF LT
//   compare_gt_int(a, b) — true iff a > b iff (a - b) > 0   → SETF GT
//   compare_eq_int(a, b) — true iff a == b iff (a - b) == 0 → SETF E
//
// VERIFICATION: as with the rest of the RH850 slice, these encodings
// are sourced from public Renesas references (R01US0165) but have not
// been validated against a real VB WRX ECU. Bench-rig round-trip
// (compile → flash → read-back → byte-identical) is the gating step
// before any RH850 PatchObject reaches a real ECU.
[[nodiscard]] Status emit_rh850_int_compare(std::vector<std::uint8_t> &code,
                                            std::string_view symbol,
                                            PrimitiveOperand const &op1,
                                            PrimitiveOperand const &op2,
                                            std::uint32_t dst_addr,
                                            bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;    // op1 (a) / result
    constexpr rh850::Reg kBReg = rh850::Reg::R11;    // op2 (b)
    constexpr rh850::Reg kScratch = rh850::Reg::R12; // address scratch

    emit_rh850_load_primitive_operand(code, op1, kAReg, kScratch);
    emit_rh850_load_primitive_operand(code, op2, kBReg, kScratch);

    // CMP r11, r10  → flags = (r10 - r11) = (a - b).
    emit_le16(code, rh850::enc_cmp_reg(kBReg, kAReg));

    rh850::Cond cond{};
    if (symbol == "compare_lt_int") {
        cond = rh850::Cond::LT;
    } else if (symbol == "compare_gt_int") {
        cond = rh850::Cond::GT;
    } else if (symbol == "compare_eq_int") {
        cond = rh850::Cond::Z;
    } else {
        std::string msg{"RH850 backend: emit_rh850_int_compare called with "
                        "unsupported symbol '"};
        msg.append(symbol);
        msg.append("' — validator/dispatch out of sync");
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    // SETF cccc, r10 — 32-bit Format IV. r10 = (cond) ? 1 : 0.
    emit_le16(code, rh850::enc_setf_hw1(cond, kAReg));
    emit_le16(code, rh850::enc_setf_hw2());

    // Pad to restore 4-alignment: CMP (2) + SETF (4) + NOP (2) = 8.
    emit_le16(code, rh850::enc_nop());

    // Materialize dst_addr into r12, then ST.W r10, 0[r12].
    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    // JMP [lp] + tail NOP (omitted for nested non-root primitives).
    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
    return ok();
}

// Emit an RH850 unary float CallPrimitive fragment for `sqrt_float`.
// Shape:
//   load op → r10                          (8 or 12 bytes)
//   SQRTF.S r10, r10                       (4 bytes Format F:II)
//   MOVHI dst_hi, r0, r12; MOVEA dst_lo    (r12 = dst_addr)
//   ST.W r10, 0[r12]
//   JMP [lp] + tail NOP
//
// Format F:II is a 32-bit 2-register form — the reg1 slot of hw1 is
// forced to zero (R0) by the instruction's mask. Result lands in r10
// directly so the ST.W tail is byte-identical to the other slices.
//
// Domain note: SQRTF.S of a negative operand raises FPU invalid-op,
// surfaces as NaN result without the FPU exception trap enabled. Pack
// authors who care about negative inputs should gate with a compare →
// select; this matches how `divide_float` handles divide-by-zero.
[[nodiscard]] Status emit_rh850_sqrt_float(std::vector<std::uint8_t> &code,
                                           PrimitiveOperand const &op,
                                           std::uint32_t dst_addr,
                                           bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;    // op / result
    constexpr rh850::Reg kScratch = rh850::Reg::R12; // address scratch

    emit_rh850_load_primitive_operand(code, op, kAReg, kScratch);

    // SQRTF.S r10, r10 — Format F:II.
    emit_le16(code, rh850::enc_sqrtf_s_hw1(kAReg));
    emit_le16(code, rh850::enc_sqrtf_s_hw2(kAReg));

    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
    return ok();
}

// Emit an RH850 float-compare CallPrimitive fragment for
// `compare_lt_float` / `compare_gt_float` / `compare_eq_float`.
//
// RH850 has no float-side SETF; result extraction is a 3-instruction
// dance:
//   CMPF.S cccc, reg1, reg2, 0   — FPU.FCB[0] = (reg2 cccc reg1)
//   TRFSR 0                      — PSW.Z       = FPU.FCB[0]
//   SETF Z, r10                  — r10         = (PSW.Z == 1) ? 1 : 0
//
// Operand semantics with cccc = OLT (ordered less-than):
//   compare_lt_float(a, b) — load a → r10, b → r11; CMPF.S OLT, r11, r10
//                            → FCB[0] = (r10 < r11) = (a < b) ✓
// `compare_gt_float(a, b)` uses the same OLT cccc but loads with the
// operand registers swapped — load a → r11, b → r10; CMPF.S OLT, r11, r10
// → FCB[0] = (r10 < r11) = (b < a) = (a > b) ✓
// `compare_eq_float(a, b)` uses cccc = EQ; commutative, so the load
// order matches compare_lt (op1 → r10, op2 → r11).
//
// "Ordered" cccc variants make NaN inputs yield false, matching the
// natural reading of compare_lt/gt_float. compare_eq with NaN returns
// false (NaN != NaN per IEEE 754).
//
// VERIFICATION CAVEAT: same as TRFSR — we assume PSW.Z ← FCB[fff]
// direct copy. If the bench rig shows inversion, swap Cond::Z →
// Cond::NZ in the SETF below for all three branches.
[[nodiscard]] Status emit_rh850_float_compare(std::vector<std::uint8_t> &code,
                                              std::string_view symbol,
                                              PrimitiveOperand const &op1,
                                              PrimitiveOperand const &op2,
                                              std::uint32_t dst_addr,
                                              bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;    // → reg2 in F:III
    constexpr rh850::Reg kBReg = rh850::Reg::R11;    // → reg1 in F:III
    constexpr rh850::Reg kScratch = rh850::Reg::R12; // address scratch

    rh850::FloatCond cccc{};
    bool swap = false;
    if (symbol == "compare_lt_float") {
        cccc = rh850::FloatCond::OLT;
    } else if (symbol == "compare_gt_float") {
        cccc = rh850::FloatCond::OLT;
        swap = true; // load a → reg1, b → reg2
    } else if (symbol == "compare_eq_float") {
        cccc = rh850::FloatCond::EQ;
    } else {
        std::string msg{"RH850 backend: emit_rh850_float_compare called with "
                        "unsupported symbol '"};
        msg.append(symbol);
        msg.append("' — validator/dispatch out of sync");
        return failure(ErrorCode::ParseError, std::move(msg));
    }

    if (swap) {
        emit_rh850_load_primitive_operand(code, op1, kBReg, kScratch);
        emit_rh850_load_primitive_operand(code, op2, kAReg, kScratch);
    } else {
        emit_rh850_load_primitive_operand(code, op1, kAReg, kScratch);
        emit_rh850_load_primitive_operand(code, op2, kBReg, kScratch);
    }

    // CMPF.S cccc, r11 (reg1), r10 (reg2), fff=0 — sets FCB[0].
    emit_le16(code, rh850::enc_cmpf_s_hw1(kBReg, kAReg));
    emit_le16(code, rh850::enc_cmpf_s_hw2(cccc, 0));

    // TRFSR 0 — copies FCB[0] to PSW.Z.
    emit_le16(code, rh850::enc_trfsr_hw1());
    emit_le16(code, rh850::enc_trfsr_hw2(0));

    // SETF Z, r10 — r10 = (PSW.Z == 1) ? 1 : 0.
    emit_le16(code, rh850::enc_setf_hw1(rh850::Cond::Z, kAReg));
    emit_le16(code, rh850::enc_setf_hw2());

    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
    return ok();
}

// Emit an RH850 flex-fuel-scale CallPrimitive fragment. Single Float
// operand (ethanol_pct), produces a Float scale factor via the same
// baked linear curve as the SH-2A path:
//
//   scale = 1.0 + ethanol_pct × (0.28 / 85.0)
//
// Anchors: E0 → 1.00, E85 → 1.28, E100 → ≈1.329. Same caveats as
// SH-2A — outside [0, 85] the linear extrapolation continues; pack
// authors layering a safety clamp should gate with compare → select.
//
// Shape (8 instructions, 4-aligned):
//   load op → r10                                    (8 or 12 bytes)
//   MOVHI+MOVEA slope_const → r11                    (8 bytes)
//   MULF.S r11, r10, r10   ; r10 = r10 * slope       (4 bytes)
//   MOVHI+MOVEA intercept_const → r11                (8 bytes)
//   ADDF.S r11, r10, r10   ; r10 = r10 + 1.0         (4 bytes)
//   MOVHI+MOVEA dst → r12                            (8 bytes)
//   ST.W r10, 0[r12]                                 (4 bytes)
//   JMP [lp] + tail NOP                              (4 bytes)
[[nodiscard]] Status emit_rh850_flex_fuel_scale(std::vector<std::uint8_t> &code,
                                                PrimitiveOperand const &op,
                                                std::uint32_t dst_addr,
                                                bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;    // op / running result
    constexpr rh850::Reg kBReg = rh850::Reg::R11;    // constant scratch
    constexpr rh850::Reg kScratch = rh850::Reg::R12; // address scratch

    // Same baked constants as the SH-2A path. Bit-cast preserves the
    // exact IEEE 754 pattern across host compilers.
    constexpr float kSlope = 0.28F / 85.0F;
    constexpr float kIntercept = 1.0F;
    std::uint32_t const slope_bits = std::bit_cast<std::uint32_t>(kSlope);
    std::uint32_t const intercept_bits = std::bit_cast<std::uint32_t>(kIntercept);

    emit_rh850_load_primitive_operand(code, op, kAReg, kScratch);

    // r11 = slope (MOVHI+MOVEA — same shape as a Constant operand).
    auto const slope_split = rh850::split_imm32(slope_bits);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kBReg));
    emit_le16(code, slope_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kBReg, kBReg));
    emit_le16(code, slope_split.lo);

    // MULF.S r11, r10, r10 — r10 = r10 * slope.
    emit_le16(code, rh850::enc_mulf_s_hw1(kBReg, kAReg));
    emit_le16(code, rh850::enc_mulf_s_hw2(kAReg));

    // r11 = intercept.
    auto const intercept_split = rh850::split_imm32(intercept_bits);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kBReg));
    emit_le16(code, intercept_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kBReg, kBReg));
    emit_le16(code, intercept_split.lo);

    // ADDF.S r11, r10, r10 — r10 = r10 + intercept.
    emit_le16(code, rh850::enc_addf_s_hw1(kBReg, kAReg));
    emit_le16(code, rh850::enc_addf_s_hw2(kAReg));

    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
    return ok();
}

// Emit an RH850 3-operand branchless select fragment for
// `select_int`, `select_bool`, `select_float`.
//
// Strategy (works for any 32-bit operand kind because we manipulate
// bits, not values):
//
//   1. Load cond → r10  (Bool: 0 or 1)
//   2. Load true_val → r11
//   3. Load false_val → r12
//   4. MOV r0 → r13      ; r13 = 0
//   5. SUB r10, r13      ; r13 = 0 - cond = -cond (0 or 0xFFFFFFFF)
//   6. AND r13, r11      ; r11 = true_val & mask
//   7. NOT r13, r13      ; r13 = ~mask
//   8. AND r13, r12      ; r12 = false_val & ~mask
//   9. OR  r12, r11      ; r11 = result
//  10. MOVHI+MOVEA dst → r13
//  11. ST.W r11, 0[r13]
//  12. JMP [lp] + NOP tail
//
// Six Format-I 16-bit ops between the operand loads and the dst
// materialize — alignment is preserved if we emit them as 12 bytes
// (3 instructions×4 bytes would be 12, but each Format-I is 2 bytes,
// so 6 × 2 = 12 bytes, naturally 4-aligned). No pad NOP needed.
//
// Operand kinds: Constant operands materialize via MOVHI+MOVEA (8
// bytes each); HookInputPointer operands need MOVHI+MOVEA + LD.W
// (12 bytes each). Each operand uses its own scratch (r14) for the
// pointer materialization since r10/r11/r12 hold the live values.
//
// Bool invariant: this only steers correctly when cond ∈ {0, 1}. The
// codegen normalizes Bool LoadConstants to 0/1 via coerce_constant_to_u32,
// and Bool HookInputPointer values are pack-author's responsibility
// — same contract as not_bool's XOR-with-1.
void emit_rh850_select(std::vector<std::uint8_t> &code, PrimitiveOperand const &cond,
                       PrimitiveOperand const &true_val, PrimitiveOperand const &false_val,
                       std::uint32_t dst_addr, bool with_tail = true) {
    constexpr rh850::Reg kCondReg = rh850::Reg::R10;
    constexpr rh850::Reg kTrueReg = rh850::Reg::R11;
    constexpr rh850::Reg kFalseReg = rh850::Reg::R12;
    constexpr rh850::Reg kMaskReg = rh850::Reg::R13; // mask = -cond, then ~mask
    constexpr rh850::Reg kScratch = rh850::Reg::R14; // address scratch for loads/store

    emit_rh850_load_primitive_operand(code, cond, kCondReg, kScratch);
    emit_rh850_load_primitive_operand(code, true_val, kTrueReg, kScratch);
    emit_rh850_load_primitive_operand(code, false_val, kFalseReg, kScratch);

    // mask = 0 - cond.
    emit_le16(code, rh850::enc_mov_reg(rh850::kZero, kMaskReg));    // r13 = 0
    emit_le16(code, rh850::enc_sub_reg(kCondReg, kMaskReg));        // r13 -= cond
    // true_val &= mask
    emit_le16(code, rh850::enc_and_reg(kMaskReg, kTrueReg));        // r11 &= r13
    // r13 = ~mask
    emit_le16(code, rh850::enc_not_reg(kMaskReg, kMaskReg));        // r13 = ~r13
    // false_val &= ~mask
    emit_le16(code, rh850::enc_and_reg(kMaskReg, kFalseReg));       // r12 &= r13
    // result = true_val | false_val (only one of the two is non-zero)
    emit_le16(code, rh850::enc_or_reg(kFalseReg, kTrueReg));        // r11 |= r12

    // Materialize dst_addr into r13, then ST.W r11, 0[r13].
    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kMaskReg));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kMaskReg, kMaskReg));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kTrueReg, kMaskReg));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
}

// Emit an RH850 unary-bool CallPrimitive fragment for `not_bool`.
// Computes `result = x XOR 1` — correct for the 0/1-normalized Bool
// invariant the codegen maintains. Bitwise NOT would yield
// 0xFFFFFFFF/0xFFFFFFFE, breaking the invariant for downstream
// consumers; XOR-with-1 stays inside {0, 1}.
//
// Shape:
//   load op → r10                (8 or 12 bytes)
//   load constant 1 → r11        (8 bytes — always Constant)
//   XOR r11, r10                 (2 bytes, result in r10)
//   NOP pad                      (2 bytes, 4-align)
//   MOVHI+MOVEA dst → r12        (8 bytes)
//   ST.W r10, 0[r12]             (4 bytes)
//   JMP [lp] + NOP tail          (4 bytes)
//
// Total: 36 (C) or 40 (H) bytes, 4-aligned.
void emit_rh850_not_bool(std::vector<std::uint8_t> &code, PrimitiveOperand const &op,
                         std::uint32_t dst_addr, bool with_tail = true) {
    constexpr rh850::Reg kAReg = rh850::Reg::R10;
    constexpr rh850::Reg kBReg = rh850::Reg::R11;
    constexpr rh850::Reg kScratch = rh850::Reg::R12;

    emit_rh850_load_primitive_operand(code, op, kAReg, kScratch);
    // Load constant 1 into r11. Reuses the same Constant materialization
    // path with split_imm32(1) = {hi=0, lo=1}.
    PrimitiveOperand const one{PrimitiveOperand::Kind::Constant, 1U};
    emit_rh850_load_primitive_operand(code, one, kBReg, kScratch);

    // XOR r11, r10 → r10 = x ^ 1.
    emit_le16(code, rh850::enc_xor_reg(kBReg, kAReg));
    // 4-byte alignment pad.
    emit_le16(code, rh850::enc_nop());

    auto const dst_split = rh850::split_imm32(dst_addr);
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kScratch));
    emit_le16(code, dst_split.hi);
    emit_le16(code, rh850::enc_movea_hw1(kScratch, kScratch));
    emit_le16(code, dst_split.lo);
    emit_le16(code, rh850::enc_st_w_hw1(kAReg, kScratch));
    emit_le16(code, rh850::enc_st_w_hw2(0));

    if (with_tail) {
        emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
        emit_le16(code, rh850::enc_nop());
    }
}

// Resolve a CallPrimitive's operand by ValueId into a PrimitiveOperand.
// Two modes, selected by whether `slots` is non-null:
//
//   slots == nullptr → flat mode. Only LoadConstant + LoadHookInput
//                      producers are accepted; CallPrimitive producers
//                      get a NotImplemented (the caller is on the flat
//                      dispatch path and didn't allocate intermediate
//                      slots).
//   slots != nullptr → nested mode. CallPrimitive producers are
//                      accepted and resolved as HookInputPointer reads
//                      from the RAM slot the nested driver already
//                      allocated for that producer. Mechanically
//                      identical to a HookInputPointer at the byte
//                      level — semantically it's a load from scratch
//                      RAM rather than a firmware variable.
//
// Mirrors the SH-2A `operand_from_producer` shape. The pointer-vs-
// reference asymmetry vs SH-2A (which takes slots by const-ref) keeps
// the existing flat-mode callers unchanged.
[[nodiscard]] Result<PrimitiveOperand>
rh850_operand_from_producer(Definition const &def, ir::Module const &m, ir::ValueId value_id,
                            Hook const *target_hook, PinType expected_operand_type,
                            std::unordered_map<ir::ValueId, std::uint32_t> const *slots = nullptr) {
    ir::Instruction const *prod = find_producer(m, value_id);
    if (prod == nullptr) {
        return failure(ErrorCode::ParseError, "RH850 backend: primitive operand does not "
                                              "resolve to any producing instruction");
    }
    if (prod->result_type != expected_operand_type) {
        std::string msg{"RH850 backend: primitive operand of type "};
        msg.append(pin_type_name(prod->result_type));
        msg.append(" does not match expected operand type ");
        msg.append(pin_type_name(expected_operand_type));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    if (prod->op == ir::Op::LoadConstant) {
        auto u32 = coerce_constant_to_u32(*prod);
        if (!u32.has_value())
            return failure(u32.error());
        return PrimitiveOperand{PrimitiveOperand::Kind::Constant, *u32};
    }
    if (prod->op == ir::Op::LoadHookInput) {
        std::uint32_t addr = 0;
        if (auto s = resolve_hook_input_address(def, *prod, target_hook, addr); !s.has_value()) {
            return failure(s.error());
        }
        return PrimitiveOperand{PrimitiveOperand::Kind::HookInputPointer, addr};
    }
    if (prod->op == ir::Op::CallPrimitive) {
        if (slots != nullptr) {
            auto it = slots->find(value_id);
            if (it == slots->end()) {
                return failure(ErrorCode::ParseError,
                               "RH850 backend: nested primitive operand has no "
                               "RAM slot (walk should have allocated one)");
            }
            return PrimitiveOperand{PrimitiveOperand::Kind::HookInputPointer, it->second};
        }
        return failure(ErrorCode::NotImplemented,
                       "RH850 backend: nested CallPrimitive operands are "
                       "not supported in this slice — flatten the graph or "
                       "wait for the next bundle");
    }
    return failure(ErrorCode::NotImplemented, "RH850 backend: primitive operand has unsupported "
                                              "producer op");
}

// Validate an RH850 CallPrimitive: recognized symbol within the slice,
// operand count + result type per the shared `primitive_shape` table.
// Rejects with NotImplemented for known primitives the RH850 slice
// doesn't cover yet (the supported list is the 22-primitive SH-2A
// parity set; anything outside that — future curve-lookup,
// fixed-point, etc. — trips this gate before codegen runs).
[[nodiscard]] Status rh850_validate_call_primitive(ir::Instruction const &prim) {
    PrimitiveShape const *shape = primitive_shape(prim.symbol);
    if (shape == nullptr) {
        std::string msg{"RH850 backend: CallPrimitive '"};
        msg.append(prim.symbol);
        msg.append("' is not a recognized primitive symbol");
        return failure(ErrorCode::NotImplemented, std::move(msg));
    }
    // Per-symbol gate. Extend this set as new slices land.
    bool const supported =
        (prim.symbol == "add_int" || prim.symbol == "subtract_int" ||
         prim.symbol == "multiply_int" || prim.symbol == "divide_int" ||
         prim.symbol == "and_bool" || prim.symbol == "or_bool" ||
         prim.symbol == "not_bool" || prim.symbol == "select_int" ||
         prim.symbol == "select_bool" || prim.symbol == "select_float" ||
         prim.symbol == "compare_lt_int" || prim.symbol == "compare_gt_int" ||
         prim.symbol == "compare_eq_int" ||
         prim.symbol == "add_float" || prim.symbol == "subtract_float" ||
         prim.symbol == "multiply_float" || prim.symbol == "divide_float" ||
         prim.symbol == "sqrt_float" || prim.symbol == "compare_lt_float" ||
         prim.symbol == "compare_gt_float" || prim.symbol == "compare_eq_float" ||
         prim.symbol == "flex_fuel_scale");
    if (!supported) {
        std::string msg{"RH850 backend: CallPrimitive '"};
        msg.append(prim.symbol);
        msg.append("' is not yet covered by the RH850 slice (supported: "
                   "add_int, subtract_int, multiply_int, divide_int, "
                   "and_bool, or_bool, not_bool, "
                   "select_int, select_bool, select_float, "
                   "compare_lt_int, compare_gt_int, compare_eq_int, "
                   "add_float, subtract_float, multiply_float, divide_float, "
                   "sqrt_float, compare_lt_float, compare_gt_float, "
                   "compare_eq_float, flex_fuel_scale). "
                   "See docs/16 §Current state.");
        return failure(ErrorCode::NotImplemented, std::move(msg));
    }
    if (prim.operands.size() != shape->arity) {
        std::string msg{"RH850 backend: "};
        msg.append(prim.symbol);
        msg.append(" requires exactly ");
        msg.append(std::to_string(shape->arity));
        msg.append(" operands, got ");
        msg.append(std::to_string(prim.operands.size()));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    if (prim.result_type != shape->result_type) {
        std::string msg{"RH850 backend: "};
        msg.append(prim.symbol);
        msg.append(" expects result type ");
        msg.append(pin_type_name(shape->result_type));
        msg.append(", got ");
        msg.append(pin_type_name(prim.result_type));
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    return ok();
}

// Unified RH850 primitive dispatch — picks the right emit by symbol.
// Mirrors SH-2A's `emit_primitive_fragment` shape so the nested driver
// has a single emission entry point. `with_tail` controls whether the
// fragment ends in JMP[lp]+NOP (the patch terminator) or just runs off
// the end into the next fragment (intermediate primitive in a nested
// tree). Operand count is trusted to match the symbol's arity —
// rh850_validate_call_primitive is the gate for that.
[[nodiscard]] Status emit_rh850_primitive_fragment(std::vector<std::uint8_t> &code,
                                                   std::string_view symbol,
                                                   std::vector<PrimitiveOperand> const &operands,
                                                   std::uint32_t dst, bool with_tail) {
    if (symbol == "not_bool") {
        emit_rh850_not_bool(code, operands[0], dst, with_tail);
        return ok();
    }
    if (symbol == "sqrt_float") {
        return emit_rh850_sqrt_float(code, operands[0], dst, with_tail);
    }
    if (symbol == "flex_fuel_scale") {
        return emit_rh850_flex_fuel_scale(code, operands[0], dst, with_tail);
    }
    if (symbol == "select_int" || symbol == "select_bool" || symbol == "select_float") {
        emit_rh850_select(code, operands[0], operands[1], operands[2], dst, with_tail);
        return ok();
    }
    if (symbol == "compare_lt_int" || symbol == "compare_gt_int" || symbol == "compare_eq_int") {
        return emit_rh850_int_compare(code, symbol, operands[0], operands[1], dst, with_tail);
    }
    if (symbol == "compare_lt_float" || symbol == "compare_gt_float" ||
        symbol == "compare_eq_float") {
        return emit_rh850_float_compare(code, symbol, operands[0], operands[1], dst, with_tail);
    }
    if (symbol == "add_float" || symbol == "subtract_float" || symbol == "multiply_float" ||
        symbol == "divide_float") {
        return emit_rh850_binary_float_arith(code, symbol, operands[0], operands[1], dst,
                                             with_tail);
    }
    // Default — binary int arith (add/sub/mul/div + and/or covered here too).
    return emit_rh850_binary_int_arith(code, symbol, operands[0], operands[1], dst, with_tail);
}

// Emit an RH850 Store rooted at a (possibly nested) CallPrimitive tree.
// Walks the tree bottom-up: each interior primitive (non-root) gets a
// 4-byte RAM slot for its result and emits a fragment that stores into
// that slot (no JMP tail — execution falls through to the next
// primitive's load). The root primitive's fragment stores to the
// caller's `dst_addr` and includes the JMP[lp]+NOP terminator.
//
// The walk handles arbitrary nesting depth as long as every interior
// node is a supported CallPrimitive. Shared SSA values (fan-out: one
// nested primitive's result feeding multiple consumers) are deduped via
// `visited` — the producer fragment emits exactly once and all
// consumers read the same slot, mirroring the SH-2A nested driver's
// behavior.
//
// For the degenerate single-primitive case (no nested operands), this
// produces output byte-identical to the flat-mode emit it replaces:
// one fragment with terminator, no RAM slots beyond the existing
// output-pin slot.
[[nodiscard]] Status emit_rh850_nested_call_primitive(Definition const &def, ir::Module const &m,
                                                      ir::Instruction const &root_prim,
                                                      Hook const *hook, RamAllocator &ram,
                                                      std::vector<RamClaim> &claims,
                                                      std::vector<std::uint8_t> &out_code,
                                                      std::uint32_t dst_addr) {
    std::unordered_map<ir::ValueId, std::uint32_t> slots;
    std::unordered_set<ir::ValueId> visited;
    std::vector<ir::Instruction const *> emit_order;

    auto const walk = [&](auto &self_ref, ir::Instruction const *prim, bool is_root) -> Status {
        if (!is_root && visited.find(prim->result_id) != visited.end()) {
            return ok(); // already walked via another consumer
        }
        if (auto s = rh850_validate_call_primitive(*prim); !s.has_value()) {
            return failure(s.error());
        }
        for (auto operand_id : prim->operands) {
            ir::Instruction const *prod = find_producer(m, operand_id);
            if (prod == nullptr) {
                return failure(ErrorCode::ParseError, "RH850 backend: nested operand does not "
                                                      "resolve to any producing instruction");
            }
            if (prod->op == ir::Op::CallPrimitive) {
                if (auto s = self_ref(self_ref, prod, /*is_root=*/false); !s.has_value()) {
                    return failure(s.error());
                }
            }
        }
        if (!is_root) {
            auto claim_r = ram.claim(4, 4);
            if (!claim_r.has_value()) {
                std::string msg{"RH850 backend: hook '"};
                msg.append(hook->id);
                msg.append("' free_ram exhausted while allocating slot "
                           "for nested primitive result");
                return failure(ErrorCode::OutOfRange, std::move(msg));
            }
            slots[prim->result_id] = static_cast<std::uint32_t>(claim_r->address);
            claims.push_back(*claim_r);
            visited.insert(prim->result_id);
        }
        emit_order.push_back(prim);
        return ok();
    };
    if (auto s = walk(walk, &root_prim, /*is_root=*/true); !s.has_value()) {
        return failure(s.error());
    }

    for (ir::Instruction const *prim : emit_order) {
        PrimitiveShape const *shape = primitive_shape(prim->symbol);
        if (shape == nullptr) {
            return failure(ErrorCode::ParseError, "RH850 backend: primitive symbol lookup "
                                                  "regressed after validation");
        }
        std::vector<PrimitiveOperand> operands;
        operands.reserve(shape->arity);
        for (std::size_t i = 0; i < shape->arity; ++i) {
            auto op = rh850_operand_from_producer(def, m, prim->operands[i], hook,
                                                  shape->operand_types[i], &slots);
            if (!op.has_value())
                return failure(op.error());
            operands.push_back(*op);
        }
        bool const is_root = (prim == &root_prim);
        std::uint32_t const this_dest = is_root ? dst_addr : slots.at(prim->result_id);
        if (auto s = emit_rh850_primitive_fragment(out_code, prim->symbol, operands, this_dest,
                                                   /*with_tail=*/is_root);
            !s.has_value()) {
            return failure(s.error());
        }
    }
    return ok();
}

// Emit the RH850 "load from hook input, store to RAM slot" sequence —
// the LoadHookInput → StoreHookOutput slice. Shape:
//
//   offset  bytes              instruction
//   0       [movhi hw1][hi]    MOVHI hi, r0, r12    ; r12 = source_hi << 16
//   4       [movea hw1][lo]    MOVEA lo, r12, r12   ; r12 = source_address
//   8       [ld.w hw1][disp]   LD.W 0[r12], r10     ; r10 = mem[r12]
//   12      [movhi hw1][hi]    MOVHI hi, r0, r11    ; r11 = dst_hi << 16
//   16      [movea hw1][lo]    MOVEA lo, r11, r11   ; r11 = dst_address
//   20      [st.w hw1][disp]   ST.W r10, 0[r11]     ; mem[r11] = r10
//   24      [jmp hw1][nop]     JMP [lp] + NOP pad   ; return + align
//
// Total: 6 × 32-bit + 1 × 16-bit JMP + 1 × 16-bit NOP = 28 bytes. The
// extra 4 bytes vs the load-constant shape come from materializing the
// source address (separate register from the destination so we don't
// have to spill/reload across the LD.W).
//
// Register allocation:
//   r10 — value-in-transit (LD.W writes here, ST.W reads from here)
//   r11 — destination address (after MOVHI+MOVEA)
//   r12 — source address (after MOVHI+MOVEA, before LD.W)
//
// All three are caller-saved per the RH850 ABI; no save/restore needed
// since the patch is leaf code (no outbound calls).
void emit_rh850_load_hook_store(std::vector<std::uint8_t> &code,
                                std::uint32_t source_address,
                                std::uint32_t destination_address) {
    constexpr rh850::Reg kValReg = rh850::Reg::R10;
    constexpr rh850::Reg kDstReg = rh850::Reg::R11;
    constexpr rh850::Reg kSrcReg = rh850::Reg::R12;

    auto const src_split = rh850::split_imm32(source_address);
    auto const dst_split = rh850::split_imm32(destination_address);

    // MOVHI src_hi, r0, r12
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kSrcReg));
    emit_le16(code, src_split.hi);
    // MOVEA src_lo, r12, r12   — r12 = full source address
    emit_le16(code, rh850::enc_movea_hw1(kSrcReg, kSrcReg));
    emit_le16(code, src_split.lo);
    // LD.W 0[r12], r10         — r10 = mem[r12]
    emit_le16(code, rh850::enc_ld_w_hw1(kSrcReg, kValReg));
    emit_le16(code, rh850::enc_ld_w_hw2(0));
    // MOVHI dst_hi, r0, r11
    emit_le16(code, rh850::enc_movhi_hw1(rh850::kZero, kDstReg));
    emit_le16(code, dst_split.hi);
    // MOVEA dst_lo, r11, r11   — r11 = full destination address
    emit_le16(code, rh850::enc_movea_hw1(kDstReg, kDstReg));
    emit_le16(code, dst_split.lo);
    // ST.W r10, 0[r11]         — mem[r11] = r10
    emit_le16(code, rh850::enc_st_w_hw1(kValReg, kDstReg));
    emit_le16(code, rh850::enc_st_w_hw2(0));
    // JMP [lp]                 — return; followed by a NOP for 4-byte
    // alignment of whatever follows the patch.
    emit_le16(code, rh850::enc_jmp_reg(rh850::kLp));
    emit_le16(code, rh850::enc_nop());
}

} // namespace

Result<PatchObject> Rh850Backend::compile(ir::Module const &m, Definition const &def) {
    // First pass mirrors the SH-2A backend: every Op is structurally
    // supported (each has at least one emission path). Per-op semantic
    // checks happen at Store-emit time below.
    for (auto const &ins : m.instructions) {
        switch (ins.op) {
        case ir::Op::LoadConstant:
        case ir::Op::LoadHookInput:
        case ir::Op::CallPrimitive:
        case ir::Op::StoreHookOutput:
            break;
        }
    }

    // Second pass: walk Stores, group by hook. Same control shape as
    // Sh2aBackend::compile — we don't share the loop because the SH-2A
    // version drives an SH-2A-specific FragmentEmitter and pulls in
    // FPU/multiply-result extraction concerns we don't have here.
    // Until RH850's primitive/nested-call slices land, the source set
    // is just LoadConstant.
    std::map<std::string, HookWork> works;
    std::vector<std::string> ordered_hooks;

    for (auto const &ins : m.instructions) {
        if (ins.op != ir::Op::StoreHookOutput)
            continue;

        if (ins.operands.size() != 1) {
            return failure(ErrorCode::ParseError, "RH850 backend: StoreHookOutput must have "
                                                  "exactly one operand");
        }
        ir::Instruction const *src = find_producer(m, ins.operands[0]);
        if (src == nullptr) {
            return failure(ErrorCode::ParseError, "RH850 backend: StoreHookOutput operand "
                                                  "does not resolve to any producing instruction "
                                                  "(invariant: lower() should assign result_id "
                                                  "to every value-producing op)");
        }

        // RH850 slice: source must be LoadConstant, LoadHookInput, or a
        // supported CallPrimitive. Unsupported CallPrimitive symbols
        // (e.g. multiply_int, compare_lt_int) get a precise
        // NotImplemented from rh850_validate_call_primitive below;
        // anything else (Store from Store, malformed IR) trips here.
        if (src->op != ir::Op::LoadConstant && src->op != ir::Op::LoadHookInput &&
            src->op != ir::Op::CallPrimitive) {
            return failure(ErrorCode::NotImplemented, "RH850 backend: StoreHookOutput source must "
                                                      "be a LoadConstant, LoadHookInput, or "
                                                      "CallPrimitive in this slice");
        }
        if (src->op == ir::Op::LoadConstant && !src->constant_value.has_value()) {
            return failure(ErrorCode::ParseError, "RH850 backend: LoadConstant has no "
                                                  "constant_value (lower() invariant violation)");
        }

        // Resolve target hook. Same field requirements as SH-2A: a
        // hook needs an ecu_address (splice point) and a free_ram
        // region (scratch for our output slot).
        Hook const *hook = find_hook(def, ins.symbol);
        if (hook == nullptr) {
            std::string msg{"RH850 backend: hook '"};
            msg.append(ins.symbol);
            msg.append("' not declared in the loaded definition pack");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        if (!hook->ecu_address.has_value()) {
            std::string msg{"RH850 backend: hook '"};
            msg.append(ins.symbol);
            msg.append("' has no ecu_address — pack must declare the "
                       "splice point for codegen");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }
        if (!hook->free_ram_base.has_value() || !hook->free_ram_length.has_value() ||
            *hook->free_ram_length == 0) {
            std::string msg{"RH850 backend: hook '"};
            msg.append(ins.symbol);
            msg.append("' has no free_ram region — pack must declare "
                       "scratch RAM for codegen");
            return failure(ErrorCode::InvalidArgument, std::move(msg));
        }

        auto it = works.find(hook->id);
        if (it == works.end()) {
            HookWork w{};
            w.hook = hook;
            w.ram = RamAllocator{*hook->free_ram_base, *hook->free_ram_length};
            ordered_hooks.push_back(hook->id);
            it = works.emplace(hook->id, std::move(w)).first;
        }
        HookWork &work = it->second;

        // One RAM slot per (hook, output_pin_name). Two writes to the
        // same pin share the slot; second write overwrites the first
        // at runtime, which is the intended semantic.
        auto slot_it = work.pin_slots.find(ins.pin_name);
        std::uint32_t dst_addr = 0;
        if (slot_it == work.pin_slots.end()) {
            auto claim_r = work.ram.claim(4, 4); // 4-byte word
            if (!claim_r.has_value()) {
                std::string msg{"RH850 backend: hook '"};
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
            if (!u32.has_value())
                return failure(u32.error());
            emit_rh850_load_const_store(work.code, *u32, dst_addr);
        } else if (src->op == ir::Op::LoadHookInput) {
            // LoadHookInput. Resolve the source pin → firmware
            // address via the shared (now arch-neutral) resolver,
            // then emit the LD.W→ST.W sequence.
            std::uint32_t pin_addr = 0;
            if (auto s = resolve_hook_input_address(def, *src, hook, pin_addr); !s.has_value()) {
                return failure(s.error());
            }
            emit_rh850_load_hook_store(work.code, pin_addr, dst_addr);
        } else {
            // CallPrimitive Store source. The nested driver handles both
            // the flat case (no nested operands, one fragment with
            // terminator — byte-identical to the prior per-symbol
            // dispatch) and the nested case (allocates an intermediate
            // RAM slot per non-root primitive, emits each in topological
            // order, JMP[lp]+NOP on the root only).
            if (auto s = emit_rh850_nested_call_primitive(def, m, *src, hook, work.ram,
                                                          work.claims, work.code, dst_addr);
                !s.has_value()) {
                return failure(s.error());
            }
        }
    }

    PatchObject obj{};
    obj.arch = Arch::Rh850;
    obj.hooks.reserve(ordered_hooks.size());
    for (auto const &id : ordered_hooks) {
        auto const &w = works.at(id);
        HookPatch hp{};
        hp.symbol = w.hook->id;
        hp.splice_address = *w.hook->ecu_address;
        hp.code = w.code;
        hp.ram_claims = w.claims;
        obj.hooks.push_back(std::move(hp));
    }
    // Same address gate as SH-2A. An empty PatchObject passes
    // vacuously (no hooks to check).
    if (auto s = gate_patch(obj, def); !s.has_value()) {
        return failure(s.error());
    }
    return obj;
}

// ---- Address gate -------------------------------------------------------

namespace {

// Pretty-format a `[address, address + length)` range as
// `0xAAAA..0xBBBB (N bytes)` for the gate error messages. Plain
// decimal + hex helpers below already exist for other paths; the
// hex format here matches what `hex_addr` in src/flash uses for
// consistency across the safety surfaces.
std::string format_range(std::size_t address, std::size_t length) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "0x%08zX..0x%08zX (%zu bytes)", address, address + length,
                  length);
    return std::string{buf};
}

} // namespace

Status gate_patch(PatchObject const &p, Definition const &def) {
    auto const &regions = def.writable_regions();

    if (regions.empty()) {
        if (p.hooks.empty()) {
            // Vacuously passes — no hooks to gate. Matches the empty-
            // PatchObject = no-op semantic the SH-2A backend produces
            // for an empty module.
            return ok();
        }
        std::string msg{"codegen address gate: pack has no [[writable_region]] "
                        "declared but the patch carries "};
        msg.append(std::to_string(p.hooks.size()));
        msg.append(" hook(s). Codegen fails closed when no writable regions "
                   "are declared — declare them in the pack TOML (see "
                   "docs/11-definition-format.md §writable_region) or the "
                   "patch cannot be safely emitted.");
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }

    // Total scan — every hook gets checked; the message gathers every
    // violation so the user fixes them all in one pass instead of one-
    // by-one. Inspired by `Definition::validate()` which uses the same
    // multi-violation idiom.
    std::string violations;
    std::size_t violation_count = 0;

    for (auto const &hp : p.hooks) {
        // Reject zero-byte hook code defensively. Should never happen
        // (backends emit at least one instruction per hook), but a
        // zero-length range trivially fits any region which would mask
        // a real bug.
        if (hp.code.empty()) {
            continue;
        }
        // A hook fits iff a SINGLE region fully contains the entire
        // [splice, splice+code.size()) range. Spanning two regions is
        // refused even when the union covers the range — bridging two
        // declared-safe zones is exactly the foot-gun the gate exists
        // to prevent.
        bool fits = false;
        for (auto const &r : regions) {
            if (hp.splice_address >= r.address &&
                hp.splice_address + hp.code.size() <= r.address + r.length) {
                fits = true;
                break;
            }
        }
        if (!fits) {
            if (!violations.empty()) {
                violations.append("\n");
            }
            violations.append("  - hook '");
            violations.append(hp.symbol);
            violations.append("' writes ");
            violations.append(format_range(hp.splice_address, hp.code.size()));
            violations.append(" which is not contained in any single "
                              "[[writable_region]]");
            ++violation_count;
        }
    }

    if (violation_count != 0) {
        std::string msg{"codegen address gate: "};
        msg.append(std::to_string(violation_count));
        msg.append(" hook(s) target addresses outside the pack's declared "
                   "writable regions:\n");
        msg.append(violations);
        msg.append("\n  Available regions:");
        for (auto const &r : regions) {
            msg.append("\n  - '");
            msg.append(r.name);
            msg.append("' (");
            msg.append(r.kind);
            msg.append(") ");
            msg.append(format_range(r.address, r.length));
        }
        return failure(ErrorCode::InvalidArgument, std::move(msg));
    }
    return ok();
}

// ---- Backend selection --------------------------------------------------

namespace {

[[nodiscard]] bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char const ca = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 32) : a[i];
        char const cb = b[i] >= 'a' && b[i] <= 'z' ? static_cast<char>(b[i] - 32) : b[i];
        if (ca != cb)
            return false;
    }
    return true;
}

} // namespace

Result<std::unique_ptr<IBackend>> select_backend(std::string_view platform) {
    auto const has_platform_token = [platform](std::string_view wanted) {
        std::size_t start = 0;
        while (start <= platform.size()) {
            auto const end = platform.find('.', start);
            auto const token = platform.substr(
                start, end == std::string_view::npos ? platform.size() - start : end - start);
            if (ieq(token, wanted)) {
                return true;
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    };
    if (has_platform_token("VA")) {
        return std::unique_ptr<IBackend>{new Sh2aBackend{}};
    }
    if (has_platform_token("VB")) {
        return std::unique_ptr<IBackend>{new Rh850Backend{}};
    }
    std::string msg{"select_backend: no codegen backend for platform '"};
    msg.append(platform);
    msg.append("' (recognized tokens: VA → sh2a, VB → rh850)");
    return failure(ErrorCode::UnsupportedVersion, std::move(msg));
}

Result<std::unique_ptr<IBackend>> select_backend(Definition const &def) {
    return select_backend(def.pack().platform);
}

// ---- PatchObject TOML round-trip ----------------------------------

namespace {

[[nodiscard]] std::optional<Arch> parse_arch(std::string_view s) noexcept {
    if (s == "sh2a")
        return Arch::Sh2a;
    if (s == "rh850")
        return Arch::Rh850;
    if (s == "unknown")
        return Arch::Unknown;
    return std::nullopt;
}

// Parse an even-length, all-hex string into bytes. Tolerant of leading
// 0x and embedded whitespace would complicate the diff story, so this
// function requires a contiguous run of [0-9A-Fa-f] of even length.
[[nodiscard]] Result<std::vector<std::uint8_t>> parse_hex_bytes(std::string_view s) {
    if (s.size() % 2 != 0) {
        return failure(ErrorCode::ParseError, "patch TOML: code hex string has odd length");
    }
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 2);
    auto const nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int const hi = nibble(s[i]);
        int const lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return failure(ErrorCode::ParseError, "patch TOML: code hex string has non-hex chars");
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace

std::string patch_to_toml(PatchObject const &p) {
    std::ostringstream out;
    out << "[patch]\n";
    out << "arch = \"" << arch_name(p.arch) << "\"\n";
    for (auto const &h : p.hooks) {
        out << "\n[[patch.hook]]\n";
        // Symbol uses TOML basic-string quoting — escape backslash and
        // double-quote, reject literal newlines. Mirrors feature::
        // toml_quote in src/feature/src/feature.cpp.
        out << "symbol = \"";
        for (char c : h.symbol) {
            if (c == '\\' || c == '"')
                out << '\\';
            out << c;
        }
        out << "\"\n";
        out << "splice_address = 0x" << std::hex << std::uppercase << h.splice_address << std::dec
            << std::nouppercase << "\n";
        out << "code = \"";
        char buf[3];
        for (auto const b : h.code) {
            std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned>(b));
            out << buf;
        }
        out << "\"\n";
        for (auto const &rc : h.ram_claims) {
            out << "\n[[patch.hook.ram_claim]]\n";
            out << "address = 0x" << std::hex << std::uppercase << rc.address << std::dec
                << std::nouppercase << "\n";
            out << "size = " << rc.size << "\n";
            out << "alignment = " << rc.alignment << "\n";
        }
    }
    return out.str();
}

Result<std::optional<PatchObject>> patch_from_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (toml::parse_error const &e) {
        std::string msg{"patch TOML parse: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    auto const *ptbl = tbl["patch"].as_table();
    if (ptbl == nullptr) {
        // No [patch] section — legitimate (a graph-only .stmod).
        return std::optional<PatchObject>{};
    }
    PatchObject p;
    auto const arch_s = (*ptbl)["arch"].value_or<std::string>("");
    if (arch_s.empty()) {
        return failure(ErrorCode::ParseError, "patch TOML: [patch] missing `arch`");
    }
    auto const arch = parse_arch(arch_s);
    if (!arch.has_value()) {
        std::string msg{"patch TOML: unknown arch '"};
        msg.append(arch_s);
        msg.append("' (recognized: sh2a, rh850, unknown)");
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    p.arch = *arch;
    auto const *hooks = (*ptbl)["hook"].as_array();
    if (hooks != nullptr) {
        for (auto const &elem : *hooks) {
            auto const *htbl = elem.as_table();
            if (htbl == nullptr) {
                return failure(ErrorCode::ParseError, "patch TOML: [[patch.hook]] not a table");
            }
            HookPatch hp;
            hp.symbol = (*htbl)["symbol"].value_or<std::string>("");
            if (hp.symbol.empty()) {
                return failure(ErrorCode::ParseError,
                               "patch TOML: [[patch.hook]] missing `symbol`");
            }
            auto const splice = (*htbl)["splice_address"].value<std::int64_t>();
            if (!splice.has_value()) {
                return failure(ErrorCode::ParseError, "patch TOML: [[patch.hook]] '" + hp.symbol +
                                                          "' missing `splice_address`");
            }
            hp.splice_address = static_cast<std::size_t>(*splice);
            auto const code_s = (*htbl)["code"].value_or<std::string>("");
            auto code_r = parse_hex_bytes(code_s);
            if (!code_r.has_value()) {
                std::string msg{"patch TOML: [[patch.hook]] '"};
                msg.append(hp.symbol);
                msg.append("': ");
                msg.append(code_r.error().message());
                return failure(ErrorCode::ParseError, std::move(msg));
            }
            hp.code = std::move(*code_r);
            auto const *claims = (*htbl)["ram_claim"].as_array();
            if (claims != nullptr) {
                for (auto const &ce : *claims) {
                    auto const *ctbl = ce.as_table();
                    if (ctbl == nullptr) {
                        return failure(ErrorCode::ParseError, "patch TOML: [[patch.hook.ram_claim]]"
                                                              " not a table");
                    }
                    RamClaim rc;
                    auto const ad = (*ctbl)["address"].value<std::int64_t>();
                    auto const sz = (*ctbl)["size"].value<std::int64_t>();
                    auto const al = (*ctbl)["alignment"].value<std::int64_t>();
                    if (!ad.has_value() || !sz.has_value() || !al.has_value()) {
                        return failure(ErrorCode::ParseError, "patch TOML: ram_claim missing "
                                                              "address/size/alignment");
                    }
                    rc.address = static_cast<std::size_t>(*ad);
                    rc.size = static_cast<std::size_t>(*sz);
                    rc.alignment = static_cast<std::size_t>(*al);
                    hp.ram_claims.push_back(rc);
                }
            }
            p.hooks.push_back(std::move(hp));
        }
    }
    return std::optional<PatchObject>{std::move(p)};
}

} // namespace st::feature::codegen
