// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::feature::ir — typed dataflow IR for the custom-features pipeline.
//
// Sits between the user-authored Graph (st::feature::Graph) and the
// per-ISA codegen backends (st::feature::sh2a, ::rh850 — future
// modules). Layered like a small SSA: each instruction produces zero
// or one value-id, consumed by later instructions via operand refs.
//
// Scope of this header:
//   - Op: the small enum of opcodes the lowerer emits
//   - Instruction: a single op with its operands + symbolic refs
//   - Module: the lowered form of one Graph — a flat instruction list
//   - lower(Graph): topological emission, honouring hook phase
//     semantics (read-side first, user logic, write-side last)
//
// Out of scope (later sessions):
//   - Control flow (if/else / state-machine nodes — straight-line
//     dataflow is enough for v1.x flat-foot + flex-fuel shapes)
//   - Codegen (separate backend modules)
//   - Real-time-budget analysis (linter responsibility, runs over
//     the Module before codegen)

#ifndef ST_FEATURE_IR_HPP
#define ST_FEATURE_IR_HPP

#include "st/core/result.hpp"
#include "st/feature.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::feature::ir {

// Symbolic identifier for an IR value. Assigned monotonically by the
// lowerer; 0 is reserved as "void / no result".
using ValueId = std::uint32_t;

enum class Op : std::uint8_t {
    // Materialize a Pin.default_value as a fresh SSA value. Operands
    // are empty; constant_value carries the literal; result_type
    // reflects the pin type that consumed the constant.
    LoadConstant,
    // Read a hook's output-side pin (ECU state offered to user
    // logic). `symbol` names the hook (e.g. "after_fuel_calc");
    // `pin_name` names the specific signal ("commanded_pw"). No
    // operands.
    LoadHookInput,
    // Invoke a pack-declared primitive. `symbol` names the primitive
    // ("multiply_float"); `pin_name` names the specific output pin
    // ("out"). Operands are the value-ids feeding each input pin in
    // their pack-declared order.
    CallPrimitive,
    // Write a value to a hook's input-side pin (override emitted to
    // the ECU). `symbol`/`pin_name` locate the target; operands has
    // exactly one entry — the value driving the override. Produces
    // no result (result_id == 0).
    StoreHookOutput,
};

[[nodiscard]] char const *op_name(Op op) noexcept;

struct Instruction {
    Op                       op{Op::LoadConstant};
    PinType                  result_type{PinType::Float};
    ValueId                  result_id{0};
    // Pack-symbol identifier for LoadHookInput / CallPrimitive /
    // StoreHookOutput. The lowerer strips the "hook." / "primitive."
    // kind prefix when storing — so this is the bare id ("multiply_
    // float", not "primitive.multiply_float"). Codegen resolves the
    // symbol back against the loaded Definition.
    std::string              symbol;
    // Pin name on the symbol. For CallPrimitive it identifies the
    // specific output pin (a primitive with multiple outputs would
    // emit one Instruction per output, sharing the operand list).
    // For LoadHookInput / StoreHookOutput it identifies the read /
    // write port.
    std::string              pin_name;
    std::vector<ValueId>     operands;
    // Symbolic names of the input pins corresponding 1:1 with
    // `operands`. Populated for CallPrimitive only — codegen uses
    // this to map operand[k] back to the primitive's declared input
    // pin (`a` vs `b` for subtract/divide, `cond` vs `t` vs `f` for
    // select). Empty for LoadConstant (no operands) and for
    // LoadHookInput / StoreHookOutput (operands trivially correspond
    // to the single port named in `pin_name`).
    std::vector<std::string> operand_pin_names;
    std::optional<double>    constant_value;  // only for LoadConstant
};

struct Module {
    std::vector<Instruction> instructions;
};

// Lower a graph to its IR. Fails when the graph itself fails
// validate(), or when an Input pin is neither edge-driven nor
// supplied with a default_value (in which case there's no value to
// load). lint() catches the latter as a warning; lower() upgrades it
// to a hard error because codegen can't proceed without a source.
[[nodiscard]] Result<Module> lower(Graph const &g);

// Human-readable dump of one Module, suitable for the CLI dump-ir
// subcommand and for golden-file tests. Format: one Instruction per
// line, `[%result_id =] OP operands... [@symbol.pin]`. Trailing
// "; estimated cost: N cycles" line per `estimate_cost`.
[[nodiscard]] std::string    dump(Module const &m);

// Rough cost estimate for the whole Module. Intended as the lint-
// side RT-budget heuristic per docs/16 §"Safety considerations" so
// codegen can refuse oversized features before flash. Per-Op costs
// are placeholders — accurate per-ISA cycle counts come with each
// codegen backend; this is the platform-agnostic approximation:
//   LoadConstant     1
//   LoadHookInput    2
//   CallPrimitive    3
//   StoreHookOutput  2
[[nodiscard]] std::size_t    estimate_cost(Module const &m) noexcept;

// One IR-level lint finding. `instruction_index` is the position in
// `Module.instructions` of the offending instruction when the
// finding is local to a specific op; absent when the finding is
// whole-module (e.g. an RT-budget overrun).
struct LintFinding {
    std::string                message;
    std::optional<std::size_t> instruction_index;
};

// Tunable thresholds for `ir::lint`. Defaults are placeholders
// matching docs/16 §"Safety considerations" intent — the real
// numbers come with each codegen backend's profiling. A budget of
// zero disables the RT-budget check.
struct LintOptions {
    // Worst-case-execution-time budget per docs/16 §"Real-time
    // budget". Compared against `estimate_cost` for now; once the
    // backends ship, this becomes a per-hook check rather than a
    // module-wide one.
    std::size_t budget_cycles{200};
};

// IR-level linter. Distinct from `feature::lint` (which runs on the
// Graph, before lowering): this pass catches problems that only
// surface in the lowered form — duplicate hook overrides, RT-budget
// overruns. Codegen consumes the same findings and refuses on any
// the project's policy treats as fatal.
[[nodiscard]] std::vector<LintFinding>
                              lint(Module const &m,
                                    LintOptions opts = {});

} // namespace st::feature::ir

#endif // ST_FEATURE_IR_HPP
