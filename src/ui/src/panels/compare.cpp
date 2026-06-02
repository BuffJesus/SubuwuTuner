// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Compare panel — GUI for the structured ROM diff workflow shipped
// in st::diff (and the CLI `diff` subcommand). Two-pane synchronized
// tree per analyst Issue #4 / docs/33:
//
//   - Toolbar: ROM A + ROM B file pickers, epsilon slider, include-
//     identical toggle, Compare button. ROM A defaults to the open
//     project's source.bin when the panel first opens against a
//     loaded project.
//   - Summary line: pack id, tables compared/changed, cells changed.
//   - Changed-tables list: each row shows [S][E] safety/emissions
//     chips + table name + cells_changed/total + max/mean |Δ|.
//     Click a row to expand the per-cell change list. Click "Open in
//     editor" to jump to the table via state.select_table().
//   - Skipped-tables list at the bottom (errors per-table).
//
// The DiffSet is recomputed on demand (Compare button); cached on
// state.compare_result for re-render without recompute. Same epsilon
// + include-identical knobs as the CLI; uses st::diff::compare under
// the hood so the panel and CLI share one source of truth.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/diff.hpp"
#include "st/rom.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace st::ui {

namespace {

// Sentinel string for ROM A — when the path equals this, the compare
// uses the open project's in-memory source ROM rather than reading
// from disk. Saves the user from typing the project's source.bin
// path; works even when the project's source file has been renamed
// or moved relative to the .stune dir.
inline constexpr char const *kProjectSourceSentinel = "<project source>";

// Run the compare; populate state.compare_result on success or
// compare_error_msg on failure. ROM A uses the project source when
// compare_rom_a_path == kProjectSourceSentinel; otherwise both ROMs
// come from disk.
void recompute_compare(AppState &state) {
    state.compare_error_msg.clear();
    state.compare_result.reset();

    if (!state.project.has_value()) {
        state.compare_error_msg = "No project open — Compare needs a loaded "
                                  "definition pack to drive the table diff.";
        return;
    }
    if (state.compare_rom_b_path[0] == '\0') {
        state.compare_error_msg = "Pick ROM B before running Compare.";
        return;
    }

    bool const use_project_source =
        (std::string{state.compare_rom_a_path} == kProjectSourceSentinel) ||
        (state.compare_rom_a_path[0] == '\0');

    std::optional<st::Rom> rom_a_loaded;
    if (!use_project_source) {
        auto rom_a = st::Rom::from_file(state.compare_rom_a_path);
        if (!rom_a.has_value()) {
            state.compare_error_msg = std::string{"ROM A: "} + rom_a.error().to_string();
            return;
        }
        rom_a_loaded = std::move(*rom_a);
    }
    Rom const &rom_a_ref = use_project_source ? state.project->source_rom() : *rom_a_loaded;

    auto rom_b = st::Rom::from_file(state.compare_rom_b_path);
    if (!rom_b.has_value()) {
        state.compare_error_msg = std::string{"ROM B: "} + rom_b.error().to_string();
        return;
    }

    st::diff::Options opts;
    opts.cell_epsilon = static_cast<double>(state.compare_epsilon);
    opts.include_identical = state.compare_include_identical;
    auto result = st::diff::compare(rom_a_ref, *rom_b, state.project->definition(), opts);
    if (!result.has_value()) {
        state.compare_error_msg = result.error().to_string();
        return;
    }
    state.compare_result = std::move(*result);
}

// Open a "pick ROM" dialog and copy the chosen path into `out`. NFD's
// filter list takes a name + comma-separated extension list.
void pick_rom_into(char *out, std::size_t out_size, std::string &error_msg) {
    nfdu8filteritem_t filters[2] = {{"ROM image", "bin"}, {"Hex / SREC", "hex,srec,mot"}};
    NFD::UniquePathU8 picked;
    nfdresult_t const r = NFD::OpenDialog(picked, filters, 2);
    if (r == NFD_OKAY) {
        std::snprintf(out, out_size, "%s", picked.get());
        error_msg.clear();
    } else if (r == NFD_ERROR) {
        error_msg = std::string{"Open dialog error: "} + NFD::GetError();
    }
}

// Color the headline value of cells_changed by intensity: green if
// zero changes, amber otherwise (engine-safety tables always promoted
// to danger color regardless).
ImVec4 changed_count_color(std::size_t changed, bool is_safety) {
    if (is_safety && changed > 0)
        return chip_fg_danger();
    if (changed == 0)
        return chip_fg_ok();
    return chip_fg_caution();
}

// Render one table row in the changed-tables tree. Returns true when
// the user clicked the "Open in editor" action.
[[nodiscard]] bool render_table_row(AppState &state, st::diff::TableDelta const &t) {
    ImGui::PushID(t.table_id.c_str());

    bool open_in_editor = false;

    // Safety / emissions chips — small badges in front of the name.
    if (t.engine_safety_critical) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("[S]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (t.emissions_relevant) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
        ImGui::TextUnformatted("[E]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    // Expandable tree node for the table.
    bool const was_expanded =
        state.compare_expanded_tables.find(t.table_id) != state.compare_expanded_tables.end();
    ImGui::SetNextItemOpen(was_expanded, ImGuiCond_Always);
    bool const is_open = ImGui::TreeNode(t.table_id.c_str(), "%s  (%s)", t.table_name.c_str(),
                                         t.table_id.c_str());
    bool const now_expanded = is_open;
    if (now_expanded && !was_expanded) {
        state.compare_expanded_tables.insert(t.table_id);
    } else if (!now_expanded && was_expanded) {
        state.compare_expanded_tables.erase(t.table_id);
    }

    // Cells-changed badge — colored by zone.
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,
                         changed_count_color(t.cells_changed, t.engine_safety_critical));
    ImGui::Text("    %zu/%zu cells   max |Δ|=%.3g   mean |Δ|=%.3g", t.cells_changed,
                t.total_cells, t.max_abs_delta, t.mean_abs_delta);
    ImGui::PopStyleColor();

    if (is_open) {
        // Open-in-editor action — jumps to the table in the side panel
        // + central editor.
        if (ImGui::Button("\xEE\x9C\xA9  Open in editor")) { // E709 Edit
            open_in_editor = true;
        }
        ImGui::SameLine();
        text_subtle("(click to load this table in the Table viewer)");

        // Per-cell change list. Cap visible rows for huge tables —
        // ImPlot tables can render thousands but the user rarely
        // scans more than 50; offer a "show all N" toggle later.
        if (!t.changes.empty()) {
            constexpr std::size_t kMaxCellRowsDefault = 50;
            auto const total = t.changes.size();
            auto const show = std::min<std::size_t>(total, kMaxCellRowsDefault);

            if (ImGui::BeginTable("##cell_changes", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 180.0f))) {
                ImGui::TableSetupColumn("row");
                ImGui::TableSetupColumn("col");
                ImGui::TableSetupColumn("A");
                ImGui::TableSetupColumn("B");
                ImGui::TableSetupColumn("Δ");
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < show; ++i) {
                    auto const &c = t.changes[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", c.row);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", c.col);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.4g", c.value_a);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.4g", c.value_b);
                    ImGui::TableNextColumn();
                    // Color delta by sign for at-a-glance scanning.
                    double const d = c.delta();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          d > 0 ? chip_fg_caution() : chip_fg_ok());
                    ImGui::Text("%+.4g", d);
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            if (show < total) {
                text_subtle("Showing first %zu of %zu changes — open the JSON / CSV "
                            "export via `subuwutuner-cli diff --format` for the full "
                            "list.",
                            show, total);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
    return open_in_editor;
}

} // namespace

void render_compare_panel(AppState &state) {
    if (!state.show_compare_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Compare###Compare (Preview)", &state.show_compare_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Structured ROM diff via st::diff. Picks per-table cell-level "
                "changes against the open project's pack. Same engine as the "
                "`subuwutuner-cli diff` subcommand. See docs/33 + analyst Issue #4.");
    ImGui::Separator();

    // ROM A — default to the in-memory project source on first open.
    // The sentinel string keeps the input field readable and signals
    // to recompute_compare() to skip the file read.
    if (state.compare_rom_a_path[0] == '\0' && state.project.has_value()) {
        std::snprintf(state.compare_rom_a_path, sizeof state.compare_rom_a_path, "%s",
                      kProjectSourceSentinel);
    }

    ImGui::Text("ROM A (baseline):");
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("##cmp_rom_a", state.compare_rom_a_path,
                     sizeof state.compare_rom_a_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cmp_a")) {
        pick_rom_into(state.compare_rom_a_path, sizeof state.compare_rom_a_path,
                      state.compare_error_msg);
    }

    ImGui::Text("ROM B (compare):");
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("##cmp_rom_b", state.compare_rom_b_path,
                     sizeof state.compare_rom_b_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cmp_b")) {
        pick_rom_into(state.compare_rom_b_path, sizeof state.compare_rom_b_path,
                      state.compare_error_msg);
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Cell epsilon", &state.compare_epsilon, 0.0f, 10.0f, "%.3f");
    ImGui::SameLine();
    ImGui::Checkbox("Show identical tables", &state.compare_include_identical);
    ImGui::SameLine();
    // Compare button — pinned to the right for thumb reach.
    if (ImGui::Button("\xEE\x9C\xA0  Compare", ImVec2(120.0f, 0.0f))) { // E700 GlobalNav
        recompute_compare(state);
    }

    if (!state.compare_error_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.compare_error_msg.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (!state.compare_result.has_value()) {
        text_subtle("No comparison yet — pick two ROMs and click Compare.");
        ImGui::End();
        return;
    }

    auto const &d = *state.compare_result;

    // Summary line.
    ImGui::Text("Pack: %s    Tables: %zu compared / %zu changed    Cells: %zu / %zu changed",
                d.pack_id.c_str(), d.tables_compared, d.tables_changed,
                d.total_cells_changed, d.total_cells_compared);
    if (d.identical()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
        ImGui::TextUnformatted("Identical — every table compared equal.");
        ImGui::PopStyleColor();
    }

    // Changed-tables tree.
    if (!d.tables.empty()) {
        ImGui::Spacing();
        // Sort by cells_changed desc so the biggest changes float to
        // the top; ties broken by table_id for stability across renders.
        std::vector<st::diff::TableDelta const *> sorted;
        sorted.reserve(d.tables.size());
        for (auto const &t : d.tables) {
            sorted.push_back(&t);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](st::diff::TableDelta const *x, st::diff::TableDelta const *y) {
                      if (x->cells_changed != y->cells_changed)
                          return x->cells_changed > y->cells_changed;
                      return x->table_id < y->table_id;
                  });

        std::string open_id;
        for (auto const *t : sorted) {
            // include_identical=true surfaces unchanged tables too;
            // collapse them into a "(no changes)" line so the user can
            // still see the table was inspected.
            if (!t->changed() && !state.compare_include_identical) {
                continue;
            }
            if (render_table_row(state, *t)) {
                open_id = t->table_id;
            }
        }
        // Defer the select_table call until after the loop so we don't
        // mutate state mid-render (select_table may rebuild the side
        // panel).
        if (!open_id.empty()) {
            state.select_table(open_id);
        }
    }

    // Skipped tables — surface so the user knows what didn't compare.
    if (!d.skipped.empty()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Skipped tables (could not compare)")) {
            for (auto const &k : d.skipped) {
                ImGui::BulletText("%s — %s", k.table_id.c_str(), k.reason.c_str());
            }
        }
    }

    ImGui::End();
}

} // namespace st::ui
