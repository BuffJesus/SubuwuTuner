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

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace st::ui {

void render_sidebar(AppState &state) {
    if (!state.show_tables_panel) {
        return;
    }
    ImGui::Begin("Tables");

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

    // Right-click anywhere in the panel for ordering actions. Single
    // entry today (reset) — disabled when there's no custom order to
    // reset, so the affordance is always discoverable even on a fresh
    // project. Filename ID prevents collision with per-row menus
    // pushed deeper in the tree.
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
        ImGui::EndPopup();
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
        // tooltip instead.
        char const *label = t.name.empty() ? t.id.c_str() : t.name.c_str();
        // MDL2 E80A GridView — small leading icon so every table row
        // reads as a data-grid affordance even when scanning quickly.
        char row_label[256];
        std::snprintf(row_label, sizeof row_label, "\xEE\xA0\x8A  %s", label);
        ImGui::PushID(t.id.c_str());
        if (ImGui::Selectable(row_label, selected, ImGuiSelectableFlags_AllowOverlap)) {
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
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        auto const &g = groups[gi];
        // Count matches in this group up-front so the header line can
        // report it AND so we can skip an entirely-filtered-out group
        // (don't render an empty TreeNode that just clutters the panel).
        std::size_t group_matched = 0;
        for (auto idx : g.indices) {
            if (table_matches(def.tables()[idx])) {
                ++group_matched;
            }
        }
        if (group_matched == 0)
            continue;

        // The visible label embeds the table count. The ID hash uses
        // the category name only (via ImGui's `###` separator: text
        // before is shown, text after is the stable ID hash). Without
        // this split the ID would change every time the count
        // changes — applying a filter, loading a different pack —
        // and ImGui::TreeNode would lose the user's collapse state
        // because the new ID has no stored entry. With `###cat_<name>`
        // the ID stays stable across filter / pack / count changes,
        // so the collapse state persists for as long as imgui.ini is
        // writable (across sessions).
        // MDL2 E8B7 Folder — leads the category header so the group
        // hierarchy reads at a glance. The ### suffix keeps the ImGui
        // ID stable on the category name only, so the leading icon
        // doesn't churn the collapse state when the count changes.
        char tn_label[200];
        if (filter.empty()) {
            std::snprintf(tn_label, sizeof tn_label, "\xEE\xA2\xB7  %.*s (%zu)###cat_%.*s",
                          static_cast<int>(g.name.size()), g.name.data(),
                          g.indices.size(),
                          static_cast<int>(g.name.size()), g.name.data());
        } else {
            std::snprintf(tn_label, sizeof tn_label,
                          "\xEE\xA2\xB7  %.*s (%zu of %zu)###cat_%.*s",
                          static_cast<int>(g.name.size()), g.name.data(),
                          group_matched, g.indices.size(),
                          static_cast<int>(g.name.size()), g.name.data());
        }

        // When filtering, force the group open so matches are always
        // visible. Otherwise default-open on first run; the user can
        // collapse manually and the state persists via imgui.ini
        // (across sessions, thanks to the stable `###cat_<name>` ID
        // suffix above).
        if (!filter.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        ImGuiTreeNodeFlags const tn_flags =
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        bool const opened = ImGui::TreeNodeEx(tn_label, tn_flags);
        // Drag the header to reorder; drop another header on it to
        // place the dragged group before this one. Payload is the
        // source's index into `groups` so the post-loop apply can
        // rotate the vector + persist the new ordering.
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
        // Glossary hover on the category header — many category strings
        // (Table, Datalog, DTC, Flash, Scaling) match glossary entries
        // and the user has the header right there to hover.
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
    }
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
    ImGui::End();
}

} // namespace st::ui
