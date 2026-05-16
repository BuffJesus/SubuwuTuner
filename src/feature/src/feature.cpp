// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature.hpp"

#include "st/core/result.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_map>
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

std::optional<PinType> parse_pin_type(std::string_view s) noexcept {
    if (s == "float") return PinType::Float;
    if (s == "int")   return PinType::Int;
    if (s == "bool")  return PinType::Bool;
    return std::nullopt;
}

namespace {

char const *pin_direction_name(PinDirection d) noexcept {
    return d == PinDirection::Output ? "output" : "input";
}

std::optional<PinDirection> parse_pin_direction(std::string_view s) noexcept {
    if (s == "input")  return PinDirection::Input;
    if (s == "output") return PinDirection::Output;
    return std::nullopt;
}

// TOML strings need quotes + standard escape handling. The values we
// emit are short user-authored labels; rather than pulling in a full
// quoter, escape the two characters that would break a basic string
// (backslash and double-quote) and reject anything with a newline.
std::string toml_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string to_toml(Graph const &g) {
    std::ostringstream out;
    out << "[graph]\n";
    out << "schema_version = 1\n\n";
    for (auto const &n : g.nodes()) {
        out << "[[node]]\n";
        out << "id    = "  << n.id    << "\n";
        out << "kind  = "  << toml_quote(n.kind)  << "\n";
        out << "label = "  << toml_quote(n.label) << "\n";
        out << "x     = "  << n.x << "\n";
        out << "y     = "  << n.y << "\n";
        out << "pins  = [\n";
        for (std::size_t i = 0; i < n.pins.size(); ++i) {
            auto const &p = n.pins[i];
            out << "  { id = " << p.id
                << ", name = " << toml_quote(p.name)
                << ", type = " << toml_quote(pin_type_name(p.type))
                << ", direction = " << toml_quote(pin_direction_name(p.direction))
                << ", unit = " << toml_quote(p.unit)
                << " }";
            if (i + 1 < n.pins.size()) out << ",";
            out << "\n";
        }
        out << "]\n\n";
    }
    for (auto const &e : g.edges()) {
        out << "[[edge]]\n";
        out << "from_node = " << e.from_node << "\n";
        out << "from_pin  = " << e.from_pin  << "\n";
        out << "to_node   = " << e.to_node   << "\n";
        out << "to_pin    = " << e.to_pin    << "\n\n";
    }
    return out.str();
}

Result<Graph> from_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (toml::parse_error const &e) {
        std::string msg{"feature TOML parse: "};
        msg.append(e.description());
        return failure(ErrorCode::ParseError, std::move(msg));
    }
    auto const *gtbl = tbl["graph"].as_table();
    if (gtbl == nullptr) {
        return failure(ErrorCode::ParseError,
                       "feature TOML: missing [graph] section");
    }
    int const schema = static_cast<int>(
        (*gtbl)["schema_version"].value_or<std::int64_t>(0));
    if (schema < 1) {
        return failure(ErrorCode::ParseError,
                       "feature TOML: [graph].schema_version must be >= 1");
    }

    Graph g;
    // Persisted-id → assigned-id, since Graph::add_node assigns its
    // own monotonic ids. Edges remap through this on the second pass.
    std::unordered_map<NodeId, NodeId> id_map;

    auto const *nodes = tbl["node"].as_array();
    if (nodes != nullptr) {
        for (auto const &elem : *nodes) {
            auto const *ntbl = elem.as_table();
            if (ntbl == nullptr) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[node]] not a table");
            }
            Node n;
            auto const persisted_id = static_cast<NodeId>(
                (*ntbl)["id"].value_or<std::int64_t>(0));
            if (persisted_id == 0) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[node]] missing id");
            }
            n.kind  = (*ntbl)["kind"].value_or<std::string>("");
            n.label = (*ntbl)["label"].value_or<std::string>("");
            n.x     = static_cast<float>(
                (*ntbl)["x"].value_or<double>(0.0));
            n.y     = static_cast<float>(
                (*ntbl)["y"].value_or<double>(0.0));
            auto const *pins = (*ntbl)["pins"].as_array();
            if (pins != nullptr) {
                for (auto const &pe : *pins) {
                    auto const *ptbl = pe.as_table();
                    if (ptbl == nullptr) {
                        return failure(ErrorCode::ParseError,
                                       "feature TOML: pin not a table");
                    }
                    Pin p;
                    p.id   = static_cast<PinId>(
                        (*ptbl)["id"].value_or<std::int64_t>(0));
                    p.name = (*ptbl)["name"].value_or<std::string>("");
                    auto const type_s = (*ptbl)["type"].value_or<std::string>("");
                    auto const dir_s  = (*ptbl)["direction"].value_or<std::string>("");
                    p.unit = (*ptbl)["unit"].value_or<std::string>("");
                    auto const t = parse_pin_type(type_s);
                    auto const d = parse_pin_direction(dir_s);
                    if (!t.has_value() || !d.has_value()) {
                        return failure(ErrorCode::ParseError,
                                       "feature TOML: unknown pin type / "
                                       "direction");
                    }
                    p.type      = *t;
                    p.direction = *d;
                    n.pins.push_back(std::move(p));
                }
            }
            auto const assigned = g.add_node(std::move(n));
            id_map[persisted_id] = assigned;
        }
    }

    auto const *edges = tbl["edge"].as_array();
    if (edges != nullptr) {
        for (auto const &elem : *edges) {
            auto const *etbl = elem.as_table();
            if (etbl == nullptr) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[edge]] not a table");
            }
            auto const from_persisted = static_cast<NodeId>(
                (*etbl)["from_node"].value_or<std::int64_t>(0));
            auto const to_persisted = static_cast<NodeId>(
                (*etbl)["to_node"].value_or<std::int64_t>(0));
            auto const fit = id_map.find(from_persisted);
            auto const tit = id_map.find(to_persisted);
            if (fit == id_map.end() || tit == id_map.end()) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: edge references unknown node");
            }
            auto const from_pin = static_cast<PinId>(
                (*etbl)["from_pin"].value_or<std::int64_t>(0));
            auto const to_pin = static_cast<PinId>(
                (*etbl)["to_pin"].value_or<std::int64_t>(0));
            if (auto r = g.connect(fit->second, from_pin,
                                    tit->second, to_pin);
                !r.has_value()) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: connect rejected: "
                                   + r.error().to_string());
            }
        }
    }

    return g;
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
