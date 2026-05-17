// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature_ir.hpp"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace st::feature::ir {

char const *op_name(Op op) noexcept {
    switch (op) {
        case Op::LoadConstant:    return "LoadConstant";
        case Op::LoadHookInput:   return "LoadHookInput";
        case Op::CallPrimitive:   return "CallPrimitive";
        case Op::StoreHookOutput: return "StoreHookOutput";
    }
    return "?";
}

namespace {

// Strip the "hook." / "primitive." kind prefix from a Node.kind
// before storing it as the IR symbol. Robust to either prefix or
// neither (debug nodes from the no-pack fallback).
std::string_view strip_kind_prefix(std::string const &kind) {
    if (kind.starts_with("hook."))      return std::string_view{kind}.substr(5);
    if (kind.starts_with("primitive.")) return std::string_view{kind}.substr(10);
    return std::string_view{kind};
}

// Index helper: stable position of a node in nodes() so we can fold
// edges into adjacency lists indexed by position rather than
// chasing NodeIds through find-loops.
std::size_t index_of(std::vector<Node> const &nodes, NodeId id) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) return i;
    }
    return nodes.size();   // sentinel: not found
}

// Topological sort that respects the same phase-break semantics as
// Graph::validate (see feature.cpp). Each non-phase-break node is
// one vertex; each phase-break (hook) node is two vertices —
// output-side (index*2) and input-side (index*2 + 1), with NO
// internal edge so the output-side can be scheduled FIRST and the
// input-side LAST. The resulting order has every consumer after its
// producer, which is exactly what straight-line dataflow IR needs.
//
// Returns a vector of (node_index, is_input_side) pairs in
// emit-order. For non-phase-break nodes, only the (index, false)
// pair appears once; for phase-break, (index, false) appears first
// in the output-side wave and (index, true) appears later in the
// input-side wave.
struct SchedSlot {
    std::size_t node_index{};
    bool        is_input_side{false};
};

std::vector<SchedSlot> schedule(Graph const &g) {
    auto const &nodes = g.nodes();
    auto const &edges = g.edges();
    std::size_t const nv = nodes.size() * 2;
    auto const output_v = [](std::size_t i) { return i * 2; };
    auto const input_v  = [](std::size_t i) { return i * 2 + 1; };
    std::vector<std::vector<std::size_t>> adj(nv);
    std::vector<std::size_t>              indeg(nv, 0);
    for (auto const &e : edges) {
        auto const a = index_of(nodes, e.from_node);
        auto const b = index_of(nodes, e.to_node);
        if (a >= nodes.size() || b >= nodes.size()) continue;
        adj[output_v(a)].push_back(input_v(b));
        ++indeg[input_v(b)];
    }
    // For non-phase-break nodes the output-side depends on the
    // input-side (output is a pure function of inputs at time T).
    // Add this internal edge so toposort produces the natural
    // "consume inputs, then produce outputs" order.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].is_phase_break) {
            adj[input_v(i)].push_back(output_v(i));
            ++indeg[output_v(i)];
        }
    }
    std::vector<SchedSlot> order;
    order.reserve(nv);
    std::vector<std::size_t> queue;
    for (std::size_t v = 0; v < nv; ++v) {
        if (indeg[v] == 0) queue.push_back(v);
    }
    while (!queue.empty()) {
        std::size_t const v = queue.back();
        queue.pop_back();
        order.push_back({v / 2, (v % 2) == 1});
        for (auto w : adj[v]) {
            if (--indeg[w] == 0) queue.push_back(w);
        }
    }
    return order;
}

} // namespace

Result<Module> lower(Graph const &g) {
    if (auto v = g.validate(); !v.has_value()) {
        return failure(v.error());
    }

    auto const &nodes = g.nodes();
    auto const &edges = g.edges();
    Module      m;

    // Locate the producing pin for a given (consumer_node, consumer_pin)
    // — at most one entry per Input pin per the single-driver invariant
    // that connect() enforces.
    auto const find_driver = [&](NodeId nid, PinId pid)
        -> std::optional<std::pair<NodeId, PinId>> {
        for (auto const &e : edges) {
            if (e.to_node == nid && e.to_pin == pid) {
                return std::make_pair(e.from_node, e.from_pin);
            }
        }
        return std::nullopt;
    };

    // ValueId allocator + map. `value_of[(producer_node, producer_pin)]`
    // returns the SSA id that holds the value the producer emits. Set
    // when an Output-pin's emitter runs (LoadHookInput on read-side of
    // a hook, or CallPrimitive on a primitive's output emission).
    ValueId next_id = 1;
    auto const make_key = [](NodeId nid, PinId pid) {
        return (static_cast<std::uint64_t>(nid) << 32)
             | static_cast<std::uint64_t>(pid);
    };
    std::unordered_map<std::uint64_t, ValueId> value_of;

    auto const sched = schedule(g);

    for (auto const &slot : sched) {
        auto const &n      = nodes[slot.node_index];
        bool const  is_in  = slot.is_input_side;
        bool const  is_hk  = n.is_phase_break;
        std::string const sym{strip_kind_prefix(n.kind)};

        if (is_hk && !is_in) {
            // Hook output-side: emit one LoadHookInput per Output
            // pin that ever feeds something (or always — emitting
            // dead loads is fine, codegen DCE can prune). For
            // simplicity emit unconditionally so the symbol/pin
            // mapping is stable across edits.
            for (auto const &p : n.pins) {
                if (p.direction != PinDirection::Output) continue;
                Instruction ins;
                ins.op          = Op::LoadHookInput;
                ins.result_type = p.type;
                ins.result_id   = next_id++;
                ins.symbol      = sym;
                ins.pin_name    = p.name;
                value_of[make_key(n.id, p.id)] = ins.result_id;
                m.instructions.push_back(std::move(ins));
            }
        } else if (is_hk && is_in) {
            // Hook input-side: emit StoreHookOutput per Input pin
            // that has a value source. Edge-driven first; defaulted
            // pins emit LoadConstant + StoreHookOutput.
            for (auto const &p : n.pins) {
                if (p.direction != PinDirection::Input) continue;
                ValueId src = 0;
                if (auto driver = find_driver(n.id, p.id);
                    driver.has_value()) {
                    auto it = value_of.find(make_key(driver->first,
                                                       driver->second));
                    if (it == value_of.end()) {
                        return failure(ErrorCode::InvalidArgument,
                                       "ir::lower: hook input pin '"
                                           + p.name
                                           + "' driver not yet "
                                             "scheduled (graph "
                                             "topology bug?)");
                    }
                    src = it->second;
                } else if (p.default_value.has_value()) {
                    Instruction k;
                    k.op             = Op::LoadConstant;
                    k.result_type    = p.type;
                    k.result_id      = next_id++;
                    k.constant_value = *p.default_value;
                    src              = k.result_id;
                    m.instructions.push_back(std::move(k));
                } else {
                    // Override pin with no source. lint warns; we
                    // refuse — codegen can't proceed.
                    return failure(ErrorCode::InvalidArgument,
                                   "ir::lower: hook input pin '"
                                       + p.name + "' on '" + sym
                                       + "' has no driver and no "
                                         "default_value");
                }
                Instruction s;
                s.op       = Op::StoreHookOutput;
                s.symbol   = sym;
                s.pin_name = p.name;
                s.operands = {src};
                m.instructions.push_back(std::move(s));
            }
        } else {
            // Primitive (or generic debug) node. The scheduler
            // visits non-phase-break nodes twice (once per side)
            // because the bipartite topo expansion adds an
            // internal input→output edge to enforce read-before-
            // write order. Emit only on the output-side visit so
            // each node turns into exactly one set of IR ops.
            if (is_in) continue;
            std::vector<ValueId>     operands;
            std::vector<std::string> operand_pin_names;
            operands.reserve(n.pins.size());
            operand_pin_names.reserve(n.pins.size());
            for (auto const &p : n.pins) {
                if (p.direction != PinDirection::Input) continue;
                ValueId src = 0;
                if (auto driver = find_driver(n.id, p.id);
                    driver.has_value()) {
                    auto it = value_of.find(make_key(driver->first,
                                                       driver->second));
                    if (it == value_of.end()) {
                        return failure(ErrorCode::InvalidArgument,
                                       "ir::lower: primitive input "
                                       "pin '" + p.name
                                           + "' driver not yet "
                                             "scheduled");
                    }
                    src = it->second;
                } else if (p.default_value.has_value()) {
                    Instruction k;
                    k.op             = Op::LoadConstant;
                    k.result_type    = p.type;
                    k.result_id      = next_id++;
                    k.constant_value = *p.default_value;
                    src              = k.result_id;
                    m.instructions.push_back(std::move(k));
                } else {
                    return failure(ErrorCode::InvalidArgument,
                                   "ir::lower: primitive input pin '"
                                       + p.name + "' on '" + sym
                                       + "' has no driver and no "
                                         "default_value");
                }
                operands.push_back(src);
                operand_pin_names.push_back(p.name);
            }
            for (auto const &p : n.pins) {
                if (p.direction != PinDirection::Output) continue;
                Instruction c;
                c.op                = Op::CallPrimitive;
                c.result_type       = p.type;
                c.result_id         = next_id++;
                c.symbol            = sym;
                c.pin_name          = p.name;
                c.operands          = operands;
                c.operand_pin_names = operand_pin_names;
                value_of[make_key(n.id, p.id)] = c.result_id;
                m.instructions.push_back(std::move(c));
            }
        }
    }

    return m;
}

std::string dump(Module const &m) {
    std::ostringstream out;
    for (auto const &i : m.instructions) {
        if (i.result_id != 0) {
            out << "%" << i.result_id << " = ";
        }
        out << op_name(i.op);
        if (i.op == Op::LoadConstant && i.constant_value.has_value()) {
            out << ' ' << *i.constant_value;
        }
        if (!i.operands.empty()) {
            out << ' ';
            bool const named =
                i.operand_pin_names.size() == i.operands.size();
            for (std::size_t k = 0; k < i.operands.size(); ++k) {
                if (k != 0) out << ", ";
                if (named) out << i.operand_pin_names[k] << '=';
                out << '%' << i.operands[k];
            }
        }
        if (!i.symbol.empty()) {
            out << " @" << i.symbol << '.' << i.pin_name;
        }
        out << '\n';
    }
    out << "; estimated cost: " << estimate_cost(m) << " cycles\n";
    return out.str();
}

std::size_t estimate_cost(Module const &m) noexcept {
    std::size_t total = 0;
    for (auto const &i : m.instructions) {
        switch (i.op) {
            case Op::LoadConstant:    total += 1; break;
            case Op::LoadHookInput:   total += 2; break;
            case Op::CallPrimitive:   total += 3; break;
            case Op::StoreHookOutput: total += 2; break;
        }
    }
    return total;
}

std::vector<LintFinding> lint(Module const &m, LintOptions opts) {
    std::vector<LintFinding> findings;

    // Duplicate StoreHookOutput targeting the same (symbol, pin_name).
    // Codegen would emit two writes to the same override slot — second
    // wins, first is dead, but the user almost certainly intended a
    // different pin or forgot to merge logic upstream.
    std::vector<std::size_t> first_store_at;   // parallel to seen
    std::vector<std::pair<std::string, std::string>> seen;
    for (std::size_t i = 0; i < m.instructions.size(); ++i) {
        auto const &ins = m.instructions[i];
        if (ins.op != Op::StoreHookOutput) continue;
        bool duplicate = false;
        for (std::size_t k = 0; k < seen.size(); ++k) {
            if (seen[k].first == ins.symbol
                && seen[k].second == ins.pin_name) {
                findings.push_back({
                    "duplicate store to '" + ins.symbol + '.'
                        + ins.pin_name + "' (first at instruction "
                        + std::to_string(first_store_at[k]) + ')',
                    i});
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            seen.push_back({ins.symbol, ins.pin_name});
            first_store_at.push_back(i);
        }
    }

    // RT-budget. Whole-module estimate against the configured budget.
    // Per-hook breakdown comes later — needs codegen-side cycle counts
    // to be meaningful.
    if (opts.budget_cycles > 0) {
        auto const cost = estimate_cost(m);
        if (cost > opts.budget_cycles) {
            findings.push_back({
                "estimated cost " + std::to_string(cost)
                    + " cycles exceeds budget "
                    + std::to_string(opts.budget_cycles),
                std::nullopt});
        }
    }

    return findings;
}

} // namespace st::feature::ir
