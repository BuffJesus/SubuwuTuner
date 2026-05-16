// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature.hpp"

#include "st/core/result.hpp"

#include <algorithm>
#include <utility>

namespace st::feature {

char const *pin_type_name(PinType t) noexcept {
    switch (t) {
        case PinType::Float: return "float";
        case PinType::Int:   return "int";
        case PinType::Bool:  return "bool";
    }
    return "?";
}

NodeId Graph::add_node(Node node) {
    node.id = next_node_id_++;
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

Node const *Graph::find_node(NodeId id) const noexcept {
    for (auto const &n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

Pin const *Graph::find_pin(NodeId node_id, PinId pin_id) const noexcept {
    auto const *n = find_node(node_id);
    if (n == nullptr) return nullptr;
    for (auto const &p : n->pins) {
        if (p.id == pin_id) return &p;
    }
    return nullptr;
}

Status Graph::connect(NodeId from_node, PinId from_pin,
                       NodeId to_node,   PinId to_pin) {
    auto const *from_p = find_pin(from_node, from_pin);
    if (from_p == nullptr) {
        return failure(ErrorCode::InvalidArgument,
                       "connect: source pin not found");
    }
    auto const *to_p = find_pin(to_node, to_pin);
    if (to_p == nullptr) {
        return failure(ErrorCode::InvalidArgument,
                       "connect: dest pin not found");
    }
    if (from_p->direction != PinDirection::Output) {
        return failure(ErrorCode::InvalidArgument,
                       "connect: source pin is not an output");
    }
    if (to_p->direction != PinDirection::Input) {
        return failure(ErrorCode::InvalidArgument,
                       "connect: dest pin is not an input");
    }
    if (from_p->type != to_p->type) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"connect: type mismatch ("}
                           + pin_type_name(from_p->type) + " -> "
                           + pin_type_name(to_p->type) + ")");
    }
    // Single-driver invariant: an input is driven by at most one edge.
    // Outputs can fan out without limit.
    for (auto const &e : edges_) {
        if (e.to_node == to_node && e.to_pin == to_pin) {
            return failure(ErrorCode::InvalidArgument,
                           "connect: dest input is already driven");
        }
    }
    edges_.push_back(Edge{from_node, from_pin, to_node, to_pin});
    return ok();
}

void Graph::remove_node(NodeId id) {
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                 [id](Node const &n) { return n.id == id; }),
                  nodes_.end());
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                 [id](Edge const &e) {
                                     return e.from_node == id || e.to_node == id;
                                 }),
                  edges_.end());
}

void Graph::set_node_position(NodeId id, float x, float y) {
    for (auto &n : nodes_) {
        if (n.id == id) {
            n.x = x;
            n.y = y;
            return;
        }
    }
}

void Graph::remove_edge(Edge const &target) {
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                 [&](Edge const &e) {
                                     return e.from_node == target.from_node
                                         && e.from_pin  == target.from_pin
                                         && e.to_node   == target.to_node
                                         && e.to_pin    == target.to_pin;
                                 }),
                  edges_.end());
}

Status Graph::validate() const {
    // Re-check type/direction agreement on every edge — these can drift
    // if a node was replaced after its incident edges were created
    // (currently no such API exists, but defensive).
    for (auto const &e : edges_) {
        auto const *fp = find_pin(e.from_node, e.from_pin);
        auto const *tp = find_pin(e.to_node,   e.to_pin);
        if (fp == nullptr || tp == nullptr) {
            return failure(ErrorCode::ParseError,
                           "validate: edge references a missing pin");
        }
        if (fp->direction != PinDirection::Output
            || tp->direction != PinDirection::Input) {
            return failure(ErrorCode::ParseError,
                           "validate: edge direction wrong");
        }
        if (fp->type != tp->type) {
            return failure(ErrorCode::ParseError,
                           "validate: edge type mismatch");
        }
    }
    // Cycle detection via DFS over the node graph. Adjacency is
    // built per call — this is O(V+E) and Phase 5 graphs are small
    // (tens to low hundreds of nodes).
    enum class Mark : std::uint8_t { Unvisited, OnStack, Done };
    std::vector<NodeId> ids;
    ids.reserve(nodes_.size());
    for (auto const &n : nodes_) ids.push_back(n.id);
    auto const index_of = [&](NodeId id) -> std::size_t {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == id) return i;
        }
        return ids.size();
    };
    std::vector<std::vector<std::size_t>> adj(ids.size());
    for (auto const &e : edges_) {
        auto const a = index_of(e.from_node);
        auto const b = index_of(e.to_node);
        if (a < adj.size() && b < adj.size()) adj[a].push_back(b);
    }
    std::vector<Mark> mark(ids.size(), Mark::Unvisited);
    auto visit = [&](auto &&self, std::size_t u) -> bool {
        mark[u] = Mark::OnStack;
        for (auto v : adj[u]) {
            if (mark[v] == Mark::OnStack) return true;
            if (mark[v] == Mark::Unvisited && self(self, v)) return true;
        }
        mark[u] = Mark::Done;
        return false;
    };
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (mark[i] == Mark::Unvisited && visit(visit, i)) {
            return failure(ErrorCode::ParseError,
                           "validate: graph has a cycle");
        }
    }
    return ok();
}

} // namespace st::feature
