// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tables sidebar — left panel listing the calibration tables grouped
// by category, with a Ctrl+F-bindable filter input and per-table S/E
// policy badges. Click a row to select it (drives render_table_view).

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "persistence.hpp" // save_sidebar_category_order
#include "widgets/widgets.hpp"

#include "st/defs.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace st::ui {

void render_sidebar(AppState &state) {
    if (!state.show_tables_panel) {
        return;
    }
    // First-show fallback: if the user toggles Tables on from a
    // workspace whose layout didn't dock it (Datalog, Features), give
    // it a central-area tab instead of letting it land floating at the
    // default top-left, which overlaps every other docked panel.
    // FirstUseEver respects subsequent user docking choices and the
    // layout-rebuild path (build_workspace_layout's DockBuilderDockWindow).
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("Tables");

    track_help_context(state, AppState::HelpContext::Sidebar);
    if (!state.project.has_value()) {
        // Quiet empty state. The welcome panel on the right owns the
        // primary Open Project CTA; the sidebar just acknowledges that
        // tables will appear here once a project is loaded.
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to browse its calibration tables here.");
        ImGui::End();
        return;
    }

    // Right-click anywhere in the panel for ordering / visibility
    // actions. Disabled-but-present entries keep the affordance
    // discoverable on a fresh project. Filename ID prevents collision
    // with per-header menus pushed deeper in the tree.
    std::optional<std::string> requested_unhide;
    bool requested_unhide_all = false;
    if (ImGui::BeginPopupContextWindow("##tables_panel_ctx",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        bool const has_custom_order = !state.sidebar_category_order.empty();
        ImGui::BeginDisabled(!has_custom_order);
        if (ImGui::MenuItem("Reset category order")) {
            state.sidebar_category_order.clear();
            std::error_code ec;
            std::filesystem::remove(state.project->dir() / "sidebar_order.txt",
                                    ec);
            // remove() failures (file already absent / disk error) are
            // non-fatal — the in-memory clear above is what matters for
            // the live UI; persistence will re-sync next save.
        }
        ImGui::EndDisabled();
        if (!has_custom_order) {
            text_subtle("(no custom order saved)");
        }
        // Bulk reorder operations. Both rewrite sidebar_category_order
        // from scratch using the current pack's distinct categories.
        // Drag-reorder remains the per-tweak path; these are the
        // "I want everything alphabetized" / "safety stuff first"
        // gestures that the drag handle can't express in one step.
        if (ImGui::BeginMenu("Reorder categories")) {
            // Compute the distinct-category list inline. The popup is
            // rare; walking def.tables() each open is cheaper than
            // caching another vector on state. "Other" is the
            // first-occurrence bucket the render loop uses for
            // uncategorized tables — same name here so the persisted
            // order round-trips through the load path.
            struct CatInfo {
                std::string_view name;
                bool any_safety{false};
            };
            std::vector<CatInfo> cats;
            cats.reserve(8);
            auto const find_or_make_cat = [&](std::string_view n) -> CatInfo & {
                for (auto &c : cats) {
                    if (c.name == n) {
                        return c;
                    }
                }
                cats.push_back({n, false});
                return cats.back();
            };
            for (auto const &t : state.project->definition().tables()) {
                std::string_view const cat =
                    t.category.empty() ? std::string_view{"Other"}
                                       : std::string_view{t.category};
                auto &c = find_or_make_cat(cat);
                if (t.engine_safety_critical) {
                    c.any_safety = true;
                }
            }
            auto const persist_order = [&]() {
                save_sidebar_category_order(state.project->dir(),
                                            state.sidebar_category_order);
            };
            if (ImGui::MenuItem("Alphabetize")) {
                std::vector<std::string> order;
                order.reserve(cats.size());
                for (auto const &c : cats) {
                    order.emplace_back(c.name);
                }
                std::sort(order.begin(), order.end());
                state.sidebar_category_order = std::move(order);
                persist_order();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sort categories A–Z. Persists across\n"
                                  "sessions via sidebar_order.txt.");
            }
            // Count safety categories for the menu label — also gates
            // the disable: a pack with no safety tables or only safety
            // tables would have a no-op gesture, so hide the action.
            std::size_t safety_count = 0;
            for (auto const &c : cats) {
                if (c.any_safety) {
                    ++safety_count;
                }
            }
            char buf[64];
            std::snprintf(buf, sizeof buf, "Group by safety (%zu first)",
                          safety_count);
            ImGui::BeginDisabled(safety_count == 0 ||
                                 safety_count == cats.size());
            if (ImGui::MenuItem(buf)) {
                std::vector<std::string> order;
                order.reserve(cats.size());
                for (auto const &c : cats) {
                    if (c.any_safety) {
                        order.emplace_back(c.name);
                    }
                }
                for (auto const &c : cats) {
                    if (!c.any_safety) {
                        order.emplace_back(c.name);
                    }
                }
                state.sidebar_category_order = std::move(order);
                persist_order();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Move safety-critical categories to the top of the\n"
                    "sidebar. A category counts as safety-critical if any\n"
                    "of its tables carries the flag. Disabled when none\n"
                    "or all categories are flagged (no-op).");
            }
            ImGui::EndMenu();
        }
        // Hidden-category submenu. Per-header "Hide" is the way to
        // suppress a category; this panel-level menu is where the user
        // surfaces what's hidden + brings it back. Disabled when
        // nothing is hidden so the entry stays visible-as-affordance.
        ImGui::Separator();
        bool const has_hidden = !state.sidebar_hidden_categories.empty();
        ImGui::BeginDisabled(!has_hidden);
        if (ImGui::BeginMenu("Show hidden category")) {
            for (auto const &cat : state.sidebar_hidden_categories) {
                if (ImGui::MenuItem(cat.c_str())) {
                    requested_unhide = cat;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Show all categories")) {
            requested_unhide_all = true;
        }
        ImGui::EndDisabled();
        if (!has_hidden) {
            text_subtle("(no hidden categories)");
        }
        ImGui::EndPopup();
    }
    // Apply hidden-set mutations from the panel menu before the
    // render loop reads sidebar_hidden_categories.
    if (requested_unhide.has_value()) {
        auto &h = state.sidebar_hidden_categories;
        h.erase(std::remove(h.begin(), h.end(), *requested_unhide), h.end());
        save_sidebar_hidden_categories(state.project->dir(), h);
    }
    if (requested_unhide_all) {
        state.sidebar_hidden_categories.clear();
        save_sidebar_hidden_categories(state.project->dir(),
                                       state.sidebar_hidden_categories);
    }

    auto const &def = state.project->definition();

    // Filter input. Ctrl+F (handled in main loop) hands keyboard focus
    // here for the next frame; Esc clears the buffer and unfocuses by
    // virtue of EscapeClearsAll. Width matches the full panel so the
    // affordance is unambiguous.
    if (state.focus_table_filter) {
        ImGui::SetKeyboardFocusHere();
        state.focus_table_filter = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##table_filter", "Filter tables…  (Ctrl+F)", state.table_filter,
                             sizeof state.table_filter, ImGuiInputTextFlags_EscapeClearsAll);

    // Right-aligned "Collapse all" affordance. One-shot — flips a flag
    // the render loop consumes for one frame to force every TreeNode
    // closed; persisted imgui.ini state takes over again next frame as
    // soon as the user opens anything. Link-styled so it reads as a
    // utility action, not a primary CTA — same vocabulary as the
    // welcome footer's "Keyboard shortcuts" / "Command palette" links.
    {
        char const *const label = "Collapse all";
        float const label_w = ImGui::CalcTextSize(label).x;
        float const right_x =
            ImGui::GetWindowContentRegionMax().x - label_w -
            ImGui::GetStyle().FramePadding.x * 2.0f;
        if (right_x > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(right_x);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
        if (ImGui::SmallButton(label)) {
            state.sidebar_collapse_all_request = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Close every group in the tree.\n"
                              "Your previously-opened groups stay in imgui.ini\n"
                              "and reopen on click — this is a one-shot reset.");
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    // Quick-jump shortcuts: E → first emissions-relevant table,
    // S → first engine-safety-critical table. Only fires when the
    // sidebar window is focused AND no text-input is active. Useful
    // for jumping straight to the most-common flagged categories
    // without typing or scrolling.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            for (auto const &t : def.tables()) {
                if (t.emissions_relevant) {
                    state.select_table(t.id);
                    break;
                }
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            for (auto const &t : def.tables()) {
                if (t.engine_safety_critical) {
                    state.select_table(t.id);
                    break;
                }
            }
        }
    }

    std::string_view const filter{state.table_filter};
    // Count matches once so the active-filter header line can report
    // "N of M". When the filter is empty, the count is noise — the tree
    // below already conveys "how many tables" at a glance.
    std::size_t matched = 0;
    for (auto const &t : def.tables()) {
        if (filter.empty() || icontains(t.name, filter) || icontains(t.id, filter)) {
            ++matched;
        }
    }
    if (!filter.empty()) {
        text_subtle("%zu of %zu tables", matched, def.tables().size());
        ImGui::Separator();
    }

    // Persistent "N hidden — show all" hint near the filter. Without
    // this, a user who hid a category right-click → Hide weeks ago
    // wouldn't have a top-of-panel hint that some categories are
    // missing on purpose — they'd have to scroll to the footer
    // restore chips or right-click the panel again.
    // Counted against pack categories, not the hidden_categories
    // vector itself, so stale entries (categories that no longer
    // exist in this pack) don't inflate the count.
    {
        std::size_t hidden_present = 0;
        for (auto const &h : state.sidebar_hidden_categories) {
            for (auto const &t : def.tables()) {
                std::string_view const tcat =
                    t.category.empty() ? std::string_view{"Other"}
                                       : std::string_view{t.category};
                if (tcat == std::string_view{h}) {
                    ++hidden_present;
                    break;
                }
            }
        }
        if (hidden_present > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
            char hint[64];
            std::snprintf(hint, sizeof hint,
                          "\xE2\x9A\xA0 %zu hidden \xC2\xB7 show all",
                          hidden_present);
            // Render as a small link-style button so a click clears
            // every hidden category in one gesture, mirroring the
            // panel-level "Show all categories" menu item.
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
            if (ImGui::SmallButton(hint)) {
                state.sidebar_hidden_categories.clear();
                save_sidebar_hidden_categories(state.project->dir(),
                                               state.sidebar_hidden_categories);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Restore every hidden category.\n"
                                  "Right-click a category header to hide it again.");
            }
            ImGui::PopStyleColor(4);
            ImGui::Separator();
        }
    }

    if (!filter.empty() && matched == 0) {
        char title[80];
        std::snprintf(title, sizeof title, "No tables match \"%s\"", state.table_filter);
        render_empty_state(
            title,
            "Try a shorter prefix, or press Esc to clear the filter.");
        ImGui::End();
        return;
    }

    // Group tables by category, preserving first-occurrence order so
    // pack authors get to control the group ordering. Tables without a
    // category fall into "Other"; that group lands wherever the first
    // uncategorized table appears (predictable, not magic-sorted).
    struct Group {
        std::string_view name;
        std::vector<std::size_t> indices; // into def.tables()
    };
    std::vector<Group> groups;
    groups.reserve(8);
    auto const find_or_make = [&](std::string_view cat) -> Group & {
        for (auto &g : groups) {
            if (g.name == cat)
                return g;
        }
        groups.push_back({cat, {}});
        return groups.back();
    };
    for (std::size_t i = 0; i < def.tables().size(); ++i) {
        auto const &t = def.tables()[i];
        std::string_view const cat =
            t.category.empty() ? std::string_view{"Other"} : std::string_view{t.category};
        find_or_make(cat).indices.push_back(i);
    }

    // Apply the persisted user ordering on top of the first-occurrence
    // order. Categories that appear in sidebar_category_order get sorted
    // to the front in that sequence; the rest follow in their pack
    // discovery order. Stable for groups whose category name isn't in
    // the ordering vector — they keep their relative position.
    if (!state.sidebar_category_order.empty()) {
        std::vector<Group> reordered;
        reordered.reserve(groups.size());
        // First pass: emit groups in the user's preferred order.
        for (auto const &want : state.sidebar_category_order) {
            for (std::size_t gi = 0; gi < groups.size(); ++gi) {
                if (std::string_view{want} == groups[gi].name) {
                    reordered.push_back(std::move(groups[gi]));
                    groups[gi].name = {}; // mark consumed
                    break;
                }
            }
        }
        // Second pass: any group whose name still has content (i.e.
        // wasn't in sidebar_category_order) lands in its discovery
        // position relative to the leftover set.
        for (auto &g : groups) {
            if (!g.name.empty()) {
                reordered.push_back(std::move(g));
            }
        }
        groups = std::move(reordered);
    }

    auto const table_matches = [&](st::Table const &t) {
        return filter.empty() || icontains(t.name, filter) || icontains(t.id, filter);
    };

    auto const render_table_row = [&](st::Table const &t) {
        bool const selected = state.selected_table_id == t.id;
        // Prefer the human-readable name as the primary label.
        // Snake-case IDs are developer-facing — surface them in the
        // tooltip instead. No leading icon — the tree's left margin
        // already conveys "this is a child row," and stamping a grid
        // glyph on every one of 293 rows reads as visual noise rather
        // than affordance.
        char const *label = t.name.empty() ? t.id.c_str() : t.name.c_str();
        ImGui::PushID(t.id.c_str());
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowOverlap)) {
            state.select_table(t.id);
        }
        // Capture Selectable hover state BEFORE drawing the badge — the
        // badge becomes the "last item" once it lands, which would
        // otherwise scope the tooltip to just the tiny S/E letters
        // instead of the whole row.
        bool const row_hovered = ImGui::IsItemHovered();
        // Right-aligned policy badges: S (engine-safety-critical, warm
        // amber) and E (emissions-relevant, muted yellow). Drawn AFTER
        // the Selectable so AllowOverlap is needed; reads "tagged" at a
        // glance while browsing.
        if (t.engine_safety_critical || t.emissions_relevant) {
            char buf[4]{};
            std::size_t bi = 0;
            if (t.engine_safety_critical)
                buf[bi++] = 'S';
            if (t.emissions_relevant)
                buf[bi++] = 'E';
            float const w = ImGui::CalcTextSize(buf).x;
            float const right_x =
                ImGui::GetWindowContentRegionMax().x - w - ImGui::GetStyle().FramePadding.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_x);
            // Theme-aware via the same fg colors the chip palette uses:
            // S (engine-safety) shares chip_fg_warn (amber band), E
            // (emissions) shares chip_fg_caution (yellow band). In light
            // theme these flip to dark amber / dark olive so they stay
            // legible against the pale-blue panel background.
            ImVec4 const color =
                t.engine_safety_critical ? chip_fg_warn() : chip_fg_caution();
            ImGui::TextColored(color, "%s", buf);
        }
        if (row_hovered) {
            ImGui::BeginTooltip();
            if (!t.name.empty()) {
                ImGui::TextUnformatted(t.name.c_str());
                text_subtle("%s", t.id.c_str());
            } else {
                ImGui::TextUnformatted(t.id.c_str());
            }
            ImGui::Separator();
            ImGui::Text("%dD  \xC2\xB7  0x%08zX", t.dimensions, t.address);
            if (!t.category.empty()) {
                text_subtle("category: %s", t.category.c_str());
            }
            if (t.engine_safety_critical) {
                ImGui::TextColored(chip_fg_warn(), "engine safety critical");
            } else if (t.emissions_relevant) {
                ImGui::TextColored(chip_fg_caution(), "emissions-relevant");
            }
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    };

    // Drag-and-drop state — captured here so the loop body can post a
    // requested reorder without mutating `groups` mid-iteration. Indices
    // are positions in the (already-reordered) `groups` vector.
    int drop_src_index = -1;
    int drop_dst_index = -1;
    // Per-header context-menu state. Captured + applied post-loop for
    // the same reason as drag-drop — mutating sidebar_hidden_categories
    // while iterating groups would shift indices mid-frame.
    std::optional<std::string> requested_hide;
    auto const is_hidden = [&](std::string_view cat) {
        for (auto const &h : state.sidebar_hidden_categories) {
            if (std::string_view{h} == cat)
                return true;
        }
        return false;
    };
    // Two-level hierarchy: pack categories like "fuel - injectors - pulse"
    // split on the first " - " into a top-level group name ("fuel") and
    // a sub-group label ("injectors - pulse"). Without this the user
    // saw 91 flat folder headers at once — busy and intimidating against
    // the project's design philosophy. With it: ~8-10 top-level groups,
    // collapsed-by-default; opening one reveals its sub-categories as
    // the existing leaf groups (which keep their persisted open/close
    // state via imgui.ini's stable ###cat_<full-name> IDs). Tables that
    // have no " - " separator land directly at top-level with no nested
    // sub-tree. Empty categories fall into "Other".
    auto const split_top = [](std::string_view full)
        -> std::pair<std::string_view, std::string_view> {
        auto const pos = full.find(" - ");
        if (pos == std::string_view::npos) {
            return {full, {}};
        }
        return {full.substr(0, pos), full.substr(pos + 3)};
    };

    struct TopGroup {
        std::string_view name;            // "fuel", "engine", etc.
        std::vector<std::size_t> sub_idx; // indices into `groups`
        std::size_t total_tables{0};
    };
    std::vector<TopGroup> tops;
    tops.reserve(8);
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        auto const top = split_top(groups[gi].name).first;
        auto it = std::find_if(tops.begin(), tops.end(),
                               [top](TopGroup const &t) { return t.name == top; });
        if (it == tops.end()) {
            tops.push_back({top, {gi}, groups[gi].indices.size()});
        } else {
            it->sub_idx.push_back(gi);
            it->total_tables += groups[gi].indices.size();
        }
    }
    // Top-level sort: alphabetical. Pack-author order at the leaf level
    // still matters (and the persisted reorder pass above already
    // honored it), but at the top level alphabetical is the predictable
    // expectation — "Fuel" always comes after "Engine" regardless of
    // which categories appear first in the pack file.
    std::sort(tops.begin(), tops.end(),
              [](TopGroup const &a, TopGroup const &b) { return a.name < b.name; });

    std::size_t hidden_visible_in_pack = 0; // how many hidden cats actually exist in this pack

    // Renders one leaf-level group (the existing `Group`) as a TreeNode
    // with all its context-menu / drag-drop / glossary affordances. Used
    // both inline-at-top-level (when a top group has no sub-divisions)
    // and nested inside a top-level TreeNode.
    auto const render_leaf_group = [&](std::size_t gi, std::string_view header_label,
                                       std::size_t group_matched) -> void {
        auto const &g = groups[gi];
        char tn_label[200];
        if (filter.empty()) {
            std::snprintf(tn_label, sizeof tn_label, "%.*s (%zu)###cat_%.*s",
                          static_cast<int>(header_label.size()), header_label.data(),
                          g.indices.size(),
                          static_cast<int>(g.name.size()), g.name.data());
        } else {
            std::snprintf(tn_label, sizeof tn_label, "%.*s (%zu of %zu)###cat_%.*s",
                          static_cast<int>(header_label.size()), header_label.data(),
                          group_matched, g.indices.size(),
                          static_cast<int>(g.name.size()), g.name.data());
        }
        if (!filter.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else if (state.sidebar_collapse_all_request) {
            // One-shot "Collapse all" overrides imgui.ini.
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        }
        // Leaf-level default-CLOSED to match the top-level default.
        // Opening a top group should reveal what's INSIDE it (the
        // sub-category labels) — not auto-expand every leaf and pour
        // out every row, which puts the user back where they started.
        // Per-session open/close still persists via imgui.ini's stable
        // ###cat_<full-name> ID; once the user opens a leaf it stays
        // open across sessions.
        bool const opened =
            ImGui::TreeNodeEx(tn_label, ImGuiTreeNodeFlags_SpanAvailWidth);
        if (ImGui::BeginPopupContextItem()) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "Hide \"%.*s\"",
                          static_cast<int>(g.name.size()), g.name.data());
            if (ImGui::MenuItem(buf)) {
                requested_hide = std::string{g.name};
            }
            text_subtle("Reopen from the panel right-click menu");
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            int const src_idx = static_cast<int>(gi);
            ImGui::SetDragDropPayload("SUBUWU_SIDEBAR_CATEGORY", &src_idx, sizeof src_idx);
            ImGui::TextUnformatted(tn_label);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (auto const *payload =
                    ImGui::AcceptDragDropPayload("SUBUWU_SIDEBAR_CATEGORY")) {
                int const src_idx = *static_cast<int const *>(payload->Data);
                drop_src_index = src_idx;
                drop_dst_index = static_cast<int>(gi);
            }
            ImGui::EndDragDropTarget();
        }
        // Glossary hover on the leaf header — many category strings
        // match glossary entries (Table, Datalog, DTC, Scaling).
        glossary_tooltip_for(state, g.name);
        if (opened) {
            for (auto idx : g.indices) {
                auto const &t = def.tables()[idx];
                if (!table_matches(t))
                    continue;
                render_table_row(t);
            }
            ImGui::TreePop();
        }
    };

    for (auto const &tg : tops) {
        // Compute match counts + visible sub-group count for this top.
        // Drops the top-level entirely when nothing inside it matches
        // the active filter, OR when every sub-group is hidden.
        std::size_t top_matched = 0;
        std::size_t visible_subs = 0;
        for (auto gi : tg.sub_idx) {
            auto const &g = groups[gi];
            if (is_hidden(g.name)) {
                ++hidden_visible_in_pack;
                continue;
            }
            std::size_t sub_matched = 0;
            for (auto idx : g.indices) {
                if (table_matches(def.tables()[idx])) {
                    ++sub_matched;
                }
            }
            if (sub_matched > 0) {
                top_matched += sub_matched;
                ++visible_subs;
            }
        }
        if (visible_subs == 0) {
            continue;
        }

        // Single-leaf top-level groups whose sole leaf has no sub-name
        // (e.g. "engine", which is both a top-level and the only category
        // for its rows) get rendered as their own TreeNode at top level —
        // no redundant nesting. The leaf's full category IS the top name
        // in this case.
        bool const inline_single_leaf =
            tg.sub_idx.size() == 1 &&
            split_top(groups[tg.sub_idx[0]].name).second.empty();
        if (inline_single_leaf) {
            render_leaf_group(tg.sub_idx[0], tg.name, top_matched);
            continue;
        }

        // Top-level TreeNode. Default-CLOSED — this is the change that
        // collapses a 91-folder wall down to ~9 visible group headers
        // on first open. Stable ID via ###top_<name> so future sessions
        // remember which groups the user has opened.
        char top_label[160];
        if (filter.empty()) {
            std::snprintf(top_label, sizeof top_label, "%.*s (%zu)###top_%.*s",
                          static_cast<int>(tg.name.size()), tg.name.data(),
                          tg.total_tables,
                          static_cast<int>(tg.name.size()), tg.name.data());
        } else {
            std::snprintf(top_label, sizeof top_label, "%.*s (%zu of %zu)###top_%.*s",
                          static_cast<int>(tg.name.size()), tg.name.data(),
                          top_matched, tg.total_tables,
                          static_cast<int>(tg.name.size()), tg.name.data());
        }
        if (!filter.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        } else if (state.sidebar_collapse_all_request) {
            // One-shot "Collapse all" — closes top-level too. Filter
            // takes precedence so an active filter is never broken by
            // a stale collapse request.
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        }
        bool const top_open = ImGui::TreeNodeEx(top_label,
                                                ImGuiTreeNodeFlags_SpanAvailWidth);
        // Glossary hover on the top-level too — "fuel" / "ignition" /
        // "avcs" are all glossary entries the user might want to peek
        // before drilling in.
        glossary_tooltip_for(state, tg.name);
        if (!top_open) {
            continue;
        }
        // Render visible sub-groups nested inside, dropping the redundant
        // "<top> - " prefix from each leaf's header label.
        for (auto gi : tg.sub_idx) {
            auto const &g = groups[gi];
            if (is_hidden(g.name)) {
                continue;
            }
            std::size_t sub_matched = 0;
            for (auto idx : g.indices) {
                if (table_matches(def.tables()[idx])) {
                    ++sub_matched;
                }
            }
            if (sub_matched == 0) {
                continue;
            }
            auto const sub_name = split_top(g.name).second;
            render_leaf_group(gi, sub_name.empty() ? g.name : sub_name, sub_matched);
        }
        ImGui::TreePop();
    }
    // Clear the one-shot "Collapse all" flag now that every TreeNode
    // in this frame has had its chance to consume the SetNextItemOpen
    // override. Leaving it set would force-close the tree every frame.
    state.sidebar_collapse_all_request = false;
    // Apply the drag-drop reorder once the loop finishes. Mutating
    // `groups` mid-frame is fine (it's a local), but the persistence
    // side mutates AppState + writes disk — defer to post-loop so the
    // tree's rendering completes consistently.
    if (drop_src_index >= 0 && drop_dst_index >= 0 &&
        drop_src_index != drop_dst_index &&
        drop_src_index < static_cast<int>(groups.size()) &&
        drop_dst_index < static_cast<int>(groups.size()) &&
        state.project.has_value()) {
        // Rebuild the canonical ordering vector from the post-drop
        // groups list. Capturing the full order (not just the moved
        // category) means subsequent loads + fallbacks work uniformly
        // for any later pack that adds new categories.
        Group moved = std::move(groups[static_cast<std::size_t>(drop_src_index)]);
        groups.erase(groups.begin() + drop_src_index);
        int insert_at = drop_dst_index;
        if (drop_src_index < drop_dst_index) {
            // Erasing shifted target left by one; preserve the user's
            // intent ("drop before category X") under that shift.
            insert_at = drop_dst_index - 1;
        }
        groups.insert(groups.begin() + insert_at, std::move(moved));
        state.sidebar_category_order.clear();
        state.sidebar_category_order.reserve(groups.size());
        for (auto const &g2 : groups) {
            state.sidebar_category_order.emplace_back(g2.name);
        }
        save_sidebar_category_order(state.project->dir(),
                                    state.sidebar_category_order);
    }
    // Apply per-header Hide selection now that the tree is drawn —
    // the next frame walks the updated hidden set and suppresses the
    // category cleanly. Same defer-to-post-loop pattern as drag-drop.
    if (requested_hide.has_value()) {
        auto &h = state.sidebar_hidden_categories;
        if (std::find(h.begin(), h.end(), *requested_hide) == h.end()) {
            h.push_back(*requested_hide);
        }
        save_sidebar_hidden_categories(state.project->dir(), h);
    }
    // Footer hint when categories are hidden — without it, a user
    // who returned to a project they configured weeks ago wouldn't
    // know why some categories appear missing. Right-click the
    // panel to unhide.
    if (hidden_visible_in_pack > 0) {
        ImGui::Separator();
        text_subtle("%zu categor%s hidden",
                    hidden_visible_in_pack,
                    hidden_visible_in_pack == 1 ? "y" : "ies");
        // Per-category restore chips — finer-grained than the panel
        // right-click submenu and faster than "Show all then re-hide
        // some". Click a chip to unhide just that category. Captured
        // for post-render mutation so we don't shuffle the vector
        // while iterating.
        std::optional<std::string> footer_unhide;
        for (std::size_t i = 0; i < state.sidebar_hidden_categories.size(); ++i) {
            auto const &cat = state.sidebar_hidden_categories[i];
            // Skip categories that no longer exist in the loaded pack
            // (the panel-level "Show hidden category" submenu has the
            // same hide-stale-entry behavior — restoring a non-pack
            // category is a no-op anyway). Footer chips mirror that.
            bool present_in_pack = false;
            for (auto const &t : def.tables()) {
                std::string_view const tcat =
                    t.category.empty() ? std::string_view{"Other"}
                                        : std::string_view{t.category};
                if (tcat == cat) {
                    present_in_pack = true;
                    break;
                }
            }
            if (!present_in_pack) {
                continue;
            }
            ImGui::PushID(static_cast<int>(i));
            char label[160];
            std::snprintf(label, sizeof label, "\xE2\xA4\xB5 %s", cat.c_str());
            if (ImGui::SmallButton(label)) {
                footer_unhide = cat;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Restore '%s' to the sidebar.", cat.c_str());
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        // Newline to absorb the trailing SameLine from the last chip.
        ImGui::NewLine();
        if (footer_unhide.has_value() && state.project.has_value()) {
            auto &h = state.sidebar_hidden_categories;
            h.erase(std::remove(h.begin(), h.end(), *footer_unhide), h.end());
            save_sidebar_hidden_categories(state.project->dir(), h);
        }
    }
    ImGui::End();
}

} // namespace st::ui
