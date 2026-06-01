// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Phase 5 custom-features node-graph designer. Data model lives in
// st::feature::Graph; this canvas wires it up interactively. Grid-
// backed canvas with pan/zoom; node rendering keyed off the pack's
// hook + primitive declarations; drag-to-connect with type-checked
// edges; per-pin defaults via right-click menu; .stmod load/save +
// clipboard JSON; in-canvas compile preview against the selected
// backend (sh2a / rh850). Still preview — no auto-layout, no undo
// through st::edit::History yet (Ctrl+Z scope is per-canvas).

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"
#include "st/feature.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace st::ui {

void render_features_designer(AppState &state) {
    if (!state.show_features_designer) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(720.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Custom Features Designer###Custom Features Designer (Preview)",
                      &state.show_features_designer)) {
        ImGui::End();
        return;
    }

    // Preview status pill — surfaces the API-may-shift caveat without
    // littering the window title. Sits as a single-line strip above the
    // toolbar so the canvas below stays uncluttered.
    preview_pill();
    ImGui::SameLine();
    text_subtle("Node graph + SH-2A codegen working; flash wire-up lands in Phase 5.");
    ImGui::Separator();

    // Esc / right-click cancel an in-progress wire. The non-obvious
    // bit is the `features_wiring_blocked` latch: ImGui DOES detect
    // Esc fine, but the pin-drag handler below re-spawns a wire on
    // the next frame because the mouse is still held over the source
    // pin. Latch suppresses re-spawn until the mouse button is
    // released.
    bool const was_wiring_at_start = state.features_wiring_active;
    if (was_wiring_at_start && (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) ||
                                ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        state.features_wiring_active = false;
        state.features_wiring_blocked = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.features_wiring_blocked = false;
    }

    ImGui::TextColored(chip_fg_warn(), "\xE2\x9A\xA0 Phase 5 preview.");
    text_subtle("Editor, IR, SH-2A codegen, and .stmod persistence "
                "all shipped. Patch insertion + flashing wait on the "
                "bench rig — see docs/16.");

    ImGui::Spacing();
    ImGui::Text("Nodes: %zu     Edges: %zu", state.features_graph.nodes().size(),
                state.features_graph.edges().size());

    // Helper-lambda for spawning nodes at a free-ish slot.
    auto const next_slot_y = [&]() {
        return 40.0f + 80.0f * static_cast<float>(state.features_graph.nodes().size());
    };

    // Pack-driven node palette. Two groups in one popup:
    //   - Hooks (splice / sensor / action points declared by the pack)
    //   - Primitives (pure-computation nodes)
    // Pin-direction convention differs between the two groups:
    //   - For hooks, pack-declared `inputs` become graph Output pins
    //     (the hook offers data TO user logic) and `outputs` become
    //     graph Input pins (the hook demands data FROM user logic).
    //   - For primitives, the convention is the obvious one:
    //     pack-declared `inputs` = graph Input pins, `outputs` =
    //     graph Output pins.
    // Without a loaded project, fall back to the generic debug
    // buttons so an empty workspace can still be exercised.
    bool const have_palette =
        state.project.has_value() && (!state.project->definition().hooks().empty() ||
                                      !state.project->definition().primitives().empty());
    if (have_palette) {
        if (ImGui::Button("\xEE\x9C\x90  Insert \xE2\x96\xBE")) {
            ImGui::OpenPopup("##features_palette");
        }
        if (ImGui::BeginPopup("##features_palette")) {
            auto const &hooks = state.project->definition().hooks();
            auto const &prims = state.project->definition().primitives();
            // Spawn helper — shared between the two groups. The pin-
            // direction lambda decides what `inputs`/`outputs` mean on
            // a given kind of node.
            auto const spawn_node = [&](std::string const &kind_prefix, std::string const &id,
                                        std::string const &display_name,
                                        std::vector<st::HookSignal> const &decl_inputs,
                                        std::vector<st::HookSignal> const &decl_outputs,
                                        st::feature::PinDirection in_dir,
                                        st::feature::PinDirection out_dir, bool phase_break) {
                st::feature::Node n;
                n.kind = kind_prefix + id;
                n.label = !display_name.empty() ? display_name : id;
                n.x = 40.0f;
                n.y = next_slot_y();
                n.is_phase_break = phase_break;
                st::feature::PinId next_pin = 0;
                auto const push_pin = [&](st::HookSignal const &s, st::feature::PinDirection d) {
                    auto pt = st::feature::parse_pin_type(s.type);
                    // Canonical `name` goes into Pin.name so
                    // codegen can resolve it back to the pack's
                    // HookSignal; pretty `label` (if any)
                    // populates Pin.label for display only.
                    // Before this fix Pin.name held the label,
                    // which silently broke compilation.
                    n.pins.push_back(st::feature::Pin{next_pin++, s.name,
                                                      pt.value_or(st::feature::PinType::Float), d,
                                                      s.unit, s.label});
                };
                for (auto const &s : decl_inputs)
                    push_pin(s, in_dir);
                for (auto const &s : decl_outputs)
                    push_pin(s, out_dir);
                state.features_graph.add_node(std::move(n));
            };

            if (!hooks.empty()) {
                text_subtle("Hooks");
                ImGui::Separator();
                for (auto const &h : hooks) {
                    char const *human =
                        !h.display_name.empty() ? h.display_name.c_str() : h.id.c_str();
                    char label[200];
                    std::snprintf(label, sizeof label, "%s##hook_%s", human, h.id.c_str());
                    if (ImGui::MenuItem(label)) {
                        spawn_node("hook.", h.id, h.display_name, h.inputs, h.outputs,
                                   /*in_dir =*/st::feature::PinDirection::Output,
                                   /*out_dir=*/st::feature::PinDirection::Input,
                                   /*phase_break=*/true);
                    }
                    if (!h.description.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", h.description.c_str());
                    }
                }
            }
            if (!prims.empty()) {
                if (!hooks.empty()) {
                    ImGui::Spacing();
                }
                text_subtle("Primitives");
                ImGui::Separator();
                for (auto const &p : prims) {
                    char const *human =
                        !p.display_name.empty() ? p.display_name.c_str() : p.id.c_str();
                    char label[200];
                    std::snprintf(label, sizeof label, "%s##prim_%s", human, p.id.c_str());
                    if (ImGui::MenuItem(label)) {
                        spawn_node("primitive.", p.id, p.display_name, p.inputs, p.outputs,
                                   /*in_dir =*/st::feature::PinDirection::Input,
                                   /*out_dir=*/st::feature::PinDirection::Output,
                                   /*phase_break=*/false);
                    }
                    if (!p.description.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", p.description.c_str());
                    }
                }
            }
            ImGui::EndPopup();
        }
    } else {
        if (ImGui::Button("Add source (Float out)")) {
            st::feature::Node n;
            n.kind = "sensor.float";
            n.label = "Source";
            n.x = 40.0f;
            n.y = next_slot_y();
            n.pins.push_back(st::feature::Pin{0, "out", st::feature::PinType::Float,
                                              st::feature::PinDirection::Output, ""});
            state.features_graph.add_node(std::move(n));
        }
        ImGui::SameLine();
        if (ImGui::Button("Add sink (Float in)")) {
            st::feature::Node n;
            n.kind = "output.float";
            n.label = "Sink";
            n.x = 320.0f;
            n.y = next_slot_y();
            n.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                              st::feature::PinDirection::Input, ""});
            state.features_graph.add_node(std::move(n));
        }
        ImGui::SameLine();
        if (ImGui::Button("Add 2-in/1-out (Float)")) {
            st::feature::Node n;
            n.kind = "math.add";
            n.label = "Add";
            n.x = 180.0f;
            n.y = next_slot_y();
            n.pins.push_back(st::feature::Pin{0, "a", st::feature::PinType::Float,
                                              st::feature::PinDirection::Input, ""});
            n.pins.push_back(st::feature::Pin{1, "b", st::feature::PinType::Float,
                                              st::feature::PinDirection::Input, ""});
            n.pins.push_back(st::feature::Pin{2, "out", st::feature::PinType::Float,
                                              st::feature::PinDirection::Output, ""});
            state.features_graph.add_node(std::move(n));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("\xEE\x9D\x9A  Clear graph")) {
        state.features_graph = st::feature::Graph{};
        state.features_wiring_active = false;
        state.features_wire_error.clear();
        state.features_selected_nodes.clear();
        state.features_selected_edge.reset();
        state.features_context_edge.reset();
        state.features_band_active = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("\xEE\x9C\xAC  Reset view")) {
        state.features_view_offset = ImVec2(0.0f, 0.0f);
        state.features_view_scale = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("\xEE\x9D\x8E  Save…")) {
        nfdu8filteritem_t filters[1] = {{"SubuwuTuner mod (TOML)", "stmod"}};
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::SaveDialog(out, filters, 1, nullptr, "graph.stmod");
        if (r == NFD_OKAY) {
            std::ofstream ofs{out.get(), std::ios::trunc};
            if (!ofs) {
                state.features_wire_error = std::string{"save: cannot open "} + out.get();
            } else {
                ofs << st::feature::to_toml(state.features_graph);
                state.features_wire_error.clear();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("\xEE\x86\x97  Load…")) {
        nfdu8filteritem_t filters[1] = {{"SubuwuTuner mod (TOML)", "stmod"}};
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
        if (r == NFD_OKAY) {
            std::ifstream ifs{out.get(), std::ios::binary};
            if (!ifs) {
                state.features_wire_error = std::string{"load: cannot open "} + out.get();
            } else {
                std::stringstream buf;
                buf << ifs.rdbuf();
                auto loaded = st::feature::from_toml(buf.str());
                if (!loaded.has_value()) {
                    state.features_wire_error = "load: " + loaded.error().to_string();
                } else {
                    state.features_graph = std::move(*loaded);
                    state.features_wiring_active = false;
                    state.features_wire_error.clear();
                    state.features_selected_nodes.clear();
                    state.features_selected_edge.reset();
                    state.features_context_edge.reset();
                    state.features_band_active = false;
                }
            }
        }
    }

    // Status chip — three-state, styled like the status-bar profile
    // chip so the editor's quality signal lives at the eye-line of
    // the toolbar rather than in muted body text below it. Click
    // opens a popup with the actionable detail.
    auto const validate_result = state.features_graph.validate();
    auto const lint_findings = st::feature::lint(state.features_graph);
    char const *chip_label;
    ImVec4 chip_fg, chip_bg;
    char chip_buf[48];
    if (!validate_result.has_value()) {
        chip_label = "Invalid";
        chip_fg = chip_fg_danger();
        chip_bg = chip_bg_danger();
    } else if (!lint_findings.empty()) {
        std::snprintf(chip_buf, sizeof chip_buf, "%zu warning%s", lint_findings.size(),
                      lint_findings.size() == 1 ? "" : "s");
        chip_label = chip_buf;
        chip_fg = chip_fg_warn();
        chip_bg = chip_bg_warn();
    } else {
        chip_label = "OK";
        chip_fg = chip_fg_ok();
        chip_bg = chip_bg_ok();
    }
    chip(chip_label, chip_fg, chip_bg);
    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup("##features_status_popup");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click for details.");
    }
    if (ImGui::BeginPopup("##features_status_popup")) {
        if (!validate_result.has_value()) {
            ImGui::TextColored(chip_fg_danger(), "Structural error");
            ImGui::Separator();
            ImGui::TextWrapped("%s", validate_result.error().to_string().c_str());
        } else if (!lint_findings.empty()) {
            text_subtle("Completeness warnings");
            ImGui::Separator();
            for (auto const &f : lint_findings) {
                ImGui::BulletText("%s", f.message.c_str());
            }
        } else {
            text_subtle("Graph passes structural validation "
                        "and has no completeness warnings.");
        }
        ImGui::EndPopup();
    }

    if (!state.features_wire_error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "wire: %s",
                           state.features_wire_error.c_str());
    }
    text_subtle("Click a node or edge to select; Shift+click "
                "to multi-select. Drag from empty canvas to "
                "box-select. Delete removes the selection. "
                "Drag a body to move (group-drag works); "
                "drag pin → pin to wire. Middle-drag pans, "
                "wheel zooms. Right-click (when not wiring) "
                "for delete / disconnect.");

    ImGui::Spacing();

    // ---- Canvas ----------------------------------------------------
    // Layout constants in graph space. The viewport applies a pan
    // (`view_offset`) and zoom (`view_scale`) on top, so anything
    // that converts a graph-space measurement to screen pixels has
    // to multiply by `scale`. Snap-to-grid stays in graph space —
    // the grid drawing scales but the snap step doesn't.
    constexpr float kNodeWidth = 150.0f;
    constexpr float kHeaderHeight = 24.0f;
    constexpr float kPinRowHeight = 20.0f;
    constexpr float kPinRadius = 5.0f;
    constexpr float kPinHitRadius = 10.0f;
    constexpr float kFooterPadding = 8.0f;
    constexpr float kGridStep = 24.0f;

    ImVec2 const canvas_p = ImGui::GetCursorScreenPos();
    ImVec2 const canvas_sz = ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x),
                                    std::max(240.0f, ImGui::GetContentRegionAvail().y - 8.0f));
    ImVec2 const canvas_end(canvas_p.x + canvas_sz.x, canvas_p.y + canvas_sz.y);

    // View transform. Lambdas take graph-space, return screen-space
    // (and back). Kept as locals so the rest of the function can
    // read `scale` directly when it's the cleaner expression.
    float const scale = state.features_view_scale;
    ImVec2 const offset = state.features_view_offset;
    auto const to_screen = [&](float gx, float gy) {
        return ImVec2(canvas_p.x + offset.x + gx * scale, canvas_p.y + offset.y + gy * scale);
    };

    // Pan + zoom input. Middle-mouse drag pans; mouse wheel zooms
    // anchored to the cursor (the graph point under the cursor stays
    // put as the scale changes). Both gated on canvas hover so they
    // don't fight with toolbar / context menus elsewhere in the panel.
    bool const canvas_hovered = ImGui::IsMouseHoveringRect(canvas_p, canvas_end) &&
                                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        ImVec2 const d = ImGui::GetIO().MouseDelta;
        state.features_view_offset.x += d.x;
        state.features_view_offset.y += d.y;
    }
    if (canvas_hovered) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float const old_scale = state.features_view_scale;
            float const factor = wheel > 0.0f ? 1.1f : 1.0f / 1.1f;
            float const new_scale = std::clamp(old_scale * factor, 0.25f, 3.0f);
            if (new_scale != old_scale) {
                ImVec2 const m = ImGui::GetIO().MousePos;
                // Graph-space point under the cursor at the old scale.
                float const gx = (m.x - canvas_p.x - state.features_view_offset.x) / old_scale;
                float const gy = (m.y - canvas_p.y - state.features_view_offset.y) / old_scale;
                state.features_view_offset.x = m.x - canvas_p.x - gx * new_scale;
                state.features_view_offset.y = m.y - canvas_p.y - gy * new_scale;
                state.features_view_scale = new_scale;
            }
        }
    }

    auto *const dl = ImGui::GetWindowDrawList();
    ImU32 const grid_col = ImGui::GetColorU32(ImGuiCol_Separator);
    ImU32 const bg_col = ImGui::GetColorU32(ImGuiCol_ChildBg);
    dl->AddRectFilled(canvas_p, canvas_end, bg_col);
    // Grid lines: drawn in graph-aligned screen positions. Hidden
    // when the on-screen step would be too dense to be useful.
    {
        float const step_s = kGridStep * scale;
        if (step_s >= 4.0f) {
            float start_x = std::fmod(state.features_view_offset.x, step_s);
            if (start_x > 0.0f)
                start_x -= step_s;
            for (float x = start_x; x < canvas_sz.x; x += step_s) {
                if (x < 0.0f)
                    continue;
                dl->AddLine(ImVec2(canvas_p.x + x, canvas_p.y),
                            ImVec2(canvas_p.x + x, canvas_p.y + canvas_sz.y), grid_col);
            }
            float start_y = std::fmod(state.features_view_offset.y, step_s);
            if (start_y > 0.0f)
                start_y -= step_s;
            for (float y = start_y; y < canvas_sz.y; y += step_s) {
                if (y < 0.0f)
                    continue;
                dl->AddLine(ImVec2(canvas_p.x, canvas_p.y + y),
                            ImVec2(canvas_p.x + canvas_sz.x, canvas_p.y + y), grid_col);
            }
        }
    }
    dl->PushClipRect(canvas_p, canvas_end, true);

    // Per-pin screen-position cache. Built while drawing nodes and
    // read when rendering edges + during wiring hit-tests.
    struct PinPos {
        st::feature::NodeId node_id;
        st::feature::PinId pin_id;
        st::feature::PinDirection direction;
        st::feature::PinType type;
        ImVec2 pos;
    };
    std::vector<PinPos> pin_positions;
    pin_positions.reserve(state.features_graph.nodes().size() * 3);

    // Set of (node, pin) pairs for Input pins currently driven by
    // an edge. Used both to render the "= value" suffix on
    // unconnected-but-defaulted pins and to gate the default-value
    // editor (only shows on undriven inputs). Pre-computed once so
    // the per-pin path doesn't redo it.
    auto const pin_key = [](st::feature::NodeId nid, st::feature::PinId pid) {
        return (static_cast<std::uint64_t>(nid) << 32) | static_cast<std::uint64_t>(pid);
    };
    std::unordered_set<std::uint64_t> driven_inputs;
    for (auto const &e : state.features_graph.edges()) {
        driven_inputs.insert(pin_key(e.to_node, e.to_pin));
    }
    auto const is_pin_driven = [&](st::feature::NodeId nid, st::feature::PinId pid) {
        return driven_inputs.find(pin_key(nid, pid)) != driven_inputs.end();
    };

    // Helper: what text the editor renders for a pin's label. For
    // unconnected Input pins with a default_value set, the suffix
    // "= value" is appended so the constant is visible inline.
    // Bool prints true/false; Int truncates; Float uses %g.
    auto const pin_display_text = [&](st::feature::Node const &node,
                                      st::feature::Pin const &p) -> std::string {
        // Display prefers `label` (the pretty name from the pack);
        // falls back to `name` (the snake_case canonical) when no
        // label was supplied. Codegen lookups still use `name`.
        std::string const &disp = !p.label.empty() ? p.label : p.name;
        if (p.direction == st::feature::PinDirection::Input && !is_pin_driven(node.id, p.id) &&
            p.default_value.has_value()) {
            char buf[96];
            if (p.type == st::feature::PinType::Bool) {
                std::snprintf(buf, sizeof buf, "%s = %s", disp.c_str(),
                              *p.default_value > 0.5 ? "true" : "false");
            } else if (p.type == st::feature::PinType::Int) {
                std::snprintf(buf, sizeof buf, "%s = %lld", disp.c_str(),
                              static_cast<long long>(*p.default_value));
            } else {
                std::snprintf(buf, sizeof buf, "%s = %g", disp.c_str(), *p.default_value);
            }
            return buf;
        }
        return disp;
    };

    // Pending default-value mutation, applied after the loop. Empty
    // value clears the default; populated value sets it. Captured
    // by the pin context menu's editor.
    struct PendingDefault {
        st::feature::NodeId node_id;
        st::feature::PinId pin_id;
        std::optional<double> value;
    };
    std::optional<PendingDefault> pending_default;

    // Pin appearance constants — derived from the active accent. The
    // pin fill matches the type's category (today only Float/Int/Bool
    // share one color; future work could vary).
    auto const accent = accent_for(current_theme());
    ImU32 const pin_fill = ImGui::GetColorU32(accent.base);
    ImU32 const pin_brd = ImGui::GetColorU32(ImGuiCol_Border);
    ImU32 const node_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 const node_brd = ImGui::GetColorU32(ImGuiCol_Border);
    ImU32 const hdr_bg = ImGui::GetColorU32(ImGuiCol_TitleBgActive);
    ImU32 const txt_col = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 const disabled_t = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    (void)disabled_t; // reserved for future pin labels in disabled state

    // Snapshot the nodes by id so we can iterate stably while issuing
    // ImGui::InvisibleButton hit-tests (the node positions get
    // mutated mid-loop via set_node_position).
    auto const &nodes = state.features_graph.nodes();

    // Per-node screen-rect cache, populated during the draw pass and
    // consumed by the rubber-band hit-test below. Saves recomputing
    // the per-node width measurement (which involves CalcTextSizeA
    // on every pin label).
    struct NodeRect {
        st::feature::NodeId id;
        ImVec2 tl;
        ImVec2 br;
    };
    std::vector<NodeRect> node_rects;
    node_rects.reserve(nodes.size());

    // Pending mutations from context-menu actions. Deferred until
    // after the loop so we don't invalidate iteration / pin caches.
    // Nodes are batched for the multi-select Delete-key path; the
    // edge variant stays singular (no edge multi-select yet).
    std::vector<st::feature::NodeId> pending_delete_nodes;
    std::optional<st::feature::Edge> pending_delete_edge;

    // Delete key removes the current selection — every selected
    // node, or the selected edge, depending on which one is set.
    // Gated on window focus so deletes elsewhere (table-cell editing,
    // etc.) don't blow away the designer's selection by accident.
    bool const designer_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (designer_focused && ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false)) {
        if (!state.features_selected_nodes.empty()) {
            pending_delete_nodes = state.features_selected_nodes;
        } else if (state.features_selected_edge.has_value()) {
            pending_delete_edge = state.features_selected_edge;
        }
    }

    ImFont *const fnt = ImGui::GetFont();
    float const fnt_sz_s = ImGui::GetFontSize() * scale;
    float const hdr_h_s = kHeaderHeight * scale;
    float const row_h_s = kPinRowHeight * scale;
    float const foot_s = kFooterPadding * scale;
    float const width_s = kNodeWidth * scale;
    float const pin_r_s = kPinRadius * scale;
    float const pin_hr_s = kPinHitRadius * scale;

    // Tracks whether any per-node body or pin InvisibleButton ended
    // up hovered this frame. Edge hit-test consumes this to decide
    // whether to even consider beziers — but ImGui's stock
    // `IsAnyItemHovered()` ORs in the *previous* frame's hovered id,
    // which means the canvas-bg button (always hovered last frame
    // when cursor is over empty canvas) keeps it permanently true.
    // Tracking manually inside the loop dodges that one-frame lag.
    bool any_node_pin_hovered = false;

    for (auto const &n : nodes) {
        std::size_t const inputs = static_cast<std::size_t>(
            std::count_if(n.pins.begin(), n.pins.end(), [](st::feature::Pin const &p) {
                return p.direction == st::feature::PinDirection::Input;
            }));
        std::size_t const outputs = static_cast<std::size_t>(
            std::count_if(n.pins.begin(), n.pins.end(), [](st::feature::Pin const &p) {
                return p.direction == st::feature::PinDirection::Output;
            }));
        std::size_t const rows = std::max<std::size_t>(1, std::max(inputs, outputs));
        float const node_h_s = hdr_h_s + static_cast<float>(rows) * row_h_s + foot_s;

        // Per-node body width — fits the longest input-name +
        // longest output-name on any row, plus the header label,
        // so pack-driven hooks with verbose signal names (e.g.
        // `commanded_pw_override`) don't overflow the 150 px
        // default. Falls back to the default when labels are short.
        char const *header_text = n.label.empty() ? n.kind.c_str() : n.label.c_str();
        float const header_w = fnt->CalcTextSizeA(fnt_sz_s, FLT_MAX, 0.0f, header_text).x;
        float max_in_w = 0.0f;
        float max_out_w = 0.0f;
        for (auto const &p : n.pins) {
            std::string const txt = pin_display_text(n, p);
            float const w = fnt->CalcTextSizeA(fnt_sz_s, FLT_MAX, 0.0f, txt.c_str()).x;
            if (p.direction == st::feature::PinDirection::Input) {
                max_in_w = std::max(max_in_w, w);
            } else {
                max_out_w = std::max(max_out_w, w);
            }
        }
        // Pin row: [circle][6px gap][in_name] … [out_name][6px gap][circle]
        float const pad_s = 6.0f * scale;
        float const inner_gap_s = 12.0f * scale; // visible gutter
                                                 // between the two
                                                 // text columns
        float const pins_w = pin_r_s + pad_s + max_in_w + inner_gap_s + max_out_w + pad_s + pin_r_s;
        float const header_w_total = header_w + 16.0f * scale;
        float const node_w_s = std::max({width_s, pins_w, header_w_total});

        ImVec2 const node_tl = to_screen(n.x, n.y);
        ImVec2 const node_br(node_tl.x + node_w_s, node_tl.y + node_h_s);
        node_rects.push_back({n.id, node_tl, node_br});

        // Body hit zone — InvisibleButton lives at the body origin so
        // ImGui's IsItemActive + drag-delta machinery handles the
        // move logic without needing manual click bookkeeping.
        char body_id[24];
        std::snprintf(body_id, sizeof body_id, "##fnode_%u", static_cast<unsigned>(n.id));
        ImGui::SetCursorScreenPos(node_tl);
        ImGui::InvisibleButton(body_id, ImVec2(node_w_s, node_h_s));
        bool const body_hovered = ImGui::IsItemHovered();
        if (body_hovered)
            any_node_pin_hovered = true;
        bool const body_active = ImGui::IsItemActive();
        bool const body_deactivated = ImGui::IsItemDeactivated();
        bool const body_left_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool const body_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

        // Hover tooltip — surfaces the human-friendly description
        // from the pack-declared hook or primitive (or just the
        // kind for generic debug nodes). Suppressed during drag so
        // it doesn't track the cursor mid-move. Suppressed while
        // wiring so the user can read the in-flight target instead.
        if (body_hovered && !body_active && !state.features_wiring_active) {
            char const *node_kind = n.kind.c_str();
            std::string description;
            bool is_pack_node = false;
            if (state.project.has_value()) {
                auto const &def = state.project->definition();
                if (n.kind.starts_with("hook.")) {
                    is_pack_node = true;
                    std::string_view const id{n.kind.data() + 5, n.kind.size() - 5};
                    if (auto const *h = def.find_hook(id); h != nullptr) {
                        description = h->description;
                    }
                } else if (n.kind.starts_with("primitive.")) {
                    is_pack_node = true;
                    std::string_view const id{n.kind.data() + 10, n.kind.size() - 10};
                    if (auto const *p = def.find_primitive(id); p != nullptr) {
                        description = p->description;
                    }
                }
            }
            ImGui::BeginTooltip();
            char const *title_str = !n.label.empty() ? n.label.c_str() : node_kind;
            ImGui::TextUnformatted(title_str);
            text_subtle("%s", node_kind);
            if (!description.empty()) {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextWrapped("%s", description.c_str());
                ImGui::PopTextWrapPos();
            } else if (!is_pack_node) {
                ImGui::Separator();
                text_subtle("Generic debug node. Load a project with hooks "
                            "to insert pack-declared nodes from the palette.");
            }
            ImGui::EndTooltip();
        }
        if (body_left_clicked) {
            // Shift toggles the node in/out of the selection set.
            // Plain click on a non-selected node replaces with
            // {this}; plain click on an already-selected node keeps
            // the selection intact so a subsequent drag moves every
            // selected node together (file-manager / Sketch / Figma
            // style group-drag). Edge selection always clears — only
            // one kind of thing highlighted at a time.
            bool const shift = ImGui::GetIO().KeyShift;
            auto &sel = state.features_selected_nodes;
            auto it = std::find(sel.begin(), sel.end(), n.id);
            bool const was_in_sel = (it != sel.end());
            if (shift) {
                if (was_in_sel)
                    sel.erase(it);
                else
                    sel.push_back(n.id);
            } else if (!was_in_sel) {
                sel.assign({n.id});
            }
            state.features_selected_edge.reset();
        }
        if (body_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            // Mouse delta is in screen pixels; convert back to graph
            // space before storing — otherwise a zoomed-in canvas
            // would over-shoot moves and a zoomed-out one would lag.
            // Move every selected node by the same delta so group
            // drag works; fall back to just the active node if it
            // somehow isn't in the selection (shouldn't happen given
            // the click handler above, but the cost is one set
            // membership check).
            ImVec2 const d = ImGui::GetIO().MouseDelta;
            auto const &sel = state.features_selected_nodes;
            bool const active_in_sel = std::find(sel.begin(), sel.end(), n.id) != sel.end();
            if (active_in_sel) {
                for (auto id : sel) {
                    if (auto const *nn = state.features_graph.find_node(id)) {
                        state.features_graph.set_node_position(id, nn->x + d.x / scale,
                                                               nn->y + d.y / scale);
                    }
                }
            } else {
                state.features_graph.set_node_position(n.id, n.x + d.x / scale, n.y + d.y / scale);
            }
        }
        // Snap-to-grid on drag release. IsItemDeactivated fires once
        // on the frame the active button stops being active — exactly
        // the "user just let go" event we want. Snap is intentionally
        // release-only (not live) so the drag feels smooth and the
        // tidy-up happens after the user commits the move. With
        // group drag, snap every selected node so the whole group
        // lands on the grid.
        if (body_deactivated) {
            auto const &sel = state.features_selected_nodes;
            bool const active_in_sel = std::find(sel.begin(), sel.end(), n.id) != sel.end();
            auto const snap_one = [&](st::feature::NodeId id) {
                if (auto const *nn = state.features_graph.find_node(id)) {
                    float const sx = std::round(nn->x / kGridStep) * kGridStep;
                    float const sy = std::round(nn->y / kGridStep) * kGridStep;
                    state.features_graph.set_node_position(id, sx, sy);
                }
            };
            if (active_in_sel) {
                for (auto id : sel)
                    snap_one(id);
            } else {
                snap_one(n.id);
            }
        }
        // Right-click on the body opens a context menu. Suppressed
        // when the click cancelled an in-progress wire (handled at
        // the top of the function) — using `was_wiring_at_start`
        // because by this point in the frame, features_wiring_active
        // has already been cleared.
        char node_popup_id[28];
        std::snprintf(node_popup_id, sizeof node_popup_id, "##nctx_%u",
                      static_cast<unsigned>(n.id));
        if (body_right_clicked && !was_wiring_at_start) {
            ImGui::OpenPopup(node_popup_id);
        }
        if (ImGui::BeginPopup(node_popup_id)) {
            text_subtle("%s", n.label.empty() ? n.kind.c_str() : n.label.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Node")) {
                pending_delete_nodes.push_back(n.id);
            }
            ImGui::EndPopup();
        }

        // Draw chrome. Selected nodes get a thicker accent-coloured
        // border so the active selection reads at a glance.
        bool const is_selected =
            std::find(state.features_selected_nodes.begin(), state.features_selected_nodes.end(),
                      n.id) != state.features_selected_nodes.end();
        dl->AddRectFilled(node_tl, node_br, node_bg, 6.0f);
        dl->AddRectFilled(node_tl, ImVec2(node_br.x, node_tl.y + hdr_h_s), hdr_bg, 6.0f,
                          ImDrawFlags_RoundCornersTop);
        ImU32 const this_brd = is_selected ? ImGui::GetColorU32(accent.base) : node_brd;
        float const this_thick = is_selected ? 2.5f : 1.0f;
        dl->AddRect(node_tl, node_br, this_brd, 6.0f, 0, this_thick);
        // Header label. Drawn at the scaled font size so it tracks
        // the zoom level — ImGui's default AddText overload uses the
        // active font size, which is wrong here.
        char const *label = n.label.empty() ? n.kind.c_str() : n.label.c_str();
        dl->AddText(fnt, fnt_sz_s,
                    ImVec2(node_tl.x + 8.0f * scale, node_tl.y + (hdr_h_s - fnt_sz_s) * 0.5f),
                    txt_col, label);

        // Lay pins out by direction. Inputs sit on the left edge,
        // outputs on the right; both vertically distributed within
        // the pin-row band.
        std::size_t in_idx = 0;
        std::size_t out_idx = 0;
        for (auto const &p : n.pins) {
            bool const is_input = p.direction == st::feature::PinDirection::Input;
            std::size_t const row_idx = is_input ? in_idx++ : out_idx++;
            float const row_center_y =
                node_tl.y + hdr_h_s + (static_cast<float>(row_idx) + 0.5f) * row_h_s;
            float const pin_x = is_input ? node_tl.x : node_br.x;
            ImVec2 const pin_center(pin_x, row_center_y);

            dl->AddCircleFilled(pin_center, pin_r_s, pin_fill);
            dl->AddCircle(pin_center, pin_r_s, pin_brd);
            // Pin name beside the circle, on the inside edge. Text
            // width measured at the scaled font size so the trailing
            // offset for output pins stays correct under zoom. The
            // display text includes a "= value" suffix when the
            // pin is an undriven Input with a default_value — the
            // constant editor populates this and the renderer
            // surfaces it inline.
            std::string const display_text = pin_display_text(n, p);
            ImVec2 const text_size =
                fnt->CalcTextSizeA(fnt_sz_s, FLT_MAX, 0.0f, display_text.c_str());
            ImVec2 const text_pos =
                is_input
                    ? ImVec2(pin_center.x + pin_r_s + 6.0f * scale, row_center_y - fnt_sz_s * 0.5f)
                    : ImVec2(pin_center.x - pin_r_s - 6.0f * scale - text_size.x,
                             row_center_y - fnt_sz_s * 0.5f);
            dl->AddText(fnt, fnt_sz_s, text_pos, txt_col, display_text.c_str());

            pin_positions.push_back(PinPos{n.id, p.id, p.direction, p.type, pin_center});

            // Pin hit zone — slightly larger than the visible circle
            // so the pin is easy to grab. Tracked by InvisibleButton
            // so we can detect drag start cleanly without colliding
            // with the body's hit zone (which we cover with the body
            // button above; ImGui's last-issued button wins on overlap,
            // so issuing the pin zone AFTER the body means the pin
            // takes precedence in the overlap region).
            char pin_id_buf[40];
            std::snprintf(pin_id_buf, sizeof pin_id_buf, "##fpin_%u_%u",
                          static_cast<unsigned>(n.id), static_cast<unsigned>(p.id));
            ImGui::SetCursorScreenPos(ImVec2(pin_center.x - pin_hr_s, pin_center.y - pin_hr_s));
            ImGui::InvisibleButton(pin_id_buf, ImVec2(pin_hr_s * 2.0f, pin_hr_s * 2.0f));
            bool const pin_hovered = ImGui::IsItemHovered();
            bool const pin_active = ImGui::IsItemActive();
            if (pin_hovered)
                any_node_pin_hovered = true;
            // Tooltip: prefers the pretty label, parenthesizes the
            // canonical name when both are present (so users can see
            // the snake_case id needed for hand-authoring .stmod
            // files or for codegen error messages).
            if (pin_hovered) {
                if (!p.label.empty() && p.label != p.name) {
                    ImGui::SetTooltip("%s (%s) : %s%s%s", p.label.c_str(), p.name.c_str(),
                                      st::feature::pin_type_name(p.type),
                                      p.unit.empty() ? "" : "  ", p.unit.c_str());
                } else {
                    ImGui::SetTooltip("%s : %s%s%s", p.name.c_str(),
                                      st::feature::pin_type_name(p.type),
                                      p.unit.empty() ? "" : "  ", p.unit.c_str());
                }
            }
            // Start wiring on drag-out from a pin. The wiring is
            // bi-directional intent: dragging from an output starts a
            // wire heading toward an input; dragging from an input
            // starts a wire heading toward an output. We resolve the
            // direction on drop so the user doesn't have to think
            // about pin polarity.
            bool const pin_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            if (pin_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) &&
                !state.features_wiring_active && !state.features_wiring_blocked) {
                state.features_wiring_active = true;
                state.features_wiring_from_node = n.id;
                state.features_wiring_from_pin = p.id;
                state.features_wire_error.clear();
            }
            // Right-click on a pin opens a context menu listing every
            // edge touching it (typically 0 or 1, more on output fan-
            // out). Each row deletes that edge.
            char pin_popup_id[40];
            std::snprintf(pin_popup_id, sizeof pin_popup_id, "##pctx_%u_%u",
                          static_cast<unsigned>(n.id), static_cast<unsigned>(p.id));
            if (pin_right_clicked && !was_wiring_at_start) {
                ImGui::OpenPopup(pin_popup_id);
                // Pre-fill the constant editor with the pin's
                // current value (or 0 when none), so the InputFloat
                // inside the popup starts on the right number.
                state.features_pin_edit_buf =
                    p.default_value.has_value() ? static_cast<float>(*p.default_value) : 0.0f;
            }
            if (ImGui::BeginPopup(pin_popup_id)) {
                if (!p.label.empty() && p.label != p.name) {
                    text_subtle("%s (%s) : %s", p.label.c_str(), p.name.c_str(),
                                st::feature::pin_type_name(p.type));
                } else {
                    text_subtle("%s : %s", p.name.c_str(), st::feature::pin_type_name(p.type));
                }
                ImGui::Separator();
                bool any_edge = false;
                for (auto const &e : state.features_graph.edges()) {
                    bool const touches = (e.from_node == n.id && e.from_pin == p.id) ||
                                         (e.to_node == n.id && e.to_pin == p.id);
                    if (!touches)
                        continue;
                    any_edge = true;
                    // Label the other end so the user knows what
                    // they're disconnecting on a fan-out.
                    st::feature::NodeId other_n = (e.from_node == n.id) ? e.to_node : e.from_node;
                    st::feature::PinId other_p = (e.from_node == n.id) ? e.to_pin : e.from_pin;
                    auto const *on = state.features_graph.find_node(other_n);
                    auto const *op = state.features_graph.find_pin(other_n, other_p);
                    char item_label[96];
                    std::snprintf(item_label, sizeof item_label, "Disconnect from %s.%s",
                                  on != nullptr
                                      ? (on->label.empty() ? on->kind.c_str() : on->label.c_str())
                                      : "?",
                                  op != nullptr ? op->name.c_str() : "?");
                    if (ImGui::MenuItem(item_label)) {
                        pending_delete_edge = e;
                    }
                }
                // Constant-value editor — only shown on Input pins
                // (Output pins don't consume values) and only when
                // no edge drives the pin (a driver always wins over
                // the default; surfacing the editor for a wired
                // input would be misleading).
                bool const editor_eligible =
                    p.direction == st::feature::PinDirection::Input && !is_pin_driven(n.id, p.id);
                if (editor_eligible) {
                    if (any_edge)
                        ImGui::Separator();
                    text_subtle("Constant value");
                    ImGui::SetNextItemWidth(120.0f);
                    bool commit = false;
                    if (p.type == st::feature::PinType::Bool) {
                        bool b = state.features_pin_edit_buf > 0.5f;
                        if (ImGui::Checkbox("##pin_def_bool", &b)) {
                            state.features_pin_edit_buf = b ? 1.0f : 0.0f;
                            commit = true;
                        }
                    } else if (p.type == st::feature::PinType::Int) {
                        int v = static_cast<int>(state.features_pin_edit_buf);
                        if (ImGui::InputInt("##pin_def_int", &v, 0, 0,
                                            ImGuiInputTextFlags_EnterReturnsTrue)) {
                            state.features_pin_edit_buf = static_cast<float>(v);
                            commit = true;
                        }
                    } else {
                        if (ImGui::InputFloat("##pin_def_float", &state.features_pin_edit_buf, 0.0f,
                                              0.0f, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) {
                            commit = true;
                        }
                    }
                    if (commit) {
                        pending_default = PendingDefault{
                            n.id, p.id, static_cast<double>(state.features_pin_edit_buf)};
                        ImGui::CloseCurrentPopup();
                    }
                    if (p.default_value.has_value()) {
                        if (ImGui::MenuItem("Clear Value")) {
                            pending_default = PendingDefault{n.id, p.id, std::nullopt};
                        }
                    }
                }
                if (!any_edge && !editor_eligible) {
                    text_subtle("(no edges)");
                }
                ImGui::EndPopup();
            }
            // Highlight pins that would accept the in-flight wire.
            if (state.features_wiring_active && (state.features_wiring_from_node != n.id ||
                                                 state.features_wiring_from_pin != p.id)) {
                auto const *from = state.features_graph.find_pin(state.features_wiring_from_node,
                                                                 state.features_wiring_from_pin);
                if (from != nullptr && from->direction != p.direction && from->type == p.type) {
                    dl->AddCircle(pin_center, pin_r_s + 3.0f * scale, pin_fill, 0, 2.0f);
                }
            }
        }
    }

    // ---- Edge hit-test, click handling, and rendering -------------
    // Edges aren't ImGui items (no InvisibleButton — they're
    // drawn-only beziers), so click handling rolls its own. The
    // approach: sample N points along each curve, find the closest
    // edge to the cursor under an 8px threshold. Skip while any
    // node/pin is hovered (those win in overlap) and while a wire
    // is in flight (the in-flight bezier would self-hit otherwise).
    ImU32 const edge_col = ImGui::GetColorU32(accent.base);
    ImU32 const edge_sel_col = ImGui::GetColorU32(accent.hover);
    auto const find_pos = [&](st::feature::NodeId nid, st::feature::PinId pid) -> ImVec2 const * {
        for (auto const &pp : pin_positions) {
            if (pp.node_id == nid && pp.pin_id == pid)
                return &pp.pos;
        }
        return nullptr;
    };
    auto const edge_eq = [](st::feature::Edge const &a, st::feature::Edge const &b) {
        return a.from_node == b.from_node && a.from_pin == b.from_pin && a.to_node == b.to_node &&
               a.to_pin == b.to_pin;
    };
    auto const cubic_at = [](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
        float const u = 1.0f - t;
        float const w0 = u * u * u;
        float const w1 = 3.0f * u * u * t;
        float const w2 = 3.0f * u * t * t;
        float const w3 = t * t * t;
        return ImVec2(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                      w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y);
    };

    auto const &edges = state.features_graph.edges();

    // Find the closest edge under the cursor. Empty means none in
    // range or hit-test suppressed this frame.
    std::optional<std::size_t> hovered_edge_idx;
    {
        bool const can_hit = !state.features_wiring_active && !any_node_pin_hovered &&
                             ImGui::IsMouseHoveringRect(canvas_p, canvas_end);
        if (can_hit) {
            ImVec2 const mouse = ImGui::GetIO().MousePos;
            // 14 px screen-space threshold — wide enough that the
            // user doesn't need to land precisely on the 2.5-3 px
            // stroke. Tightening this past ~10 makes the curve
            // genuinely fiddly to click on a moving cursor.
            float const thresh_px = 14.0f;
            float best_d2 = thresh_px * thresh_px;
            constexpr int kSamples = 24;
            for (std::size_t i = 0; i < edges.size(); ++i) {
                auto const &e = edges[i];
                auto const *a = find_pos(e.from_node, e.from_pin);
                auto const *b = find_pos(e.to_node, e.to_pin);
                if (a == nullptr || b == nullptr)
                    continue;
                float const d = std::max(40.0f, std::abs(b->x - a->x) * 0.5f);
                ImVec2 const c1(a->x + d, a->y);
                ImVec2 const c2(b->x - d, b->y);
                for (int s = 0; s <= kSamples; ++s) {
                    float const t = static_cast<float>(s) / static_cast<float>(kSamples);
                    ImVec2 const pt = cubic_at(*a, c1, c2, *b, t);
                    float const dx = pt.x - mouse.x;
                    float const dy = pt.y - mouse.y;
                    float const d2 = dx * dx + dy * dy;
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        hovered_edge_idx = i;
                    }
                }
            }
        }
    }

    // Edge click → select (clears node selection — only one thing
    // highlighted at a time). Right-click → open context menu
    // WITHOUT changing selection; the popup reads from
    // features_context_edge instead, so dismissing the menu leaves
    // the prior selection alone (matches typical desktop UX).
    bool edge_consumed_left_click = false;
    if (hovered_edge_idx.has_value() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.features_selected_edge = edges[*hovered_edge_idx];
        state.features_selected_nodes.clear();
        edge_consumed_left_click = true;
    }
    if (hovered_edge_idx.has_value() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !was_wiring_at_start) {
        state.features_context_edge = edges[*hovered_edge_idx];
        ImGui::OpenPopup("##fectx");
    }
    if (ImGui::BeginPopup("##fectx")) {
        if (state.features_context_edge.has_value()) {
            auto const &se = *state.features_context_edge;
            auto const *fn = state.features_graph.find_node(se.from_node);
            auto const *fp = state.features_graph.find_pin(se.from_node, se.from_pin);
            auto const *tn = state.features_graph.find_node(se.to_node);
            auto const *tp = state.features_graph.find_pin(se.to_node, se.to_pin);
            char label[128];
            std::snprintf(
                label, sizeof label, "%s.%s → %s.%s",
                fn != nullptr ? (fn->label.empty() ? fn->kind.c_str() : fn->label.c_str()) : "?",
                fp != nullptr ? fp->name.c_str() : "?",
                tn != nullptr ? (tn->label.empty() ? tn->kind.c_str() : tn->label.c_str()) : "?",
                tp != nullptr ? tp->name.c_str() : "?");
            text_subtle("%s", label);
            ImGui::Separator();
            if (ImGui::MenuItem("Disconnect")) {
                pending_delete_edge = se;
            }
        }
        ImGui::EndPopup();
    }

    // Render edges. Selected and hovered states are deliberately
    // exaggerated — at 2.5 px default stroke a one-pixel difference
    // doesn't read on a moving cursor.
    for (std::size_t i = 0; i < edges.size(); ++i) {
        auto const &e = edges[i];
        auto const *p1 = find_pos(e.from_node, e.from_pin);
        auto const *p2 = find_pos(e.to_node, e.to_pin);
        if (p1 == nullptr || p2 == nullptr)
            continue;
        float const dist = std::max(40.0f, std::abs(p2->x - p1->x) * 0.5f);
        ImVec2 const c1(p1->x + dist, p1->y);
        ImVec2 const c2(p2->x - dist, p2->y);
        bool const is_sel =
            state.features_selected_edge.has_value() && edge_eq(*state.features_selected_edge, e);
        bool const is_hov = hovered_edge_idx.has_value() && *hovered_edge_idx == i;
        ImU32 const col = (is_sel || is_hov) ? edge_sel_col : edge_col;
        float const thick = is_sel ? 5.0f : (is_hov ? 4.0f : 2.5f);
        dl->AddBezierCubic(*p1, c1, c2, *p2, col, thick);
    }

    // ---- In-progress wire ------------------------------------------
    if (state.features_wiring_active) {
        auto const *from_pos =
            find_pos(state.features_wiring_from_node, state.features_wiring_from_pin);
        if (from_pos != nullptr) {
            ImVec2 const mouse = ImGui::GetIO().MousePos;
            float const dist = std::max(40.0f, std::abs(mouse.x - from_pos->x) * 0.5f);
            auto const *from = state.features_graph.find_pin(state.features_wiring_from_node,
                                                             state.features_wiring_from_pin);
            bool const from_is_output =
                from != nullptr && from->direction == st::feature::PinDirection::Output;
            ImVec2 const c1 = from_is_output ? ImVec2(from_pos->x + dist, from_pos->y)
                                             : ImVec2(from_pos->x - dist, from_pos->y);
            ImVec2 const c2 =
                from_is_output ? ImVec2(mouse.x - dist, mouse.y) : ImVec2(mouse.x + dist, mouse.y);
            dl->AddBezierCubic(*from_pos, c1, c2, mouse, edge_col, 2.0f);
        }

        // Mouse-up while wiring: try to land on a pin. Hit-test by
        // distance to the cached pin centers.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ImVec2 const mouse = ImGui::GetIO().MousePos;
            PinPos const *target = nullptr;
            for (auto const &pp : pin_positions) {
                if (pp.node_id == state.features_wiring_from_node &&
                    pp.pin_id == state.features_wiring_from_pin) {
                    continue;
                }
                float const dx = pp.pos.x - mouse.x;
                float const dy = pp.pos.y - mouse.y;
                if (dx * dx + dy * dy <= pin_hr_s * pin_hr_s) {
                    target = &pp;
                    break;
                }
            }
            if (target != nullptr) {
                auto const *from = state.features_graph.find_pin(state.features_wiring_from_node,
                                                                 state.features_wiring_from_pin);
                if (from != nullptr) {
                    // Normalize endpoints to (output → input) regardless
                    // of which pin the user grabbed first.
                    st::feature::NodeId src_n = state.features_wiring_from_node;
                    st::feature::PinId src_p = state.features_wiring_from_pin;
                    st::feature::NodeId dst_n = target->node_id;
                    st::feature::PinId dst_p = target->pin_id;
                    if (from->direction == st::feature::PinDirection::Input) {
                        std::swap(src_n, dst_n);
                        std::swap(src_p, dst_p);
                    }
                    auto r = state.features_graph.connect(src_n, src_p, dst_n, dst_p);
                    if (!r.has_value()) {
                        state.features_wire_error = r.error().to_string();
                    } else {
                        state.features_wire_error.clear();
                    }
                }
            }
            state.features_wiring_active = false;
        }
    }

    // Canvas-background hit zone — handles click-to-deselect AND
    // box-select rubber-banding. Issued AFTER every node body and
    // pin so the earlier items naturally claim their own hit
    // regions. The same InvisibleButton drives both gestures:
    // IsItemActivated → start the band; IsItemActive → drag the
    // band; IsItemDeactivated → finalize; IsItemClicked (which
    // ImGui fires for press+release with no movement) → treat as
    // empty-click deselect.
    ImGui::SetCursorScreenPos(canvas_p);
    ImGui::InvisibleButton("##fcanvas_bg", canvas_sz);
    bool const canvas_empty_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // Rubber-band start. Suppressed while wiring so the wire-cancel
    // path stays on the empty-release branch.
    if (ImGui::IsItemActivated() && !state.features_wiring_active) {
        state.features_band_active = true;
        state.features_band_start = ImGui::GetIO().MousePos;
    }
    // Rubber-band drag — recompute selection live each frame from
    // the rectangle. Without Shift, every drag starts from a clean
    // set so the rectangle alone determines membership. With Shift,
    // the existing selection is preserved and the rectangle adds
    // to it (toggling individual nodes already works via
    // Shift+click on a body).
    if (state.features_band_active && ImGui::IsItemActive()) {
        ImVec2 const m = ImGui::GetIO().MousePos;
        float const x0 = std::min(state.features_band_start.x, m.x);
        float const x1 = std::max(state.features_band_start.x, m.x);
        float const y0 = std::min(state.features_band_start.y, m.y);
        float const y1 = std::max(state.features_band_start.y, m.y);
        bool const additive = ImGui::GetIO().KeyShift;
        // Cache the additive set on the first frame so re-sweeps
        // don't double-count already-toggled nodes. Stored as a
        // function-local static via `state` isn't needed — we
        // just rebuild from scratch each frame using the current
        // node positions. With additive=true, start from whatever
        // was selected when the band started; without, start empty.
        std::vector<st::feature::NodeId> result;
        if (additive)
            result = state.features_selected_nodes;
        for (auto const &nr : node_rects) {
            bool const intersects = !(nr.br.x < x0 || nr.tl.x > x1 || nr.br.y < y0 || nr.tl.y > y1);
            if (!intersects)
                continue;
            if (std::find(result.begin(), result.end(), nr.id) == result.end()) {
                result.push_back(nr.id);
            }
        }
        state.features_selected_nodes = std::move(result);
        state.features_selected_edge.reset();
        // Draw the band rectangle on top of nodes + edges so it
        // reads as a marquee. Filled rect for area cue, accent
        // border for the edge. Cheap to compute, redrawn every
        // frame so it tracks the cursor without lag.
        ImU32 const fill =
            ImGui::GetColorU32(ImVec4(accent.base.x, accent.base.y, accent.base.z, 0.20f));
        ImU32 const brd = ImGui::GetColorU32(accent.base);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), brd, 0.0f, 0, 1.5f);
    }
    if (state.features_band_active && ImGui::IsItemDeactivated()) {
        state.features_band_active = false;
    }

    dl->PopClipRect();

    // Apply pending mutations from context menus. Done here, after
    // every reference into `nodes` / `pin_positions` has been
    // consumed, so we never touch invalidated state.
    if (pending_default.has_value()) {
        state.features_graph.set_pin_default(pending_default->node_id, pending_default->pin_id,
                                             pending_default->value);
    }
    if (pending_delete_edge.has_value()) {
        state.features_graph.remove_edge(*pending_delete_edge);
        if (state.features_selected_edge.has_value() &&
            edge_eq(*state.features_selected_edge, *pending_delete_edge)) {
            state.features_selected_edge.reset();
        }
    }
    for (auto const id : pending_delete_nodes) {
        state.features_graph.remove_node(id);
        // If the user was about to wire FROM this node, drop the
        // wiring state so we don't reference a vanished pin.
        if (state.features_wiring_active && state.features_wiring_from_node == id) {
            state.features_wiring_active = false;
        }
        auto &sel = state.features_selected_nodes;
        sel.erase(std::remove(sel.begin(), sel.end(), id), sel.end());
        // remove_node cascades to edges touching the removed node —
        // drop the selected edge if it referenced either endpoint.
        if (state.features_selected_edge.has_value() &&
            (state.features_selected_edge->from_node == id ||
             state.features_selected_edge->to_node == id)) {
            state.features_selected_edge.reset();
        }
    }

    // Click on empty canvas → drop selection. Gated on:
    // - edge_consumed_left_click: edges aren't ImGui items; clicking
    //   a bezier still leaves the canvas-bg button "hovered", so
    //   without this gate every edge-click would be immediately
    //   followed by a deselect-on-empty.
    // - Shift: Shift+empty-click is additive (no-op), to match the
    //   Shift+band-drag and Shift+body-click semantics.
    if (canvas_empty_clicked && !edge_consumed_left_click && !ImGui::GetIO().KeyShift) {
        state.features_selected_nodes.clear();
        state.features_selected_edge.reset();
    }

    // Reserve the canvas footprint so the rest of the window scrolls
    // sensibly when more nodes are added than fit.
    ImGui::SetCursorScreenPos(canvas_p);
    ImGui::Dummy(canvas_sz);

    ImGui::End();
}

} // namespace st::ui
