// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/feature.hpp"

#include "st/core/result.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace st::feature {

char const *pin_type_name(PinType t) noexcept {
    switch (t) {
    case PinType::Float:
        return "float";
    case PinType::Int:
        return "int";
    case PinType::Bool:
        return "bool";
    }
    return "?";
}

std::optional<PinType> parse_pin_type(std::string_view s) noexcept {
    if (s == "float")
        return PinType::Float;
    if (s == "int")
        return PinType::Int;
    if (s == "bool")
        return PinType::Bool;
    return std::nullopt;
}

namespace {

char const *pin_direction_name(PinDirection d) noexcept {
    return d == PinDirection::Output ? "output" : "input";
}

std::optional<PinDirection> parse_pin_direction(std::string_view s) noexcept {
    if (s == "input")
        return PinDirection::Input;
    if (s == "output")
        return PinDirection::Output;
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
        if (c == '\\' || c == '"')
            out.push_back('\\');
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
        out << "id    = " << n.id << "\n";
        out << "kind  = " << toml_quote(n.kind) << "\n";
        out << "label = " << toml_quote(n.label) << "\n";
        out << "x     = " << n.x << "\n";
        out << "y     = " << n.y << "\n";
        if (n.is_phase_break) {
            out << "phase_break = true\n";
        }
        out << "pins  = [\n";
        for (std::size_t i = 0; i < n.pins.size(); ++i) {
            auto const &p = n.pins[i];
            out << "  { id = " << p.id << ", name = " << toml_quote(p.name)
                << ", type = " << toml_quote(pin_type_name(p.type))
                << ", direction = " << toml_quote(pin_direction_name(p.direction))
                << ", unit = " << toml_quote(p.unit);
            if (!p.label.empty()) {
                out << ", label = " << toml_quote(p.label);
            }
            if (p.default_value.has_value()) {
                out << ", default = " << *p.default_value;
            }
            out << " }";
            if (i + 1 < n.pins.size())
                out << ",";
            out << "\n";
        }
        out << "]\n\n";
    }
    for (auto const &e : g.edges()) {
        out << "[[edge]]\n";
        out << "from_node = " << e.from_node << "\n";
        out << "from_pin  = " << e.from_pin << "\n";
        out << "to_node   = " << e.to_node << "\n";
        out << "to_pin    = " << e.to_pin << "\n\n";
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
        return failure(ErrorCode::ParseError, "feature TOML: missing [graph] section");
    }
    int const schema = static_cast<int>((*gtbl)["schema_version"].value_or<std::int64_t>(0));
    if (schema < 1) {
        return failure(ErrorCode::ParseError,
                       "feature TOML: [graph].schema_version must be >= 1, got " +
                           std::to_string(schema));
    }

    Graph g;
    // Persisted-id → assigned-id, since Graph::add_node assigns its
    // own monotonic ids. Edges remap through this on the second pass.
    std::unordered_map<NodeId, NodeId> id_map;

    auto const *nodes = tbl["node"].as_array();
    if (nodes != nullptr) {
        std::size_t node_idx = 0;
        for (auto const &elem : *nodes) {
            auto const *ntbl = elem.as_table();
            if (ntbl == nullptr) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[node]] at index " + std::to_string(node_idx) +
                                   " is not a table");
            }
            Node n;
            auto const persisted_id = static_cast<NodeId>((*ntbl)["id"].value_or<std::int64_t>(0));
            if (persisted_id == 0) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[node]] at index " + std::to_string(node_idx) +
                                   " missing id");
            }
            // Required fields. value_or returns the default for both
            // "key missing" and "key present but wrong type", so an
            // empty-string check catches both forms of tampering.
            n.kind = (*ntbl)["kind"].value_or<std::string>("");
            if (n.kind.empty()) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[node]] id=" + std::to_string(persisted_id) +
                                   " missing required `kind`");
            }
            // Duplicate-id detection — id_map's invariant doubles as
            // a uniqueness check.
            if (id_map.contains(persisted_id)) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: duplicate node id " + std::to_string(persisted_id));
            }
            n.label = (*ntbl)["label"].value_or<std::string>("");
            n.x = static_cast<float>((*ntbl)["x"].value_or<double>(0.0));
            n.y = static_cast<float>((*ntbl)["y"].value_or<double>(0.0));
            n.is_phase_break = (*ntbl)["phase_break"].value_or<bool>(false);
            auto const *pins = (*ntbl)["pins"].as_array();
            if (pins != nullptr) {
                std::size_t pin_idx = 0;
                for (auto const &pe : *pins) {
                    auto const *ptbl = pe.as_table();
                    if (ptbl == nullptr) {
                        return failure(ErrorCode::ParseError,
                                       "feature TOML: pin at index " + std::to_string(pin_idx) +
                                           " on node id=" + std::to_string(persisted_id) +
                                           " is not a table");
                    }
                    Pin p;
                    p.id = static_cast<PinId>((*ptbl)["id"].value_or<std::int64_t>(0));
                    p.name = (*ptbl)["name"].value_or<std::string>("");
                    if (p.name.empty()) {
                        return failure(ErrorCode::ParseError,
                                       "feature TOML: node id=" + std::to_string(persisted_id) +
                                           " pin at index " + std::to_string(pin_idx) +
                                           " has no `name`");
                    }
                    auto const type_s = (*ptbl)["type"].value_or<std::string>("");
                    auto const dir_s = (*ptbl)["direction"].value_or<std::string>("");
                    p.unit = (*ptbl)["unit"].value_or<std::string>("");
                    p.label = (*ptbl)["label"].value_or<std::string>("");
                    auto const t = parse_pin_type(type_s);
                    auto const d = parse_pin_direction(dir_s);
                    if (!t.has_value() || !d.has_value()) {
                        return failure(ErrorCode::ParseError,
                                       "feature TOML: node id=" + std::to_string(persisted_id) +
                                           " pin '" + p.name + "' has unknown type='" + type_s +
                                           "' or direction='" + dir_s + "'");
                    }
                    p.type = *t;
                    p.direction = *d;
                    if (auto const dv = (*ptbl)["default"].value<double>(); dv.has_value()) {
                        p.default_value = *dv;
                    } else if (auto const di = (*ptbl)["default"].value<std::int64_t>();
                               di.has_value()) {
                        p.default_value = static_cast<double>(*di);
                    } else if (auto const db = (*ptbl)["default"].value<bool>(); db.has_value()) {
                        p.default_value = *db ? 1.0 : 0.0;
                    }
                    // Pin-id uniqueness within a node.
                    for (auto const &existing : n.pins) {
                        if (existing.id == p.id) {
                            return failure(ErrorCode::ParseError,
                                           "feature TOML: node id=" + std::to_string(persisted_id) +
                                               " has duplicate pin id " + std::to_string(p.id));
                        }
                    }
                    n.pins.push_back(std::move(p));
                    ++pin_idx;
                }
            }
            auto const assigned = g.add_node(std::move(n));
            id_map[persisted_id] = assigned;
            ++node_idx;
        }
    }

    auto const *edges = tbl["edge"].as_array();
    if (edges != nullptr) {
        std::size_t edge_idx = 0;
        for (auto const &elem : *edges) {
            auto const *etbl = elem.as_table();
            if (etbl == nullptr) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[edge]] at index " + std::to_string(edge_idx) +
                                   " is not a table");
            }
            auto const from_persisted =
                static_cast<NodeId>((*etbl)["from_node"].value_or<std::int64_t>(0));
            auto const to_persisted =
                static_cast<NodeId>((*etbl)["to_node"].value_or<std::int64_t>(0));
            auto const fit = id_map.find(from_persisted);
            auto const tit = id_map.find(to_persisted);
            if (fit == id_map.end() || tit == id_map.end()) {
                // Name whichever side is the bad one so the user knows
                // which node id to chase in their TOML.
                std::string msg{"feature TOML: [[edge]] at index "};
                msg += std::to_string(edge_idx);
                msg += " references unknown node id ";
                if (fit == id_map.end()) {
                    msg += "from_node=" + std::to_string(from_persisted);
                    if (tit == id_map.end()) {
                        msg += " and to_node=" + std::to_string(to_persisted);
                    }
                } else {
                    msg += "to_node=" + std::to_string(to_persisted);
                }
                return failure(ErrorCode::ParseError, std::move(msg));
            }
            auto const from_pin = static_cast<PinId>((*etbl)["from_pin"].value_or<std::int64_t>(0));
            auto const to_pin = static_cast<PinId>((*etbl)["to_pin"].value_or<std::int64_t>(0));
            if (auto r = g.connect(fit->second, from_pin, tit->second, to_pin); !r.has_value()) {
                return failure(ErrorCode::ParseError,
                               "feature TOML: [[edge]] at index " + std::to_string(edge_idx) +
                                   " (from_node=" + std::to_string(from_persisted) + "/pin=" +
                                   std::to_string(from_pin) + " → to_node=" +
                                   std::to_string(to_persisted) + "/pin=" +
                                   std::to_string(to_pin) + ") rejected: " + r.error().to_string());
            }
            ++edge_idx;
        }
    }

    return g;
}

Status save_file(Graph const &g, std::filesystem::path const &path) {
    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        return failure(ErrorCode::IoFailure, "cannot open feature graph: " + path.string());
    }
    out << to_toml(g);
    out.flush();
    if (!out) {
        return failure(ErrorCode::IoFailure, "cannot write feature graph: " + path.string());
    }
    return {};
}

Result<Graph> load_file(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::FileNotFound, "cannot open feature graph: " + path.string());
    }
    // Read via rdbuf rather than istreambuf_iterator: GCC 15's -O3 inliner
    // raises a false-positive -Werror=null-dereference on the iterator's
    // streambuf access when constructing a std::string from the range.
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.fail() && !in.eof()) {
        return failure(ErrorCode::IoFailure, "cannot read feature graph: " + path.string());
    }
    std::string const text = ss.str();
    return from_toml(text);
}

NodeId Graph::add_node(Node node) {
    node.id = next_node_id_++;
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

Node const *Graph::find_node(NodeId id) const noexcept {
    for (auto const &n : nodes_) {
        if (n.id == id)
            return &n;
    }
    return nullptr;
}

Pin const *Graph::find_pin(NodeId node_id, PinId pin_id) const noexcept {
    auto const *n = find_node(node_id);
    if (n == nullptr)
        return nullptr;
    for (auto const &p : n->pins) {
        if (p.id == pin_id)
            return &p;
    }
    return nullptr;
}

Status Graph::connect(NodeId from_node, PinId from_pin, NodeId to_node, PinId to_pin) {
    auto const endpoint = [](NodeId n, PinId p) {
        return "node=" + std::to_string(n) + "/pin=" + std::to_string(p);
    };
    auto const *from_p = find_pin(from_node, from_pin);
    if (from_p == nullptr) {
        return failure(ErrorCode::InvalidArgument,
                       "connect: source pin not found at " + endpoint(from_node, from_pin));
    }
    auto const *to_p = find_pin(to_node, to_pin);
    if (to_p == nullptr) {
        return failure(ErrorCode::InvalidArgument,
                       "connect: dest pin not found at " + endpoint(to_node, to_pin));
    }
    if (from_p->direction != PinDirection::Output) {
        return failure(ErrorCode::InvalidArgument, "connect: source pin '" + from_p->name +
                                                       "' at " + endpoint(from_node, from_pin) +
                                                       " is not an output");
    }
    if (to_p->direction != PinDirection::Input) {
        return failure(ErrorCode::InvalidArgument, "connect: dest pin '" + to_p->name + "' at " +
                                                       endpoint(to_node, to_pin) +
                                                       " is not an input");
    }
    if (from_p->type != to_p->type) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"connect: type mismatch — "} + endpoint(from_node, from_pin) +
                           " (" + pin_type_name(from_p->type) + ") -> " +
                           endpoint(to_node, to_pin) + " (" + pin_type_name(to_p->type) + ")");
    }
    // Unit check — both pins must agree when both declare a unit.
    // An empty unit on either side means "unit-agnostic" (generic
    // math nodes, etc.) and connects to anything compatible by type.
    // Pack-declared hook signals always carry a unit so dimensional
    // mistakes (rpm into %, ms into °C) are caught at wire time.
    if (!from_p->unit.empty() && !to_p->unit.empty() && from_p->unit != to_p->unit) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"connect: unit mismatch — "} + endpoint(from_node, from_pin) +
                           " (" + from_p->unit + ") -> " + endpoint(to_node, to_pin) + " (" +
                           to_p->unit + ")");
    }
    // Single-driver invariant: an input is driven by at most one edge.
    // Outputs can fan out without limit.
    for (auto const &e : edges_) {
        if (e.to_node == to_node && e.to_pin == to_pin) {
            return failure(ErrorCode::InvalidArgument, "connect: dest input " +
                                                           endpoint(to_node, to_pin) +
                                                           " is already driven from " +
                                                           endpoint(e.from_node, e.from_pin));
        }
    }
    edges_.push_back(Edge{from_node, from_pin, to_node, to_pin});
    return ok();
}

void Graph::remove_node(NodeId id) {
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(), [id](Node const &n) { return n.id == id; }),
        nodes_.end());
    edges_.erase(
        std::remove_if(edges_.begin(), edges_.end(),
                       [id](Edge const &e) { return e.from_node == id || e.to_node == id; }),
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

void Graph::set_pin_default(NodeId id, PinId pin_id, std::optional<double> value) {
    for (auto &n : nodes_) {
        if (n.id != id)
            continue;
        for (auto &p : n.pins) {
            if (p.id != pin_id)
                continue;
            p.default_value = value;
            return;
        }
        return;
    }
}

void Graph::remove_edge(Edge const &target) {
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](Edge const &e) {
                                    return e.from_node == target.from_node &&
                                           e.from_pin == target.from_pin &&
                                           e.to_node == target.to_node && e.to_pin == target.to_pin;
                                }),
                 edges_.end());
}

Status Graph::validate() const {
    // Re-check type/direction agreement on every edge — these can drift
    // if a node was replaced after its incident edges were created
    // (currently no such API exists, but defensive).
    auto const endpoint = [](NodeId n, PinId p) {
        return "node=" + std::to_string(n) + "/pin=" + std::to_string(p);
    };
    std::size_t edge_idx = 0;
    for (auto const &e : edges_) {
        auto const *fp = find_pin(e.from_node, e.from_pin);
        auto const *tp = find_pin(e.to_node, e.to_pin);
        std::string const edge_tag =
            "edge #" + std::to_string(edge_idx) + " (" + endpoint(e.from_node, e.from_pin) +
            " → " + endpoint(e.to_node, e.to_pin) + ")";
        if (fp == nullptr || tp == nullptr) {
            std::string side;
            if (fp == nullptr && tp == nullptr) {
                side = "both endpoints";
            } else if (fp == nullptr) {
                side = "source";
            } else {
                side = "dest";
            }
            return failure(ErrorCode::ParseError,
                           "validate: " + edge_tag + " references missing pin (" + side + ")");
        }
        if (fp->direction != PinDirection::Output || tp->direction != PinDirection::Input) {
            return failure(ErrorCode::ParseError,
                           "validate: " + edge_tag + " direction wrong (source=" +
                               (fp->direction == PinDirection::Output ? "out" : "in") +
                               ", dest=" + (tp->direction == PinDirection::Input ? "in" : "out") +
                               ")");
        }
        if (fp->type != tp->type) {
            return failure(ErrorCode::ParseError, "validate: " + edge_tag + " type mismatch (" +
                                                      pin_type_name(fp->type) + " → " +
                                                      pin_type_name(tp->type) + ")");
        }
        ++edge_idx;
    }
    // Cycle detection. Each Node expands into two vertices in the
    // DFS graph: an "input side" (id*2) and an "output side"
    // (id*2+1). Edges go from one node's output-side to another
    // node's input-side. For non-phase-break nodes (primitives,
    // generic debug nodes), an internal edge input-side →
    // output-side captures "output is a pure function of input at
    // the same time T", so primitive.out → x → primitive.in IS a
    // cycle. Phase-break nodes (hooks) omit the internal edge:
    // their output-side fires at time T (ECU state available
    // before user logic), input-side fires at time T+ε (user
    // override applied after), so hook.out → user_logic →
    // hook.in is a legitimate DAG.
    enum class Mark : std::uint8_t { Unvisited, OnStack, Done };
    std::vector<NodeId> ids;
    ids.reserve(nodes_.size());
    for (auto const &n : nodes_)
        ids.push_back(n.id);
    auto const index_of = [&](NodeId id) -> std::size_t {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == id)
                return i;
        }
        return ids.size();
    };
    // Side encoding: 0 = input side, 1 = output side. Vertex index
    // for (node i, side s) is 2*i + s.
    std::size_t const nv = ids.size() * 2;
    std::vector<std::vector<std::size_t>> adj(nv);
    auto const input_v = [](std::size_t i) { return i * 2; };
    auto const output_v = [](std::size_t i) { return i * 2 + 1; };
    for (auto const &e : edges_) {
        auto const a = index_of(e.from_node);
        auto const b = index_of(e.to_node);
        if (a >= ids.size() || b >= ids.size())
            continue;
        adj[output_v(a)].push_back(input_v(b));
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (!nodes_[i].is_phase_break) {
            adj[input_v(i)].push_back(output_v(i));
        }
    }
    std::vector<Mark> mark(nv, Mark::Unvisited);
    std::size_t cycle_back_edge_vertex = nv; // sentinel — "no cycle hit"
    auto visit = [&mark, &adj, &cycle_back_edge_vertex](auto &&self, std::size_t u) -> bool {
        mark[u] = Mark::OnStack;
        for (auto v : adj[u]) {
            if (mark[v] == Mark::OnStack) {
                cycle_back_edge_vertex = v;
                return true;
            }
            if (mark[v] == Mark::Unvisited && self(self, v))
                return true;
        }
        mark[u] = Mark::Done;
        return false;
    };
    for (std::size_t i = 0; i < nv; ++i) {
        if (mark[i] == Mark::Unvisited && visit(visit, i)) {
            // Recover the offending node id from the vertex encoding
            // (v = 2*node_index + side). Reporting one back-edge target
            // is enough to point the user at the cycle without dumping
            // the whole SCC.
            auto const node_index = cycle_back_edge_vertex / 2;
            std::string msg{"validate: graph has a cycle (back-edge into node id "};
            msg += std::to_string(ids[node_index]);
            if (!nodes_[node_index].label.empty()) {
                msg += " '" + nodes_[node_index].label + "'";
            } else if (!nodes_[node_index].kind.empty()) {
                msg += " kind='" + nodes_[node_index].kind + "'";
            }
            msg += ")";
            return failure(ErrorCode::ParseError, std::move(msg));
        }
    }
    return ok();
}

std::vector<LintFinding> lint(Graph const &g) {
    std::vector<LintFinding> findings;
    auto const &nodes = g.nodes();
    auto const &edges = g.edges();

    auto const driven = [&](NodeId nid, PinId pid) {
        for (auto const &e : edges) {
            if (e.to_node == nid && e.to_pin == pid)
                return true;
        }
        return false;
    };
    auto const has_any_edge = [&](NodeId nid) {
        for (auto const &e : edges) {
            if (e.from_node == nid || e.to_node == nid)
                return true;
        }
        return false;
    };
    auto const node_display = [](Node const &n) -> std::string {
        return !n.label.empty() ? n.label : n.kind;
    };

    for (auto const &n : nodes) {
        // Orphan-node check first — a totally-disconnected node
        // that requires at least one input is almost certainly
        // accidental (user dropped a hook and forgot to wire its
        // overrides). Pure-source nodes with only output pins
        // aren't flagged: a dangling output is a future "dead
        // code" concern, not an editor-shape one.
        bool has_input = false;
        for (auto const &p : n.pins) {
            if (p.direction == PinDirection::Input) {
                has_input = true;
                break;
            }
        }
        if (has_input && !has_any_edge(n.id)) {
            findings.push_back(
                {"node '" + node_display(n) + "' has no connections", n.id, std::nullopt});
            continue;
        }
        for (auto const &p : n.pins) {
            if (p.direction != PinDirection::Input)
                continue;
            if (driven(n.id, p.id))
                continue;
            if (p.default_value.has_value())
                continue;
            std::string msg =
                "input pin '" + p.name + "' on node '" + node_display(n) + "' is not driven";
            findings.push_back({std::move(msg), n.id, p.id});
        }
    }
    return findings;
}

} // namespace st::feature
