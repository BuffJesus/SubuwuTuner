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
