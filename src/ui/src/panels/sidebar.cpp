// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tables sidebar — left panel listing the calibration tables grouped
// by category, with a Ctrl+F-bindable filter input and per-table S/E
// policy badges. Click a row to select it (drives render_table_view).

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
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

    auto const table_matches = [&](st::Table const &t) {
        return filter.empty() || icontains(t.name, filter) || icontains(t.id, filter);
    };

    auto const render_table_row = [&](st::Table const &t) {
        bool const selected = state.selected_table_id == t.id;
        // Prefer the human-readable name as the primary label.
        // Snake-case IDs are developer-facing — surface them in the
        // tooltip instead.
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

    for (auto const &g : groups) {
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
        char tn_label[180];
        if (filter.empty()) {
            std::snprintf(tn_label, sizeof tn_label, "%.*s (%zu)###cat_%.*s",
                          static_cast<int>(g.name.size()), g.name.data(),
                          g.indices.size(),
                          static_cast<int>(g.name.size()), g.name.data());
        } else {
            std::snprintf(tn_label, sizeof tn_label, "%.*s (%zu of %zu)###cat_%.*s",
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
        if (ImGui::TreeNodeEx(tn_label, tn_flags)) {
            for (auto idx : g.indices) {
                auto const &t = def.tables()[idx];
                if (!table_matches(t))
                    continue;
                render_table_row(t);
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();
}

} // namespace st::ui
