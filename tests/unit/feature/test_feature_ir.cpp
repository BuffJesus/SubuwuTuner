// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature.hpp"
#include "st/feature_ir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

st::feature::Node make_hook_node(char const *kind,
                                 std::vector<std::pair<char const *, st::feature::PinType>> outs,
                                 std::vector<std::pair<char const *, st::feature::PinType>> ins) {
    st::feature::Node n;
    n.kind = kind;
    n.label = kind;
    n.is_phase_break = true;
    st::feature::PinId next = 0;
    for (auto const &[name, type] : outs) {
        n.pins.push_back(
            st::feature::Pin{next++, name, type, st::feature::PinDirection::Output, ""});
    }
    for (auto const &[name, type] : ins) {
        n.pins.push_back(
            st::feature::Pin{next++, name, type, st::feature::PinDirection::Input, ""});
    }
    return n;
}

st::feature::Node
make_primitive_node(char const *kind,
                    std::vector<std::pair<char const *, st::feature::PinType>> ins,
                    std::vector<std::pair<char const *, st::feature::PinType>> outs) {
    st::feature::Node n;
    n.kind = kind;
    n.label = kind;
    st::feature::PinId next = 0;
    for (auto const &[name, type] : ins) {
        n.pins.push_back(
            st::feature::Pin{next++, name, type, st::feature::PinDirection::Input, ""});
    }
    for (auto const &[name, type] : outs) {
        n.pins.push_back(
            st::feature::Pin{next++, name, type, st::feature::PinDirection::Output, ""});
    }
    return n;
}

} // namespace

TEST_CASE("ir::lower on an empty graph returns an empty module", "[feature][ir]") {
    st::feature::Graph g;
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    REQUIRE(m->instructions.empty());
}

TEST_CASE("ir::lower emits LoadHookInput → CallPrimitive → "
          "StoreHookOutput for the canonical splice shape",
          "[feature][ir]") {
    // hook.read_rpm provides RPM; primitive.passthrough consumes
    // RPM; hook.after_fuel_calc consumes the result on its
    // override pin. Lowering should produce the natural straight-
    // line ordering.
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    auto const mid = g.add_node(make_primitive_node("primitive.passthrough",
                                                    {{"in", st::feature::PinType::Float}},
                                                    {{"out", st::feature::PinType::Float}}));
    auto const snk = g.add_node(make_hook_node(
        "hook.after_fuel_calc", {}, {{"Override fuel PW", st::feature::PinType::Float}}));
    REQUIRE(g.connect(src, 0, mid, 0).has_value());
    REQUIRE(g.connect(mid, 1, snk, 0).has_value());

    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    REQUIRE(m->instructions.size() == 3);
    REQUIRE(m->instructions[0].op == st::feature::ir::Op::LoadHookInput);
    REQUIRE(m->instructions[0].symbol == "read_rpm");
    REQUIRE(m->instructions[1].op == st::feature::ir::Op::CallPrimitive);
    REQUIRE(m->instructions[1].symbol == "passthrough");
    REQUIRE(m->instructions[1].operands.size() == 1);
    REQUIRE(m->instructions[1].operands[0] == m->instructions[0].result_id);
    REQUIRE(m->instructions[2].op == st::feature::ir::Op::StoreHookOutput);
    REQUIRE(m->instructions[2].symbol == "after_fuel_calc");
    REQUIRE(m->instructions[2].pin_name == "Override fuel PW");
    REQUIRE(m->instructions[2].operands.size() == 1);
    REQUIRE(m->instructions[2].operands[0] == m->instructions[1].result_id);
}

TEST_CASE("ir::lower emits LoadConstant for a defaulted input pin", "[feature][ir]") {
    // primitive.compare_gt with a default on its `b` pin, driven `a`
    // pin. Lowering should emit a LoadConstant for the `b` operand
    // (4000.0) then a CallPrimitive consuming both.
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    st::feature::Node cmp;
    cmp.kind = "primitive.compare_gt";
    cmp.label = "compare";
    cmp.pins.push_back(st::feature::Pin{0, "a", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    st::feature::Pin b{1, "b", st::feature::PinType::Float, st::feature::PinDirection::Input, ""};
    b.default_value = 4000.0;
    cmp.pins.push_back(std::move(b));
    cmp.pins.push_back(st::feature::Pin{2, "out", st::feature::PinType::Bool,
                                        st::feature::PinDirection::Output, ""});
    auto const cid = g.add_node(std::move(cmp));
    REQUIRE(g.connect(src, 0, cid, 0).has_value());

    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    REQUIRE(m->instructions.size() == 3);
    REQUIRE(m->instructions[0].op == st::feature::ir::Op::LoadHookInput);
    REQUIRE(m->instructions[1].op == st::feature::ir::Op::LoadConstant);
    REQUIRE(m->instructions[1].constant_value.has_value());
    REQUIRE(*m->instructions[1].constant_value == 4000.0);
    REQUIRE(m->instructions[2].op == st::feature::ir::Op::CallPrimitive);
    REQUIRE(m->instructions[2].operands.size() == 2);
    // Operand order matches pin order: a first (from LoadHookInput),
    // b second (from LoadConstant).
    REQUIRE(m->instructions[2].operands[0] == m->instructions[0].result_id);
    REQUIRE(m->instructions[2].operands[1] == m->instructions[1].result_id);
    // operand_pin_names anchor each operand to its declared input
    // pin name — codegen relies on this to map operand[k] back to
    // `a`/`b` so non-commutative primitives don't swap arguments
    // when a graph reorders pins.
    REQUIRE(m->instructions[2].operand_pin_names.size() == 2);
    REQUIRE(m->instructions[2].operand_pin_names[0] == "a");
    REQUIRE(m->instructions[2].operand_pin_names[1] == "b");
}

TEST_CASE("ir::lower's operand_pin_names tracks pin reordering", "[feature][ir]") {
    // Same primitive, but with input pins instantiated in reverse
    // declaration order (b before a). The IR's operand_pin_names
    // MUST follow the node's pin-vector order, so codegen can
    // detect the reorder and rebind accordingly. Without this, a
    // hand-edited .stmod with reordered pins silently miscompiles
    // subtract / divide / select.
    st::feature::Graph g;
    auto const src_a =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm_a", st::feature::PinType::Float}}, {}));
    auto const src_b =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm_b", st::feature::PinType::Float}}, {}));
    st::feature::Node sub;
    sub.kind = "primitive.subtract_float";
    sub.label = "sub";
    // Pin order: b, a, out — reversed from the canonical declaration.
    sub.pins.push_back(st::feature::Pin{0, "b", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    sub.pins.push_back(st::feature::Pin{1, "a", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    sub.pins.push_back(st::feature::Pin{2, "out", st::feature::PinType::Float,
                                        st::feature::PinDirection::Output, ""});
    auto const sid = g.add_node(std::move(sub));
    REQUIRE(g.connect(src_a, 0, sid, 0).has_value()); // a→b slot
    REQUIRE(g.connect(src_b, 0, sid, 1).has_value()); // b→a slot

    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    auto const &call = m->instructions.back();
    REQUIRE(call.op == st::feature::ir::Op::CallPrimitive);
    REQUIRE(call.operand_pin_names.size() == 2);
    REQUIRE(call.operand_pin_names[0] == "b");
    REQUIRE(call.operand_pin_names[1] == "a");
}

TEST_CASE("ir::lower fails when an Input pin has no driver and no default", "[feature][ir]") {
    st::feature::Graph g;
    g.add_node(make_primitive_node(
        "primitive.compare_gt",
        {{"a", st::feature::PinType::Float}, {"b", st::feature::PinType::Float}},
        {{"out", st::feature::PinType::Bool}}));
    auto const m = st::feature::ir::lower(g);
    REQUIRE_FALSE(m.has_value());
    REQUIRE(m.error().message().find("no driver") != std::string::npos);
}

TEST_CASE("ir::lower refuses a graph that fails validate()", "[feature][ir]") {
    // Two-primitive cycle. Graph::validate flags this; ir::lower
    // surfaces the same failure rather than producing garbage IR.
    st::feature::Graph g;
    auto const a =
        g.add_node(make_primitive_node("primitive.a", {{"in", st::feature::PinType::Float}},
                                       {{"out", st::feature::PinType::Float}}));
    auto const b =
        g.add_node(make_primitive_node("primitive.b", {{"in", st::feature::PinType::Float}},
                                       {{"out", st::feature::PinType::Float}}));
    REQUIRE(g.connect(a, 1, b, 0).has_value());
    REQUIRE(g.connect(b, 1, a, 0).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE_FALSE(m.has_value());
}

TEST_CASE("ir::estimate_cost adds per-op cycle counts", "[feature][ir]") {
    // Module from the canonical splice shape (LoadHookInput +
    // CallPrimitive + StoreHookOutput). The `passthrough` primitive
    // is not in the per-symbol cost table → defaults to 3 cycles.
    // Total: 2 + 3 + 2 = 7.
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    auto const mid = g.add_node(make_primitive_node("primitive.passthrough",
                                                    {{"in", st::feature::PinType::Float}},
                                                    {{"out", st::feature::PinType::Float}}));
    auto const snk = g.add_node(make_hook_node(
        "hook.after_fuel_calc", {}, {{"Override fuel PW", st::feature::PinType::Float}}));
    REQUIRE(g.connect(src, 0, mid, 0).has_value());
    REQUIRE(g.connect(mid, 1, snk, 0).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    REQUIRE(st::feature::ir::estimate_cost(*m) == 7);
}

TEST_CASE("ir::estimate_cost is symbol-aware for known primitives", "[feature][ir]") {
    // Build a Module manually so we can pin the symbol-under-test
    // exactly. `divide_int` is the FPU-bridge-via-FDIV primitive
    // priced at 18 cycles in the table.
    st::feature::ir::Module m;

    st::feature::ir::Instruction load_in;
    load_in.op = st::feature::ir::Op::LoadHookInput;
    load_in.result_type = st::feature::PinType::Int;
    load_in.result_id = 1;
    load_in.symbol = "read_rpm";
    load_in.pin_name = "rpm";
    m.instructions.push_back(load_in);

    st::feature::ir::Instruction load_k;
    load_k.op = st::feature::ir::Op::LoadConstant;
    load_k.result_type = st::feature::PinType::Int;
    load_k.result_id = 2;
    load_k.constant_value = 2.0;
    m.instructions.push_back(load_k);

    st::feature::ir::Instruction call;
    call.op = st::feature::ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Int;
    call.result_id = 3;
    call.symbol = "divide_int";
    call.pin_name = "out";
    call.operands = {1, 2};
    m.instructions.push_back(call);

    st::feature::ir::Instruction store;
    store.op = st::feature::ir::Op::StoreHookOutput;
    store.result_type = st::feature::PinType::Int;
    store.symbol = "after_fuel_calc";
    store.pin_name = "commanded_pw_override";
    store.operands = {3};
    m.instructions.push_back(store);

    // LoadHookInput(2) + LoadConstant(1) + CallPrimitive(divide_int=18)
    // + StoreHookOutput(2) = 23.
    REQUIRE(st::feature::ir::estimate_cost(m) == 23);
}

TEST_CASE("ir::estimate_cost prices add_int below divide_int", "[feature][ir]") {
    // Two single-primitive modules differing only in symbol. The
    // table prices add_int as cheap (1 cycle) and divide_int as
    // expensive (FDIV-latency-dominated, 18 cycles). Verifies the
    // ordering rather than exact totals — keeps the test resilient
    // to future cost-table tuning.
    auto build = [](std::string_view sym) {
        st::feature::ir::Module m;
        st::feature::ir::Instruction call;
        call.op = st::feature::ir::Op::CallPrimitive;
        call.result_type = st::feature::PinType::Int;
        call.result_id = 1;
        call.symbol = sym;
        call.pin_name = "out";
        m.instructions.push_back(call);
        return m;
    };

    auto const cheap = st::feature::ir::estimate_cost(build("add_int"));
    auto const heavy = st::feature::ir::estimate_cost(build("divide_int"));
    REQUIRE(cheap < heavy);
    REQUIRE(heavy >= cheap * 10); // FDIV-dominated, expect at least 10× delta
}

TEST_CASE("ir::estimate_cost falls back to default for unknown primitives", "[feature][ir]") {
    // Pack-declared primitives that the codegen doesn't yet handle
    // (e.g. `flex_fuel_scale`) still get a finite price — the
    // default constant — so a graph using them lints without a NaN.
    st::feature::ir::Module m;
    st::feature::ir::Instruction call;
    call.op = st::feature::ir::Op::CallPrimitive;
    call.result_type = st::feature::PinType::Float;
    call.result_id = 1;
    call.symbol = "flex_fuel_scale";
    call.pin_name = "scale";
    m.instructions.push_back(call);
    // Default cost is 3, matching the prior symbol-blind model so
    // unknown primitives don't suddenly become free.
    REQUIRE(st::feature::ir::estimate_cost(m) == 3);
}

TEST_CASE("ir::dump produces a stable human-readable transcript", "[feature][ir]") {
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    auto const snk = g.add_node(make_hook_node(
        "hook.after_fuel_calc", {}, {{"Override fuel PW", st::feature::PinType::Float}}));
    REQUIRE(g.connect(src, 0, snk, 0).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    auto const text = st::feature::ir::dump(*m);
    REQUIRE(text.find("LoadHookInput") != std::string::npos);
    REQUIRE(text.find("StoreHookOutput") != std::string::npos);
    REQUIRE(text.find("@read_rpm.rpm") != std::string::npos);
    REQUIRE(text.find("@after_fuel_calc.Override fuel PW") != std::string::npos);
}

TEST_CASE("ir::lint passes on a clean module under budget", "[feature][ir][lint]") {
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    auto const snk = g.add_node(make_hook_node(
        "hook.after_fuel_calc", {}, {{"Override fuel PW", st::feature::PinType::Float}}));
    REQUIRE(g.connect(src, 0, snk, 0).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    auto const findings = st::feature::ir::lint(*m);
    REQUIRE(findings.empty());
}

TEST_CASE("ir::lint flags an RT-budget overrun", "[feature][ir][lint]") {
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    auto const snk = g.add_node(make_hook_node(
        "hook.after_fuel_calc", {}, {{"Override fuel PW", st::feature::PinType::Float}}));
    REQUIRE(g.connect(src, 0, snk, 0).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    // Canonical splice = 2+2 = 4 cycles. Budget = 1 forces a flag.
    auto const findings = st::feature::ir::lint(*m, st::feature::ir::LintOptions{1});
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].message.find("exceeds budget") != std::string::npos);
    REQUIRE_FALSE(findings[0].instruction_index.has_value());
}

TEST_CASE("ir::lint flags duplicate StoreHookOutput", "[feature][ir][lint]") {
    // Two source-only hooks → one shared sink hook with TWO Input
    // pins of the same name forces the lowerer to emit two stores
    // to the same (symbol, pin_name). The lint should flag the
    // second occurrence and point at its instruction index.
    st::feature::Graph g;
    auto const src_a =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm_a", st::feature::PinType::Float}}, {}));
    auto const src_b =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm_b", st::feature::PinType::Float}}, {}));
    st::feature::Node snk;
    snk.kind = "hook.after_fuel_calc";
    snk.label = "after_fuel_calc";
    snk.is_phase_break = true;
    snk.pins.push_back(st::feature::Pin{0, "Override fuel PW", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    snk.pins.push_back(st::feature::Pin{1, "Override fuel PW", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    auto const snk_id = g.add_node(std::move(snk));
    REQUIRE(g.connect(src_a, 0, snk_id, 0).has_value());
    REQUIRE(g.connect(src_b, 0, snk_id, 1).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    auto const findings = st::feature::ir::lint(*m, st::feature::ir::LintOptions{0}); // budget off
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].message.find("duplicate store") != std::string::npos);
    REQUIRE(findings[0].instruction_index.has_value());
}

TEST_CASE("ir::lint budget=0 disables the RT-budget check", "[feature][ir][lint]") {
    st::feature::Graph g;
    auto const src =
        g.add_node(make_hook_node("hook.read_rpm", {{"rpm", st::feature::PinType::Float}}, {}));
    auto const snk = g.add_node(make_hook_node(
        "hook.after_fuel_calc", {}, {{"Override fuel PW", st::feature::PinType::Float}}));
    REQUIRE(g.connect(src, 0, snk, 0).has_value());
    auto const m = st::feature::ir::lower(g);
    REQUIRE(m.has_value());
    auto const findings = st::feature::ir::lint(*m, st::feature::ir::LintOptions{0});
    REQUIRE(findings.empty());
}
