// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Right-rail quick-stats panel for the currently-selected table.
// Min/max/mean/stddev across all cells, count of edited cells (diff
// against source), and an ImPlot histogram. Read-only; updates each
// frame the table data is loaded.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "panels/atlas_access.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace st::ui {

void render_stats_panel(AppState &state) {
    if (!state.show_stats_panel) {
        return;
    }
    // First-show fallback so toggling Stats on outside the Tune
    // workspace (whose build_workspace_layout docks it) doesn't drop
    // a floating window at the default top-left.
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Stats", &state.show_stats_panel)) {
        ImGui::End();
        return;
    }
    track_help_context(state, AppState::HelpContext::Stats);
    if (!state.project.has_value()) {
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to see table statistics here.");
        ImGui::End();
        return;
    }
    if (state.selected_table_id.empty() || !state.current_table_data.has_value()) {
        render_empty_state(
            "Pick a table",
            "Select one in the Tables panel — min / mean / max and a value histogram show up here.");
        ImGui::End();
        return;
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        render_empty_state(
            "Table not found",
            "The selected table id isn't in the loaded pack. Pick another from the Tables panel.");
        ImGui::End();
        return;
    }
    auto const &td = *state.current_table_data;

    // Flatten cells for stats + histogram. Skip dim=0 scalar — stats of
    // one value are useless.
    std::vector<float> cells;
    cells.reserve(td.values.size() * (td.values.empty() ? 0 : td.values[0].size()));
    for (auto const &row : td.values) {
        for (auto v : row)
            cells.push_back(static_cast<float>(v));
    }
    if (cells.empty() || (cells.size() == 1 && table->dimensions == 0)) {
        text_subtle("Scalar table — stats N/A.");
        ImGui::Separator();
        if (!cells.empty()) {
            ImGui::Text("Value: %g", static_cast<double>(cells[0]));
        }
        ImGui::End();
        return;
    }

    float min = cells[0], max = cells[0];
    double sum = 0.0;
    for (auto v : cells) {
        if (v < min)
            min = v;
        if (v > max)
            max = v;
        sum += static_cast<double>(v);
    }
    double const mean = sum / static_cast<double>(cells.size());
    double sq = 0.0;
    for (auto v : cells) {
        double const d = static_cast<double>(v) - mean;
        sq += d * d;
    }
    double const stddev =
        cells.size() > 1 ? std::sqrt(sq / static_cast<double>(cells.size() - 1)) : 0.0;

    // Count cells that differ from the source ROM. Cheap: re-read
    // source table data once per frame. For 1D / 2D tables of
    // moderate size this is well under a millisecond.
    //
    // When the user is viewing the working slot the comparison is
    // "edits to date". When viewing source it's always zero (td IS
    // source). When viewing an additional ROM it's "how heavily-
    // tuned is this calibration vs stock" — also useful.
    auto const count_differences =
        [&](st::Definition::TableData const &lhs,
            st::Definition::TableData const &rhs) -> std::size_t {
        std::size_t n = 0;
        if (lhs.values.size() != rhs.values.size())
            return n;
        for (std::size_t r = 0; r < lhs.values.size(); ++r) {
            if (lhs.values[r].size() != rhs.values[r].size())
                continue;
            for (std::size_t c = 0; c < lhs.values[r].size(); ++c) {
                if (lhs.values[r][c] != rhs.values[r][c])
                    ++n;
            }
        }
        return n;
    };
    std::size_t edited = 0;
    auto const source_td =
        state.project->definition().read_table_values(state.project->source_rom(), *table);
    if (source_td.has_value()) {
        edited = count_differences(td, *source_td);
    }
    // Secondary "vs working" delta when the user is viewing a non-
    // working slot (Issue #10). Skipped when view IS working (the
    // two would be identical) or source (also redundant — already
    // covered by the inverse of "edited" against working). Computed
    // lazily so the working-slot fast path doesn't pay for it.
    std::optional<std::size_t> delta_vs_working;
    if (!state.viewing_working_rom() && state.active_rom_id != "source") {
        auto const working_td = state.project->definition().read_table_values(
            state.project->working_rom(), *table);
        if (working_td.has_value()) {
            delta_vs_working = count_differences(td, *working_td);
        }
    }

    auto const *scal = state.project->definition().find_scaling(table->scaling);
    auto const unit = (scal != nullptr) ? scal->unit : std::string{};
    auto const prec = (scal != nullptr) ? scal->precision : 2;

    ImGui::Text("%s", table->id.c_str());
    if (!unit.empty())
        text_subtle("unit: %s", unit.c_str());
    ImGui::Separator();

    auto stat_row = [&](char const *label, double v) {
        ImGui::Text("%-7s %.*f%s%s", label, prec, v, unit.empty() ? "" : " ", unit.c_str());
    };
    stat_row("min", static_cast<double>(min));
    stat_row("max", static_cast<double>(max));
    stat_row("mean", mean);
    stat_row("stddev", stddev);
    // Percentiles (p10 / p50 / p90) — surface "what's the spike vs.
    // the typical value" without forcing the user to eyeball the
    // histogram. p50 = median; p10/p90 bracket the bulk of the
    // distribution. Computed via nth_element on a copy so the cell
    // order in `cells` (which the histogram still uses) doesn't get
    // shuffled.
    auto percentile = [&cells](double p) -> double {
        std::vector<float> tmp = cells;
        std::size_t const idx = static_cast<std::size_t>(
            std::clamp(p * static_cast<double>(tmp.size()), 0.0,
                       static_cast<double>(tmp.size() - 1)));
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(idx),
                         tmp.end());
        return static_cast<double>(tmp[idx]);
    };
    stat_row("p10", percentile(0.10));
    stat_row("p50", percentile(0.50));
    stat_row("p90", percentile(0.90));
    ImGui::Text("cells:  %zu", cells.size());
    if (state.viewing_working_rom()) {
        ImGui::Text("edited: %zu", edited);
    } else {
        // Reframe the label when reading a non-working ROM: "edited"
        // implies "your edits" which doesn't fit a comparison ROM.
        ImGui::Text("vs source: %zu", edited);
        if (delta_vs_working.has_value()) {
            ImGui::Text("vs working: %zu", *delta_vs_working);
        }
    }

    // Copy to clipboard — small TSV block the user can paste into a
    // notes app, spreadsheet, or forum reply. Format mirrors the
    // stat_row layout above plus the table id + scaling for context.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    if (ImGui::SmallButton("Copy TSV##stats_copy_tsv")) {
        char buf[896];
        char const *delta_label = state.viewing_working_rom() ? "edited" : "vs source";
        // Trailing "vs working" line only appears when viewing a non-
        // working / non-source ROM. Render it conditionally to keep
        // the working-slot output identical to the v1 shape.
        char vs_working[64] = "";
        if (delta_vs_working.has_value()) {
            std::snprintf(vs_working, sizeof vs_working, "vs working\t%zu\n",
                          *delta_vs_working);
        }
        std::snprintf(buf, sizeof buf,
                      "table\t%s\nunit\t%s\nmin\t%.*f\nmax\t%.*f\nmean\t%.*f\n"
                      "stddev\t%.*f\np10\t%.*f\np50\t%.*f\np90\t%.*f\n"
                      "cells\t%zu\n%s\t%zu\n%s",
                      table->id.c_str(), unit.empty() ? "" : unit.c_str(),
                      prec, static_cast<double>(min),
                      prec, static_cast<double>(max),
                      prec, mean,
                      prec, stddev,
                      prec, percentile(0.10),
                      prec, percentile(0.50),
                      prec, percentile(0.90),
                      cells.size(), delta_label, edited, vs_working);
        ImGui::SetClipboardText(buf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("TSV: paste into a spreadsheet (Excel /\n"
                          "Sheets) — columns auto-split on tab.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy MD##stats_copy_md")) {
        char buf[1024];
        std::snprintf(buf, sizeof buf,
                      "**Table:** `%s`%s%s\n\n"
                      "| Stat | Value |\n"
                      "|---|---:|\n"
                      "| min    | %.*f |\n"
                      "| max    | %.*f |\n"
                      "| mean   | %.*f |\n"
                      "| stddev | %.*f |\n"
                      "| p10    | %.*f |\n"
                      "| p50    | %.*f |\n"
                      "| p90    | %.*f |\n"
                      "| cells  | %zu |\n"
                      "| edited | %zu |\n",
                      table->id.c_str(),
                      unit.empty() ? "" : "  ·  unit: ",
                      unit.empty() ? "" : unit.c_str(),
                      prec, static_cast<double>(min),
                      prec, static_cast<double>(max),
                      prec, mean,
                      prec, stddev,
                      prec, percentile(0.10),
                      prec, percentile(0.50),
                      prec, percentile(0.90),
                      cells.size(), edited);
        ImGui::SetClipboardText(buf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Markdown: paste into a forum post / PR\n"
                          "description / Slack message — renders\n"
                          "as a proper table.");
    }

    // Atlas roll-up — purpose / FA24 portability / cluster + co-edit
    // context for the selected table. Sits between the numeric stats
    // and the histogram so the user sees "what is this" alongside
    // "what's the distribution." Lazy-loaded shared singleton; the
    // entire block is suppressed when no atlas is on disk or the
    // selected table id isn't in the atlas (graceful degrade).
    if (auto const *atlas = shared_atlas(); atlas != nullptr) {
        if (auto const *ae = atlas->find_table(table->id); ae != nullptr) {
            ImGui::Separator();
            ImGui::TextColored(chip_fg_accent(), "Atlas");
            if (!ae->purpose.empty()) {
                ImGui::TextWrapped("%s", ae->purpose.c_str());
            }
            if (!ae->fa24_portability.empty()) {
                ImVec4 color = chip_fg_muted();
                if (ae->fa24_portability == "portable") {
                    color = chip_fg_ok();
                } else if (ae->fa24_portability == "needs_retune") {
                    color = chip_fg_caution();
                } else if (ae->fa24_portability == "fa20_specific") {
                    color = chip_fg_danger();
                }
                ImGui::TextColored(color, "FA24: %s",
                                   ae->fa24_portability.c_str());
            }
            // Three badges (common-core / high-variance / needs-def-
            // promotion) on a single line — same vocabulary as the
            // table-editor Atlas-context section so the user pattern-
            // matches across surfaces.
            bool any_badge = false;
            auto badge_inline = [&](char const *label, ImVec4 const &col) {
                if (any_badge) {
                    ImGui::SameLine();
                    ImGui::TextUnformatted(" ");
                    ImGui::SameLine();
                }
                ImGui::TextColored(col, "%s", label);
                any_badge = true;
            };
            if (ae->common_core) {
                badge_inline("\xE2\x98\x85 common-core", chip_fg_accent());
            }
            if (ae->high_variance) {
                badge_inline("high-variance", chip_fg_caution());
            }
            if (ae->needs_def_promotion) {
                badge_inline("needs-def-promotion", chip_fg_warn());
            }
            if (!ae->clusters.empty()) {
                std::string joined;
                for (std::size_t i = 0; i < ae->clusters.size(); ++i) {
                    if (i > 0) {
                        joined += ", ";
                    }
                    joined += ae->clusters[i];
                }
                text_subtle("tuners: %s", joined.c_str());
            }
            if (!ae->co_edits.empty()) {
                std::string joined;
                for (std::size_t i = 0; i < ae->co_edits.size(); ++i) {
                    if (i > 0) {
                        joined += ", ";
                    }
                    joined += ae->co_edits[i];
                }
                text_subtle("co-edits: %s", joined.c_str());
            }
        }
    }

    ImGui::Separator();
    text_subtle("histogram");
    if (ImPlot::BeginPlot("##stats_hist", ImVec2(-FLT_MIN, 160.0f),
                          ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend | ImPlotFlags_NoTitle |
                              ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
        // Pick a reasonable bin count; ImPlot::PlotHistogram chooses
        // smart defaults when bins=ImPlotBin_Sturges.
        ImPlot::PlotHistogram("##bins", cells.data(), static_cast<int>(cells.size()),
                              ImPlotBin_Sturges);
        ImPlot::EndPlot();
    }
    ImGui::End();
}

} // namespace st::ui
