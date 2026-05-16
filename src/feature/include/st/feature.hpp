// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::feature — Phase 5 custom-features graph (data model only).
//
// This is the in-memory data structure for the node-graph editor
// described in docs/16-custom-features.md. Phase 5 work; the codegen,
// IR, and patch-format layers all live in separate sibling modules
// that don't exist yet.
//
// Designed from first principles per the clean-room methodology in
// docs/15-clean-room-engineering.md and docs/16 §"Stance on third-
// party prior art". No node taxonomy, file format, or IR design was
// taken from any external tool — the structure here is deliberately
// minimal and tool-agnostic.
//
// Scope of this header (what's stable today):
//   - Pin: name, type, optional unit, direction
//   - Node: id, kind tag (a free-form string supplied by the
//     definition pack — keeps the tool agnostic about taxonomy), pins,
//     2D position for the editor's canvas
//   - Edge: directed connection between an output pin on one node and
//     an input pin on another
//   - Graph: collection + topology queries + structural validation
//
// Out of scope (lands in later modules / sessions):
//   - Type system (today PinType is an enum; later it grows into a
//     unit-checked dataflow type)
//   - IR lowering, codegen, patch format, RAM allocator, linter
//   - Persistence (.stmod format) — Graph is round-trippable as
//     plain C++ for now; TOML I/O follows once the shape settles

#ifndef ST_FEATURE_HPP
#define ST_FEATURE_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::feature {

// Stable identifier for a node within a Graph. Monotonically assigned
// by Graph::add_node; never re-used within a Graph's lifetime so
// existing edges stay valid across node deletions.
using NodeId = std::uint32_t;

// Stable identifier for a pin within a Node. Like NodeId but scoped
// per-node (each Node has its own pin-id space starting from 0).
using PinId = std::uint16_t;

// Pin types. Deliberately small — this is the seed of the type
// system described in docs/16, not its final form. Float covers all
// continuous sensor + actuator values; the integer / boolean variants
// are useful for state-machine logic and discrete I/O.
enum class PinType : std::uint8_t {
    Float,
    Int,
    Bool,
};

// Direction qualifier on a Pin. A given Node may carry both Input and
// Output pins; edges always run Output→Input.
enum class PinDirection : std::uint8_t {
    Input,
    Output,
};

struct Pin {
    PinId        id{};
    std::string  name;
    PinType      type{PinType::Float};
    PinDirection direction{PinDirection::Input};
    // Optional engineering unit ("rpm", "kPa", "°C", "ms", ...). Free-
    // form for now; the unit-checking pass in a later phase will
    // formalize the small set of canonical names a Graph may use.
    std::string  unit;
};

struct Node {
    NodeId      id{};
    // Free-form tag identifying what this node "does". The editor
    // and the eventual compiler use this to look up the node's
    // behavior in the per-platform node library (see docs/16
    // §"Node library"). Keeping it a string means the tool layer
    // doesn't bake in a node taxonomy — packs declare what's
    // available.
    std::string kind;
    // Display label for the editor; defaults to `kind` when blank.
    std::string label;
    // 2D position on the editor canvas. Persisted with the graph so a
    // user's hand-laid-out diagram survives a reload.
    float       x{0.0f};
    float       y{0.0f};
    std::vector<Pin> pins;
};

struct Edge {
    NodeId from_node{};
    PinId  from_pin{};
    NodeId to_node{};
    PinId  to_pin{};
};

class Graph {
  public:
    // Add a node; assigns and returns a fresh NodeId. The node's id
    // field is ignored on input and overwritten.
    NodeId add_node(Node node);

    // Connect from (from_node, from_pin) to (to_node, to_pin).
    // Validates: both nodes exist, both pins exist, directions match
    // (Output → Input), types match, and the to_pin is not already
    // driven by another edge (single-driver invariant — fan-in into
    // an input is disallowed; fan-out from an output is fine).
    [[nodiscard]] Status connect(NodeId from_node, PinId from_pin,
                                  NodeId to_node,   PinId to_pin);

    // Remove a node and every edge touching it. Idempotent on
    // non-existent ids.
    void remove_node(NodeId id);

    // Remove a specific edge. Idempotent.
    void remove_edge(Edge const &edge);

    // Topological invariants the editor and compiler both rely on:
    // - No cycles (compiler emits straight-line dataflow per loop
    //   iteration; cycles would require explicit state-machine
    //   nodes we don't yet model).
    // - Every connected input pin matches its driver's output type.
    // Returns Ok or a ParseError naming the first violation found.
    [[nodiscard]] Status validate() const;

    // Read-only views.
    [[nodiscard]] std::vector<Node> const &nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::vector<Edge> const &edges() const noexcept { return edges_; }
    [[nodiscard]] Node const *find_node(NodeId id) const noexcept;

    // Locate a pin on a node by id. Returns nullptr if either id is
    // unknown. Const-only — pin mutation goes through add_node /
    // remove_node so external code can't desynchronize edges.
    [[nodiscard]] Pin const *find_pin(NodeId node_id, PinId pin_id) const noexcept;

  private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    NodeId            next_node_id_{1};
};

// Canonical short label for a PinType, suitable for tooltips and
// debug output. Owning the mapping here forces a compile-time switch
// update when a new PinType lands.
[[nodiscard]] char const *pin_type_name(PinType t) noexcept;

} // namespace st::feature

#endif // ST_FEATURE_HPP
