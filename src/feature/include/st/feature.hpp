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
    PinId id{};
    // Canonical identifier — must match the pack's HookSignal `name`
    // (snake_case by convention) so the codegen can resolve the pin
    // back to its firmware address. NEVER use the display label here;
    // mismatched name vs canonical = silent codegen lookup failure.
    std::string name;
    PinType type{PinType::Float};
    PinDirection direction{PinDirection::Input};
    // Optional engineering unit ("rpm", "kPa", "°C", "ms", ...). Free-
    // form for now; the unit-checking pass in a later phase will
    // formalize the small set of canonical names a Graph may use.
    std::string unit;
    // Human-readable display label ("Throttle %", "Engine RPM").
    // Optional — when empty, displays fall back to `name`. The pack's
    // HookSignal `label` field flows into this on Insert. Decoupling
    // it from `name` lets the canvas show pretty labels while the
    // codegen lookup stays on canonical names.
    std::string label;
    // Per-instance constant supplied to an Input pin when no edge
    // drives it. Stored as double regardless of PinType (Int casts,
    // Bool reads `value > 0.5`) so the wire format stays uniform.
    // Meaningless on Output pins — the editor only surfaces the
    // editor for Input pins. Empty == undefined; lint flags an
    // empty default + no driver as "not driven".
    std::optional<double> default_value;

    // Constructor accepts the historical 5-arg shape (id, name, type,
    // direction, unit) plus an optional sixth label. Existing call
    // sites that pass five args still compile; new code spells out the
    // label explicitly. Default and copy/move stay synthesized.
    Pin() = default;
    Pin(PinId id_, std::string name_, PinType type_, PinDirection direction_, std::string unit_,
        std::string label_ = "")
        : id(id_),
          name(std::move(name_)),
          type(type_),
          direction(direction_),
          unit(std::move(unit_)),
          label(std::move(label_)) {}
};

struct Node {
    NodeId id{};
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
    float x{0.0f};
    float y{0.0f};
    std::vector<Pin> pins;
    // Phase-break flag: true when the node's Input side fires at a
    // strictly later time than its Output side within one ECU loop
    // iteration. Hooks set this — their Output pins emit ECU state
    // at the splice point (time T) while their Input pins consume
    // user-overridden values that the ECU applies after the user's
    // logic ran (time T+ε). Cycle detection treats these as two
    // vertices with no internal edge, so the legitimate pattern
    // `hook.out → user_logic → hook.in` is not a cycle. Primitives
    // and generic debug nodes leave this false: their output is a
    // pure function of their inputs at the same time, so a path
    // from a primitive's output back to its input IS a cycle.
    bool is_phase_break{false};
};

struct Edge {
    NodeId from_node{};
    PinId from_pin{};
    NodeId to_node{};
    PinId to_pin{};
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
    [[nodiscard]] Status connect(NodeId from_node, PinId from_pin, NodeId to_node, PinId to_pin);

    // Remove a node and every edge touching it. Idempotent on
    // non-existent ids.
    void remove_node(NodeId id);

    // Update a node's editor position. Idempotent on unknown ids.
    // The renderer calls this when the user drags a node so the new
    // position persists into the graph data model rather than living
    // in transient editor state.
    void set_node_position(NodeId id, float x, float y);

    // Set or clear the per-instance default value on a pin. Used by
    // the editor when the user types a value into the pin's right-
    // click popup. Idempotent on unknown ids. The value is stored
    // as double regardless of pin type — Int truncates, Bool reads
    // `v > 0.5` — keeping the persisted wire format uniform.
    void set_pin_default(NodeId id, PinId pin_id, std::optional<double> value);

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
    [[nodiscard]] std::vector<Node> const &nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::vector<Edge> const &edges() const noexcept {
        return edges_;
    }
    [[nodiscard]] Node const *find_node(NodeId id) const noexcept;

    // Locate a pin on a node by id. Returns nullptr if either id is
    // unknown. Const-only — pin mutation goes through add_node /
    // remove_node so external code can't desynchronize edges.
    [[nodiscard]] Pin const *find_pin(NodeId node_id, PinId pin_id) const noexcept;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    NodeId next_node_id_{1};
};

// Canonical short label for a PinType, suitable for tooltips and
// debug output. Owning the mapping here forces a compile-time switch
// update when a new PinType lands.
[[nodiscard]] char const *pin_type_name(PinType t) noexcept;

// Inverse of pin_type_name. Returns nullopt on unknown text — the
// loader uses this to validate persisted graphs.
[[nodiscard]] std::optional<PinType> parse_pin_type(std::string_view s) noexcept;

// Persistence (a minimal slice of the docs/16 `.stmod` format —
// graph only, no compiled patch bytes yet). Round-trip stable for
// every valid graph. Schema-versioned via [graph].schema_version so
// future-compatible changes can be detected at load time.
[[nodiscard]] std::string to_toml(Graph const &g);
[[nodiscard]] Result<Graph> from_toml(std::string_view text);

// Style / completeness warnings — distinct from validate(), which
// reports STRUCTURAL violations (cycles, dangling refs) that make a
// graph unrepresentable. Lint findings flag shapes that are valid
// but probably incomplete: input pins with no driver, output pins
// with no consumer, etc. Editor surfaces them as warnings; codegen
// later will refuse on those it considers fatal.
struct LintFinding {
    std::string message;
    std::optional<NodeId> node;
    std::optional<PinId> pin;
};

[[nodiscard]] std::vector<LintFinding> lint(Graph const &g);

} // namespace st::feature

#endif // ST_FEATURE_HPP
