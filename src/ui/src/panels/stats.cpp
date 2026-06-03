// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Right-rail quick-stats panel for the currently-selected table.
// Min/max/mean/stddev across all cells, count of edited cells (diff
// against source), and an ImPlot histogram. Read-only; updates each
// frame the table data is loaded.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>
#include <implot.h>

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace st::ui {

void render_stats_panel(AppState &state) {
    if (!state.show_stats_panel) {
        return;
    }
    if (!ImGui::Begin("Stats", &state.show_stats_panel)) {
        ImGui::End();
        return;
    }
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

    // Count edited cells (diff working vs source). Cheap: re-read source
    // table data once per frame. For 1D / 2D tables of moderate size this
    // is well under a millisecond.
    std::size_t edited = 0;
    auto const source_td =
        state.project->definition().read_table_values(state.project->source_rom(), *table);
    if (source_td.has_value() && source_td->values.size() == td.values.size()) {
        for (std::size_t r = 0; r < td.values.size(); ++r) {
            if (source_td->values[r].size() != td.values[r].size())
                continue;
            for (std::size_t c = 0; c < td.values[r].size(); ++c) {
                if (td.values[r][c] != source_td->values[r][c])
                    ++edited;
            }
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
    ImGui::Text("edited: %zu", edited);

    // Copy to clipboard — small TSV block the user can paste into a
    // notes app, spreadsheet, or forum reply. Format mirrors the
    // stat_row layout above plus the table id + scaling for context.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    if (ImGui::SmallButton("Copy TSV##stats_copy_tsv")) {
        char buf[768];
        std::snprintf(buf, sizeof buf,
                      "table\t%s\nunit\t%s\nmin\t%.*f\nmax\t%.*f\nmean\t%.*f\n"
                      "stddev\t%.*f\np10\t%.*f\np50\t%.*f\np90\t%.*f\n"
                      "cells\t%zu\nedited\t%zu\n",
                      table->id.c_str(), unit.empty() ? "" : unit.c_str(),
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
