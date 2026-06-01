// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Per-cylinder knock dashboard. Mirrors the CLI's `knock-snapshot`
// subcommand: load a CSV, map columns to mapping fields, compute a
// snapshot via st::log::knock::snapshot_from_csv, render strip charts.
// State lives in AppState (knock_* fields) so the panel survives
// across frames without re-loading the CSV per refresh.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/log/knock_dashboard.hpp"

#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace st::ui {

void render_knock_dashboard_panel(AppState &state) {
    if (!state.show_knock_dashboard_panel) {
        return;
    }
    // `###` preserves the ImGui ID hash from the old "(Preview)" label
    // so docked-layout entries in users' imgui.ini still attach to this
    // window after the visible title change. Same trick repeated in the
    // other four preview panels below.
    // First-show default: dock as a tab in the central area alongside
    // Table instead of plopping floating top-left. FirstUseEver respects
    // any subsequent user choice to tear out or re-dock. Same pattern
    // applied to the four other datalog/features panels below.
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Knock Dashboard###Knock Dashboard (Preview)",
                      &state.show_knock_dashboard_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Per-cylinder knock from a CSV datalog. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    // ---- Log + Browse -------------------------------------------------
    ImGui::Text("Log:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##knock_log_path", state.knock_log_path, sizeof state.knock_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##knock_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.knock_log_path, sizeof state.knock_log_path, "%s", out_path.get());
            state.knock_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.knock_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.knock_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.knock_load_error.c_str());
        ImGui::PopStyleColor();
    }

    // ---- Column mapping ----------------------------------------------
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderInt("Cylinders", &state.knock_cylinder_count, 1, 6);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("RPM column", state.knock_rpm_col, sizeof state.knock_rpm_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Load column", state.knock_load_col, sizeof state.knock_load_col);
        for (int c = 0; c < state.knock_cylinder_count; ++c) {
            ImGui::PushID(c);
            char label_flkc[32];
            std::snprintf(label_flkc, sizeof label_flkc, "FLKC cyl %d", c + 1);
            char label_fbkc[32];
            std::snprintf(label_fbkc, sizeof label_fbkc, "FBKC cyl %d", c + 1);
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText(label_flkc, state.knock_flkc_cols[c], sizeof state.knock_flkc_cols[c]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText(label_fbkc, state.knock_fbkc_cols[c], sizeof state.knock_fbkc_cols[c]);
            ImGui::PopID();
        }
    }

    // ---- Window config -----------------------------------------------
    if (ImGui::CollapsingHeader("Window")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Window seconds", &state.knock_window_seconds, 0.5f, 5.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Sample rate Hz", &state.knock_sample_rate_hz, 1.0f, 5.0f, "%.1f");
        ImGui::Checkbox("Load gate", &state.knock_gate_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("min RPM", &state.knock_min_rpm, 100.0f, 500.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("min load", &state.knock_min_load, 0.1f, 0.5f, "%.2f");
    }

    ImGui::Spacing();
    // ---- Compute button ----------------------------------------------
    if (ImGui::Button("Compute snapshot")) {
        state.knock_load_error.clear();
        if (state.knock_log_path[0] == '\0') {
            state.knock_load_error = "Pick a CSV log first.";
        } else {
            st::log::knock::PidMapping mapping;
            mapping.cylinder_count = static_cast<std::uint8_t>(state.knock_cylinder_count);

            // Load + parse header to resolve column names → indices.
            std::ifstream f{state.knock_log_path};
            if (!f) {
                state.knock_load_error = std::string{"Cannot open '"} + state.knock_log_path + "'";
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
                    state.knock_load_error = "CSV has no header.";
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
                            return st::log::knock::kNoPid;
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
                        return st::log::knock::kNoPid;
                    };
                    mapping.rpm_idx = resolve(state.knock_rpm_col);
                    mapping.load_idx = resolve(state.knock_load_col);
                    bool any_unresolved = false;
                    for (int c = 0; c < state.knock_cylinder_count; ++c) {
                        auto const cs = static_cast<std::size_t>(c);
                        std::size_t const f_idx = resolve(state.knock_flkc_cols[c]);
                        std::size_t const b_idx = resolve(state.knock_fbkc_cols[c]);
                        mapping.fine_knock_learn[cs] = f_idx;
                        mapping.feedback_knock[cs] = b_idx;
                        if (state.knock_flkc_cols[c][0] != '\0' && f_idx == st::log::knock::kNoPid)
                            any_unresolved = true;
                        if (state.knock_fbkc_cols[c][0] != '\0' && b_idx == st::log::knock::kNoPid)
                            any_unresolved = true;
                    }
                    if (any_unresolved) {
                        state.knock_load_error =
                            "One or more column names didn't match the CSV "
                            "header. Mappings without a match will be ignored.";
                    }
                    st::log::knock::WindowConfig cfg;
                    cfg.window_seconds = static_cast<double>(state.knock_window_seconds);
                    cfg.sample_rate_hz = static_cast<double>(state.knock_sample_rate_hz);
                    cfg.min_rpm = static_cast<double>(state.knock_min_rpm);
                    cfg.min_load = static_cast<double>(state.knock_min_load);
                    cfg.require_load_gate = state.knock_gate_enabled;
                    auto const r =
                        st::log::knock::snapshot_from_csv(state.knock_log_path, mapping, cfg);
                    if (!r.has_value()) {
                        state.knock_load_error = r.error().to_string();
                        state.knock_snapshot.reset();
                    } else {
                        state.knock_snapshot = *r;
                        state.knock_compute_msg =
                            "Considered " + std::to_string(r->samples_considered) +
                            " samples (gated out " + std::to_string(r->samples_gated_out) + ").";
                    }
                }
            }
        }
    }
    if (!state.knock_compute_msg.empty() && state.knock_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.knock_compute_msg.c_str());
    }

    // ---- Grid of per-cyl strip charts --------------------------------
    if (state.knock_snapshot.has_value()) {
        auto const &snap = *state.knock_snapshot;
        ImGui::Separator();
        int const cyls = static_cast<int>(snap.cylinder_count);
        int const cols = (cyls <= 4) ? 2 : 3;
        ImVec2 const avail = ImGui::GetContentRegionAvail();
        float const cell_w =
            (avail.x - static_cast<float>(cols - 1) * 8.0f) / static_cast<float>(cols);
        float const cell_h = 180.0f;
        for (int c = 0; c < cyls; ++c) {
            auto const &p = snap.per_cyl[static_cast<std::size_t>(c)];
            bool const no_data = p.strip_flkc.empty() && p.strip_fbkc.empty();
            ImGui::BeginGroup();
            ImGui::Text("Cyl %d", c + 1);
            if (no_data) {
                ImGui::Dummy(ImVec2(cell_w, cell_h));
                ImGui::SameLine(8.0f);
                text_subtle("(no per-cylinder signal in mapping)");
            } else {
                ImGui::Text("FLKC cur: %+.2f  mean: %+.2f  min: %+.2f  "
                            "FBKC cur: %+.2f  events: %u  dMean: %+.2f",
                            p.current_flkc, p.mean_flkc_window, p.min_flkc_window, p.current_fbkc,
                            p.event_count_window, p.delta_from_cyl_mean);
                char plot_id[32];
                std::snprintf(plot_id, sizeof plot_id, "##knock_cyl_%d", c);
                if (ImPlot::BeginPlot(plot_id, ImVec2(cell_w, cell_h),
                                      ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                                          ImPlotFlags_NoMouseText)) {
                    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit,
                                      ImPlotAxisFlags_AutoFit);
                    if (!p.strip_flkc.empty()) {
                        ImPlot::PlotLine("FLKC", p.strip_flkc.data(),
                                         static_cast<int>(p.strip_flkc.size()));
                    }
                    if (!p.strip_fbkc.empty()) {
                        ImPlot::PlotLine("FBKC", p.strip_fbkc.data(),
                                         static_cast<int>(p.strip_fbkc.size()));
                    }
                    ImPlot::EndPlot();
                }
            }
            ImGui::EndGroup();
            // Next column / row
            if ((c + 1) % cols != 0 && c + 1 < cyls) {
                ImGui::SameLine();
            }
        }
    } else if (state.knock_load_error.empty()) {
        text_subtle("No snapshot yet — pick a log and click \"Compute snapshot\".");
    }

    ImGui::End();
}

} // namespace st::ui
