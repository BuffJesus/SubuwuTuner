// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Cold-start analysis panel. Same shape as knock/adaptive: load CSV,
// map columns, compute snapshot, render results. The methodology piece
// (target lambda curve) is a free-form text buffer "ect:lambda,..." for
// v1.x; a richer curve editor is roadmap.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/log/coldstart.hpp"

#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

void render_coldstart_panel(AppState &state) {
    if (!state.show_coldstart_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Cold-Start Analysis###Cold-Start Analysis (Preview)",
                      &state.show_coldstart_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Phase-classify + ECT-bin a cold-start datalog. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    ImGui::Text("Log:");
    glossary_tooltip_for(state, "Datalog");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##cs_log_path", state.cs_log_path, sizeof state.cs_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cs_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.cs_log_path, sizeof state.cs_log_path, "%s", out_path.get());
            state.cs_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.cs_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.cs_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.cs_load_error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Timestamp col", state.cs_ts_col, sizeof state.cs_ts_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("ECT col", state.cs_ect_col, sizeof state.cs_ect_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("IAT col", state.cs_iat_col, sizeof state.cs_iat_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("RPM col", state.cs_rpm_col, sizeof state.cs_rpm_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Obs λ col", state.cs_obs_col, sizeof state.cs_obs_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Cmd λ col", state.cs_cmd_col, sizeof state.cs_cmd_col);
        text_subtle("ECT and RPM are mandatory. Others are optional.");
    }

    if (ImGui::CollapsingHeader("Window")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Cold threshold °C", &state.cs_cold_threshold_c, 1.0f, 5.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("ECT bin width °C", &state.cs_ect_bin_width_c, 0.5f, 2.5f, "%.1f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Min samples/bin", &state.cs_min_samples_per_bin, 1, 5);
        if (state.cs_min_samples_per_bin < 0)
            state.cs_min_samples_per_bin = 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        char const *ts_units[] = {"seconds", "millis", "micros", "rows"};
        ImGui::Combo("Timestamp unit", &state.cs_ts_unit, ts_units, 4);
    }

    if (ImGui::CollapsingHeader("Target lambda curve", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##cs_target_curve", state.cs_target_curve, sizeof state.cs_target_curve);
        text_subtle("Format: ect:lambda,ect:lambda,...  (linear interp).");
    }

    ImGui::Spacing();
    if (ImGui::Button("\xEE\x9D\xA8  Compute snapshot##cs")) {
        state.cs_load_error.clear();
        state.cs_target_curve_parsed.clear();
        // Parse target curve.
        bool curve_ok = true;
        {
            std::string buf{state.cs_target_curve};
            std::size_t start = 0;
            for (std::size_t i = 0; i <= buf.size(); ++i) {
                if (i == buf.size() || buf[i] == ',') {
                    std::string_view pair{buf.data() + start, i - start};
                    while (!pair.empty() && std::isspace(static_cast<unsigned char>(pair.front())))
                        pair.remove_prefix(1);
                    while (!pair.empty() && std::isspace(static_cast<unsigned char>(pair.back())))
                        pair.remove_suffix(1);
                    if (!pair.empty()) {
                        auto const colon = pair.find(':');
                        if (colon == std::string_view::npos) {
                            state.cs_load_error =
                                "Target curve entry '" + std::string{pair} + "' must be ect:lambda";
                            curve_ok = false;
                            break;
                        }
                        try {
                            double const ect = std::stod(std::string{pair.substr(0, colon)});
                            double const lam = std::stod(std::string{pair.substr(colon + 1)});
                            state.cs_target_curve_parsed.emplace_back(ect, lam);
                        } catch (...) {
                            state.cs_load_error = "Target curve entry '" + std::string{pair} +
                                                  "' has non-numeric component";
                            curve_ok = false;
                            break;
                        }
                    }
                    start = i + 1;
                }
            }
            std::sort(state.cs_target_curve_parsed.begin(), state.cs_target_curve_parsed.end(),
                      [](auto const &a, auto const &b) { return a.first < b.first; });
        }
        if (!curve_ok) {
            // Error already set; skip the rest.
        } else if (state.cs_log_path[0] == '\0') {
            state.cs_load_error = "Pick a CSV log first.";
        } else {
            std::ifstream f{state.cs_log_path};
            if (!f) {
                state.cs_load_error = std::string{"Cannot open '"} + state.cs_log_path + "'";
            } else {
                std::string header_line;
                bool got = false;
                while (std::getline(f, header_line)) {
                    while (!header_line.empty() && header_line.back() == '\r') {
                        header_line.pop_back();
                    }
                    if (header_line.empty() || header_line.front() == '#')
                        continue;
                    got = true;
                    break;
                }
                if (!got) {
                    state.cs_load_error = "CSV has no header.";
                } else {
                    std::vector<std::string> header_cols;
                    std::size_t start = 0;
                    for (std::size_t i = 0; i <= header_line.size(); ++i) {
                        if (i == header_line.size() || header_line[i] == ',') {
                            std::size_t a = start;
                            std::size_t b = i;
                            while (a < b &&
                                   std::isspace(static_cast<unsigned char>(header_line[a])))
                                ++a;
                            while (b > a &&
                                   std::isspace(static_cast<unsigned char>(header_line[b - 1])))
                                --b;
                            header_cols.emplace_back(header_line.substr(a, b - a));
                            start = i + 1;
                        }
                    }
                    auto const resolve = [&](char const *name) -> std::size_t {
                        if (name == nullptr || name[0] == '\0') {
                            return st::log::coldstart::kNoColumn;
                        }
                        std::string want{name};
                        for (auto &cc : want) {
                            cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                        }
                        for (std::size_t i = 0; i < header_cols.size(); ++i) {
                            std::string have = header_cols[i];
                            for (auto &cc : have) {
                                cc =
                                    static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            }
                            if (have == want)
                                return i;
                        }
                        return st::log::coldstart::kNoColumn;
                    };
                    st::log::coldstart::PidMapping mapping;
                    mapping.timestamp_idx = resolve(state.cs_ts_col);
                    mapping.ect_idx = resolve(state.cs_ect_col);
                    mapping.iat_idx = resolve(state.cs_iat_col);
                    mapping.rpm_idx = resolve(state.cs_rpm_col);
                    mapping.observed_lambda_idx = resolve(state.cs_obs_col);
                    mapping.commanded_lambda_idx = resolve(state.cs_cmd_col);

                    st::log::coldstart::WindowConfig cfg;
                    cfg.cold_threshold_c = static_cast<double>(state.cs_cold_threshold_c);
                    cfg.ect_bin_width_c = static_cast<double>(state.cs_ect_bin_width_c);
                    cfg.min_samples_per_bin =
                        static_cast<std::uint32_t>(state.cs_min_samples_per_bin);
                    switch (state.cs_ts_unit) {
                    case 1:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixMillis;
                        break;
                    case 2:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixMicros;
                        break;
                    case 3:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::RowIndex;
                        break;
                    default:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixSeconds;
                        break;
                    }

                    st::log::coldstart::Methodology methodology;
                    methodology.target_lambda_vs_ect.points = state.cs_target_curve_parsed;

                    auto const r = st::log::coldstart::snapshot_from_csv(state.cs_log_path, mapping,
                                                                         cfg, methodology);
                    if (!r.has_value()) {
                        state.cs_load_error = r.error().to_string();
                        state.cs_snapshot.reset();
                    } else {
                        state.cs_snapshot = *r;
                        state.cs_compute_msg =
                            std::to_string(r->samples_considered) + " samples; mean lambda dev " +
                            (std::to_string(r->mean_lambda_deviation).substr(0, 6));
                    }
                }
            }
        }
    }
    if (!state.cs_compute_msg.empty() && state.cs_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.cs_compute_msg.c_str());
    }

    if (state.cs_snapshot.has_value()) {
        auto const &snap = *state.cs_snapshot;
        ImGui::Separator();
        char const *phase_names[6] = {"PreCrank", "Cranking", "InitialFiring",
                                      "HighIdle", "Warmup",   "ClosedLoop"};

        ImGui::Text("ECT span: %+.1f°C .. %+.1f°C  ·  considered %llu, gated %llu",
                    snap.coldest_ect_c, snap.warmest_ect_c,
                    static_cast<unsigned long long>(snap.samples_considered),
                    static_cast<unsigned long long>(snap.samples_gated_out));
        ImGui::Spacing();

        // Phase summary table
        if (ImGui::BeginTable("##cs_phase", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Phase");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("Seconds");
            ImGui::TableHeadersRow();
            for (std::size_t p = 0; p < st::log::coldstart::kPhaseCount; ++p) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(phase_names[p]);
                ImGui::TableNextColumn();
                ImGui::Text("%u", static_cast<unsigned>(snap.phase_counts[p]));
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", snap.phase_seconds[p]);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();

        // ECT bins + lambda chart
        if (!snap.ect_bins.empty()) {
            ImGui::Text("ECT bins (observed lambda vs target):");
            std::vector<double> bin_ect, bin_obs, bin_target;
            bin_ect.reserve(snap.ect_bins.size());
            bin_obs.reserve(snap.ect_bins.size());
            bin_target.reserve(snap.ect_bins.size());
            for (auto const &b : snap.ect_bins) {
                bin_ect.push_back(b.ect_center_c);
                bin_obs.push_back(b.observed_lambda_mean);
                bin_target.push_back(b.observed_lambda_mean - b.deviation_from_target);
            }
            ImVec2 const avail = ImGui::GetContentRegionAvail();
            if (ImPlot::BeginPlot("##cs_lambda_plot", ImVec2(avail.x, 200.0f),
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoMenus)) {
                ImPlot::SetupAxes("ECT (°C)", "lambda", ImPlotAxisFlags_AutoFit,
                                  ImPlotAxisFlags_AutoFit);
                ImPlot::PlotLine("observed", bin_ect.data(), bin_obs.data(),
                                 static_cast<int>(bin_ect.size()));
                if (!state.cs_target_curve_parsed.empty()) {
                    ImPlot::PlotLine("target", bin_ect.data(), bin_target.data(),
                                     static_cast<int>(bin_ect.size()));
                }
                ImPlot::EndPlot();
            }
            ImGui::Spacing();

            if (ImGui::BeginTable("##cs_bins", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 200.0f))) {
                ImGui::TableSetupColumn("ECT °C");
                ImGui::TableSetupColumn("n");
                ImGui::TableSetupColumn("obs mean");
                ImGui::TableSetupColumn("obs min");
                ImGui::TableSetupColumn("obs max");
                ImGui::TableSetupColumn("dev");
                ImGui::TableHeadersRow();
                for (auto const &b : snap.ect_bins) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.1f", b.ect_center_c);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", static_cast<unsigned>(b.count));
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.observed_lambda_mean);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.observed_lambda_min);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.observed_lambda_max);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.deviation_from_target);
                }
                ImGui::EndTable();
            }
        } else {
            text_subtle("No ECT bins — log has no cold-regime samples with "
                        "observed-lambda data above min_samples_per_bin.");
        }
    } else if (state.cs_load_error.empty()) {
        text_subtle("No snapshot yet — pick a log and click \"Compute snapshot\".");
    }

    ImGui::End();
}

} // namespace st::ui
