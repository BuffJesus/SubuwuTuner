// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

st::feature::Node make_source_node(char const *kind, st::feature::PinType t) {
    st::feature::Node n;
    n.kind  = kind;
    n.label = kind;
    n.pins.push_back(st::feature::Pin{
        0, "out", t, st::feature::PinDirection::Output, ""});
    return n;
}

st::feature::Node make_sink_node(char const *kind, st::feature::PinType t) {
    st::feature::Node n;
    n.kind  = kind;
    n.label = kind;
    n.pins.push_back(st::feature::Pin{
        0, "in", t, st::feature::PinDirection::Input, ""});
    return n;
}

} // namespace

TEST_CASE("Graph assigns monotonic node ids", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm",  st::feature::PinType::Float));
    auto const b = g.add_node(make_source_node("load", st::feature::PinType::Float));
    auto const c = g.add_node(make_sink_node("out",    st::feature::PinType::Float));
    REQUIRE(a != b);
    REQUIRE(b != c);
    REQUIRE(g.nodes().size() == 3);
}

TEST_CASE("Graph::connect rejects type mismatch", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm",   st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("counter", st::feature::PinType::Int));
    auto const r = g.connect(a, 0, b, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::connect rejects fan-in into a single input",
          "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    auto const b = g.add_node(make_source_node("b", st::feature::PinType::Float));
    auto const c = g.add_node(make_sink_node("c",   st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, c, 0).has_value());
    auto const r = g.connect(b, 0, c, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::connect rejects wrong-direction wires",
          "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("b",   st::feature::PinType::Float));
    // Input → Output is invalid.
    auto const r = g.connect(b, 0, a, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::remove_node cleans up incident edges",
          "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("b",   st::feature::PinType::Float));
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
    n.pins.push_back(st::feature::Pin{
        0, "in",  st::feature::PinType::Float,
        st::feature::PinDirection::Input,  ""});
    n.pins.push_back(st::feature::Pin{
        1, "out", st::feature::PinType::Float,
        st::feature::PinDirection::Output, ""});
    st::feature::Graph g;
    auto const a = g.add_node(n);
    auto const b = g.add_node(n);
    REQUIRE(g.connect(a, 1, b, 0).has_value());
    REQUIRE(g.connect(b, 1, a, 0).has_value());
    auto const r = g.validate();
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph::set_node_position updates position",
          "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("a", st::feature::PinType::Float));
    g.set_node_position(a, 123.5f, 456.25f);
    auto const *n = g.find_node(a);
    REQUIRE(n != nullptr);
    REQUIRE(n->x == 123.5f);
    REQUIRE(n->y == 456.25f);
    // Idempotent on unknown ids.
    g.set_node_position(static_cast<st::feature::NodeId>(99999),
                         1.0f, 2.0f);
    REQUIRE(g.nodes().size() == 1);
}

TEST_CASE("Graph::validate accepts a clean DAG", "[feature][graph]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm",  st::feature::PinType::Float));
    auto const b = g.add_node(make_source_node("load", st::feature::PinType::Float));
    auto const c = g.add_node(make_sink_node("out",    st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, c, 0).has_value());
    // Two outputs feeding one input is *not* fan-in; b's edge would
    // duplicate the driver invariant. Use a separate sink instead.
    auto const d = g.add_node(make_sink_node("out2",   st::feature::PinType::Float));
    REQUIRE(g.connect(b, 0, d, 0).has_value());
    REQUIRE(g.validate().has_value());
}

TEST_CASE("Graph TOML round-trip preserves shape", "[feature][toml]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm",  st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("out",    st::feature::PinType::Float));
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

TEST_CASE("Graph from_toml rejects missing schema_version",
          "[feature][toml]") {
    std::string const bad = "[graph]\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects empty kind", "[feature][toml]") {
    std::string const bad =
        "[graph]\nschema_version = 1\n\n"
        "[[node]]\n"
        "id = 1\nkind = \"\"\nlabel = \"x\"\nx = 0.0\ny = 0.0\n"
        "pins = []\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects missing kind key", "[feature][toml]") {
    std::string const bad =
        "[graph]\nschema_version = 1\n\n"
        "[[node]]\n"
        "id = 1\nlabel = \"x\"\nx = 0.0\ny = 0.0\n"
        "pins = []\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects duplicate node id", "[feature][toml]") {
    std::string const bad =
        "[graph]\nschema_version = 1\n\n"
        "[[node]]\nid = 1\nkind = \"a\"\nlabel = \"\"\nx = 0.0\ny = 0.0\n"
        "pins = []\n\n"
        "[[node]]\nid = 1\nkind = \"b\"\nlabel = \"\"\nx = 0.0\ny = 0.0\n"
        "pins = []\n";
    auto r = st::feature::from_toml(bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Graph from_toml rejects pin with empty name",
          "[feature][toml]") {
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

TEST_CASE("Graph from_toml rejects duplicate pin id within a node",
          "[feature][toml]") {
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

TEST_CASE("lint on an empty graph reports nothing",
          "[feature][lint]") {
    st::feature::Graph g;
    REQUIRE(st::feature::lint(g).empty());
}

TEST_CASE("lint flags an unconnected input pin",
          "[feature][lint]") {
    st::feature::Graph g;
    g.add_node(make_sink_node("hook.commit", st::feature::PinType::Float));
    auto const findings = st::feature::lint(g);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].node.has_value());
    REQUIRE(findings[0].pin.has_value());
    REQUIRE(findings[0].message.find("not driven") != std::string::npos);
}

TEST_CASE("lint clears once the input pin is driven",
          "[feature][lint]") {
    st::feature::Graph g;
    auto const a = g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    auto const b = g.add_node(make_sink_node("hook",  st::feature::PinType::Float));
    REQUIRE(g.connect(a, 0, b, 0).has_value());
    REQUIRE(st::feature::lint(g).empty());
}

TEST_CASE("lint reports each undriven input separately",
          "[feature][lint]") {
    // Two-input node, one driven, one not. Only the dangling input
    // surfaces in the findings.
    st::feature::Graph g;
    auto const src = g.add_node(make_source_node("src", st::feature::PinType::Float));
    st::feature::Node sink;
    sink.kind  = "hook.add";
    sink.label = "add";
    sink.pins.push_back(st::feature::Pin{
        0, "a", st::feature::PinType::Float,
        st::feature::PinDirection::Input,  ""});
    sink.pins.push_back(st::feature::Pin{
        1, "b", st::feature::PinType::Float,
        st::feature::PinDirection::Input,  ""});
    auto const sk = g.add_node(std::move(sink));
    REQUIRE(g.connect(src, 0, sk, 0).has_value());
    auto const findings = st::feature::lint(g);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].pin.has_value());
    REQUIRE(*findings[0].pin == 1);
}

TEST_CASE("lint ignores output pins regardless of consumer count",
          "[feature][lint]") {
    // Output without consumer is allowed (it just produces a value
    // nobody reads). Not warn-worthy at this stage; that's a future
    // "dead code" lint when we have IR.
    st::feature::Graph g;
    g.add_node(make_source_node("rpm", st::feature::PinType::Float));
    REQUIRE(st::feature::lint(g).empty());
}
