// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

st::feature::Node make_source_node(char const *kind, st::feature::PinType t) {
    st::feature::Node n;
    n.kind = kind;
    n.label = kind;
    n.pins.push_back(st::feature::Pin{0, "out", t, st::feature::PinDirection::Output, ""});
    return n;
}

st::feature::Node make_sink_node(char const *kind, st::feature::PinType t) {
    st::feature::Node n;
    n.kind = kind;
    n.label = kind;
    n.pins.push_back(st::feature::Pin{0, "in", t, st::feature::PinDirection::Input, ""});
    return n;
}

} // namespace

TEST_CASE("Graph assigns monotonic node ids", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    auto const b = g.add_node(make_source_node("load", st::feature::PinType::Float));
    auto const c = g.add_node(make_sink_node("out", st::feature::PinType::Float));
    REQUIRE(a != b);
    REQUIRE(b != c);
    REQUIRE(g.nodes().size() == 3);
}

TEST_CASE("Graph::connect rejects unit mismatch on otherwise-compatible pins", "[feature][graph]") {
    st::feature::Graph g;
    st::feature::Node src;
    src.kind = "sensor.rpm";
    src.label = "rpm";
    src.pins.push_back(st::feature::Pin{0, "out", st::feature::PinType::Float,
                                        st::feature::PinDirection::Output, "rpm"});
    auto const a = g.add_node(std::move(src));
    st::feature::Node snk;
    snk.kind = "hook.set_load_target";
    snk.label = "set_load_target";
    snk.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, "%"});
    auto const b = g.add_node(std::move(snk));
    auto const r = g.connect(a, 0, b, 0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message().find("unit mismatch") != std::string::npos);
}

TEST_CASE("Graph::connect accepts matching units", "[feature][graph]") {
    st::feature::Graph g;
    st::feature::Node src;
    src.kind = "sensor.rpm";
    src.pins.push_back(st::feature::Pin{0, "out", st::feature::PinType::Float,
                                        st::feature::PinDirection::Output, "rpm"});
    auto const a = g.add_node(std::move(src));
    st::feature::Node snk;
    snk.kind = "hook.set_rpm";
    snk.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, "rpm"});
    auto const b = g.add_node(std::move(snk));
    REQUIRE(g.connect(a, 0, b, 0).has_value());
}

TEST_CASE("Graph::connect treats an empty unit as unit-agnostic", "[feature][graph]") {
    // Generic math nodes don't declare units. Connecting a
    // unit-bearing sensor output to such a node should succeed —
    // otherwise pack-driven hook nodes couldn't feed any generic
    // arithmetic without explicit conversion plumbing.
    st::feature::Graph g;
    st::feature::Node src;
    src.kind = "sensor.rpm";
    src.pins.push_back(st::feature::Pin{0, "out", st::feature::PinType::Float,
                                        st::feature::PinDirection::Output, "rpm"});
    auto const a = g.add_node(std::move(src));
    st::feature::Node snk;
    snk.kind = "math.passthrough";
    snk.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    auto const b = g.add_node(std::move(snk));
    REQUIRE(g.connect(a, 0, b, 0).has_value());
}

TEST_CASE("Graph::connect rejects type mismatch", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("counter", st::feature::PinType::Int));
    auto const r = g.connect(a, 0, b, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::connect rejects fan-in into a single input", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    auto const b = g.add_node(make_source_node("b", st::feature::PinType::Float));
    auto const c = g.add_node(make_sink_node("c", st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, c, 0).has_value());
    auto const r = g.connect(b, 0, c, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::connect rejects wrong-direction wires", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("b", st::feature::PinType::Float));
    // Input -> Output is invalid.
    auto const r = g.connect(b, 0, a, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::remove_node cleans up incident edges", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("b", st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, b, 0).has_value());
    REQUIRE(g.edges().size() == 1);
    g.remove_node(a);
    REQUIRE(g.edges().empty());
    REQUIRE(g.nodes().size() == 1);
}

TEST_CASE("Graph::validate flags a two-node cycle", "[feature][graph]") {
    // Build a node with both an input and an output of the same type,
    // then wire the output back to its own input. This requires
    // crafting nodes by hand because make_source/make_sink only give
    // one pin each.
    st::feature::Node n;
    n.kind = "passthrough";
    n.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                      st::feature::PinDirection::Input, ""});
    n.pins.push_back(st::feature::Pin{1, "out", st::feature::PinType::Float,
                                      st::feature::PinDirection::Output, ""});
    st::feature::Graph g;
    auto const a = g.add_node(n);
    auto const b = g.add_node(n);
    REQUIRE(g.connect(a, 1, b, 0).has_value());
    REQUIRE(g.connect(b, 1, a, 0).has_value());
    auto const r = g.validate();
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::set_node_position updates position", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    g.set_node_position(a, 123.5f, 456.25f);
    auto const *n = g.find_node(a);
    REQUIRE(n != nullptr);
    REQUIRE(n->x == 123.5f);
    REQUIRE(n->y == 456.25f);
    // Idempotent on unknown ids.
    g.set_node_position(static_cast<st::feature::NodeId>(99999), 1.0f, 2.0f);
    REQUIRE(g.nodes().size() == 1);
}

TEST_CASE("Graph::validate accepts a clean DAG", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    auto const b = g.add_node(make_source_node("load", st::feature::PinType::Float));
    auto const c = g.add_node(make_sink_node("out", st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, c, 0).has_value());
    // Two outputs feeding one input is *not* fan-in; b's edge would
    // duplicate the driver invariant. Use a separate sink instead.
    auto const d = g.add_node(make_sink_node("out2", st::feature::PinType::Float));
    REQUIRE(g.connect(b, 0, d, 0).has_value());
    REQUIRE(g.validate().has_value());
}

TEST_CASE("Graph TOML round-trip preserves shape", "[feature][toml]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("out", st::feature::PinType::Float));
    g.set_node_position(a, 12.5f, 24.0f);
    g.set_node_position(b, 200.0f, 80.5f);
    REQUIRE(g.connect(a, 0, b, 0).has_value());

    auto const text = st::feature::to_toml(g);
    REQUIRE_FALSE(text.empty());

    auto loaded = st::feature::from_toml(text);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->nodes().size() == 2);
    REQUIRE(loaded->edges().size() == 1);
    // Positions survive — even though node ids may have been
    // remapped, the order is preserved.
    REQUIRE(loaded->nodes()[0].x == 12.5f);
    REQUIRE(loaded->nodes()[1].y == 80.5f);
    REQUIRE(loaded->validate().has_value());
}

TEST_CASE("Graph from_toml rejects unknown pin type", "[feature][toml]") {
    std::string const bad =
        "[graph]\n"
        "schema_version = 1\n\n"
        "[[node]]\n"
        "id    = 1\n"
        "kind  = \"x\"\n"
        "label = \"x\"\n"
        "x     = 0.0\n"
        "y     = 0.0\n"
        "pins  = [\n"
        "  { id = 0, name = \"o\", type = \"complex\", direction = \"output\", unit = \"\" },\n"
        "]\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects missing schema_version", "[feature][toml]") {
    std::string const bad = "[graph]\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects empty kind", "[feature][toml]") {
    std::string const bad = "[graph]\nschema_version = 1\n\n"
                            "[[node]]\n"
                            "id = 1\nkind = \"\"\nlabel = \"x\"\nx = 0.0\ny = 0.0\n"
                            "pins = []\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects missing kind key", "[feature][toml]") {
    std::string const bad = "[graph]\nschema_version = 1\n\n"
                            "[[node]]\n"
                            "id = 1\nlabel = \"x\"\nx = 0.0\ny = 0.0\n"
                            "pins = []\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects duplicate node id", "[feature][toml]") {
    std::string const bad = "[graph]\nschema_version = 1\n\n"
                            "[[node]]\nid = 1\nkind = \"a\"\nlabel = \"\"\nx = 0.0\ny = 0.0\n"
                            "pins = []\n\n"
                            "[[node]]\nid = 1\nkind = \"b\"\nlabel = \"\"\nx = 0.0\ny = 0.0\n"
                            "pins = []\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects pin with empty name", "[feature][toml]") {
    std::string const bad =
        "[graph]\nschema_version = 1\n\n"
        "[[node]]\n"
        "id = 1\nkind = \"x\"\nlabel = \"x\"\nx = 0.0\ny = 0.0\n"
        "pins = [\n"
        "  { id = 0, name = \"\", type = \"float\", direction = \"output\", unit = \"\" },\n"
        "]\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects duplicate pin id within a node", "[feature][toml]") {
    std::string const bad =
        "[graph]\nschema_version = 1\n\n"
        "[[node]]\n"
        "id = 1\nkind = \"x\"\nlabel = \"x\"\nx = 0.0\ny = 0.0\n"
        "pins = [\n"
        "  { id = 0, name = \"a\", type = \"float\", direction = \"input\",  unit = \"\" },\n"
        "  { id = 0, name = \"b\", type = \"float\", direction = \"output\", unit = \"\" },\n"
        "]\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::validate accepts hook.out -> middle -> hook.in "
          "when the hook is a phase break",
          "[feature][graph]") {
    // This pattern is the most common shape for a custom feature:
    // read an ECU value from a splice, transform it, write the
    // override back to the same splice. Topologically that's a
    // 2-cycle through the hook node, but the hook's read-side
    // fires strictly before its write-side, so it's a legitimate
    // DAG in execution-time terms. The phase-break flag tells the
    // validator to model the hook as two separate vertices.
    st::feature::Graph g;
    st::feature::Node hook;
    hook.kind = "hook.after_fuel_calc";
    hook.label = "hook";
    hook.is_phase_break = true;
    hook.pins.push_back(st::feature::Pin{0, "out", st::feature::PinType::Float,
                                         st::feature::PinDirection::Output, ""});
    hook.pins.push_back(st::feature::Pin{1, "in", st::feature::PinType::Float,
                                         st::feature::PinDirection::Input, ""});
    auto const h = g.add_node(std::move(hook));
    st::feature::Node mid;
    mid.kind = "primitive.passthrough";
    mid.label = "mid";
    mid.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                        st::feature::PinDirection::Input, ""});
    mid.pins.push_back(st::feature::Pin{1, "out", st::feature::PinType::Float,
                                        st::feature::PinDirection::Output, ""});
    auto const m = g.add_node(std::move(mid));
    REQUIRE(g.connect(h, 0, m, 0).has_value());
    REQUIRE(g.connect(m, 1, h, 1).has_value());
    REQUIRE(g.validate().has_value());
}

TEST_CASE("Graph::validate still flags a real cycle through primitives", "[feature][graph]") {
    // Same shape but with the middle node also marked phase-break-
    // free (a primitive). The hook stays phase-break; the cycle is
    // through two primitives so it should fire.
    st::feature::Graph g;
    st::feature::Node a;
    a.kind = "primitive.a";
    a.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                      st::feature::PinDirection::Input, ""});
    a.pins.push_back(st::feature::Pin{1, "out", st::feature::PinType::Float,
                                      st::feature::PinDirection::Output, ""});
    auto const aid = g.add_node(std::move(a));
    st::feature::Node b;
    b.kind = "primitive.b";
    b.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                      st::feature::PinDirection::Input, ""});
    b.pins.push_back(st::feature::Pin{1, "out", st::feature::PinType::Float,
                                      st::feature::PinDirection::Output, ""});
    auto const bid = g.add_node(std::move(b));
    REQUIRE(g.connect(aid, 1, bid, 0).has_value());
    REQUIRE(g.connect(bid, 1, aid, 0).has_value());
    auto const v = g.validate();
    REQUIRE_FALSE(v.has_value());
    REQUIRE(v.error().message().find("cycle") != std::string::npos);
}

TEST_CASE("lint on an empty graph reports nothing", "[feature][lint]") {
    st::feature::Graph g;
    REQUIRE(st::feature::lint(g).empty());
}

TEST_CASE("lint flags an orphan node that has any input pin", "[feature][lint]") {
    // A truly-disconnected node with an input is almost always a
    // mistake. Surfaces as a single 'no connections' message
    // rather than amplifying into one warning per undriven pin.
    st::feature::Graph g;
    g.add_node(make_sink_node("hook.commit", st::feature::PinType::Float));
    auto const findings = st::feature::lint(g);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].node.has_value());
    REQUIRE_FALSE(findings[0].pin.has_value());
    REQUIRE(findings[0].message.find("no connections") != std::string::npos);
}

TEST_CASE("lint clears once the input pin is driven", "[feature][lint]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("hook", st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, b, 0).has_value());
    REQUIRE(st::feature::lint(g).empty());
}

TEST_CASE("lint treats an undriven input with default_value as driven", "[feature][lint]") {
    // Per-instance constants supply a value when no edge is wired,
    // so the lint should NOT flag the pin as undriven.
    st::feature::Graph g;
    st::feature::Node n;
    n.kind = "primitive.compare_gt";
    n.label = "rpm > Y";
    n.pins.push_back(st::feature::Pin{0, "a", st::feature::PinType::Float,
                                      st::feature::PinDirection::Input, ""});
    st::feature::Pin b{1, "b", st::feature::PinType::Float, st::feature::PinDirection::Input, ""};
    b.default_value = 4000.0;
    n.pins.push_back(std::move(b));
    n.pins.push_back(st::feature::Pin{2, "out", st::feature::PinType::Bool,
                                      st::feature::PinDirection::Output, ""});
    auto const cid = g.add_node(std::move(n));
    // Drive `a` via a sensor so the node isn't orphan.
    auto const src = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    REQUIRE(g.connect(src, 0, cid, 0).has_value());
    auto const findings = st::feature::lint(g);
    REQUIRE(findings.empty());
}

TEST_CASE("Pin.default_value survives TOML round-trip", "[feature][toml]") {
    st::feature::Graph g;
    st::feature::Node n;
    n.kind = "primitive.compare_gt";
    n.label = "rpm > Y";
    st::feature::Pin b{0, "b", st::feature::PinType::Float, st::feature::PinDirection::Input, ""};
    b.default_value = 4000.0;
    n.pins.push_back(std::move(b));
    g.add_node(std::move(n));
    auto const text = st::feature::to_toml(g);
    auto const back = st::feature::from_toml(text);
    REQUIRE(back.has_value());
    REQUIRE(back->nodes().front().pins.front().default_value.has_value());
    REQUIRE(*back->nodes().front().pins.front().default_value == 4000.0);
}

TEST_CASE("lint reports each undriven input separately", "[feature][lint]") {
    // Two-input node, one driven, one not. Only the dangling input
    // surfaces in the findings.
    st::feature::Graph g;
    auto const src = g.add_node(make_source_node("src", st::feature::PinType::Float));
    st::feature::Node sink;
    sink.kind = "hook.add";
    sink.label = "add";
    sink.pins.push_back(st::feature::Pin{0, "a", st::feature::PinType::Float,
                                         st::feature::PinDirection::Input, ""});
    sink.pins.push_back(st::feature::Pin{1, "b", st::feature::PinType::Float,
                                         st::feature::PinDirection::Input, ""});
    auto const sk = g.add_node(std::move(sink));
    REQUIRE(g.connect(src, 0, sk, 0).has_value());
    auto const findings = st::feature::lint(g);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].pin.has_value());
    REQUIRE(*findings[0].pin == 1);
}

TEST_CASE("lint ignores output pins regardless of consumer count", "[feature][lint]") {
    // Output without consumer is allowed (it just produces a value
    // nobody reads). Not warn-worthy at this stage; that's a future
    // "dead code" lint when we have IR.
    st::feature::Graph g;
    g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    REQUIRE(st::feature::lint(g).empty());
}
