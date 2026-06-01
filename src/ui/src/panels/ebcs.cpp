// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// EBCS PID assistant panel. Detect tip-in events, characterize each
// step response, produce heuristic gain-adjustment suggestions. Same
// shape as the other §11 panels.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/log/ebcs.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace st::ui {

void render_ebcs_panel(AppState &state) {
    if (!state.show_ebcs_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("EBCS PID Assistant###EBCS PID Assistant (Preview)",
                      &state.show_ebcs_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Detect tip-in events; suggest Kp / Ki / Kd direction. "
                "Output is advisory — verify on a dyno. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    ImGui::Text("Log:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##ebcs_log_path", state.ebcs_log_path, sizeof state.ebcs_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##ebcs_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.ebcs_log_path, sizeof state.ebcs_log_path, "%s", out_path.get());
            state.ebcs_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.ebcs_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.ebcs_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.ebcs_load_error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Timestamp col", state.ebcs_ts_col, sizeof state.ebcs_ts_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Target boost col", state.ebcs_target_col, sizeof state.ebcs_target_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Actual boost col", state.ebcs_actual_col, sizeof state.ebcs_actual_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Throttle col", state.ebcs_throttle_col, sizeof state.ebcs_throttle_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("WGDC col", state.ebcs_wgdc_col, sizeof state.ebcs_wgdc_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("RPM col", state.ebcs_rpm_col, sizeof state.ebcs_rpm_col);
        text_subtle("Timestamp, target boost, actual boost, and throttle are mandatory.");
    }

    if (ImGui::CollapsingHeader("Detection")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Throttle step %", &state.ebcs_throttle_step_pct, 1.0f, 5.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Target boost step", &state.ebcs_target_step, 0.5f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Max event duration s", &state.ebcs_max_event_duration, 0.5f, 1.0f,
                          "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Overshoot warn %", &state.ebcs_overshoot_warn_pct, 1.0f, 5.0f, "%.1f");
        ImGui::SetNextItemWidth(140.0f);
        char const *ts_units[] = {"seconds", "millis", "micros", "rows"};
        ImGui::Combo("Timestamp unit", &state.ebcs_ts_unit, ts_units, 4);
    }

    ImGui::Spacing();
    if (ImGui::Button("Analyze##ebcs")) {
        state.ebcs_load_error.clear();
        if (state.ebcs_log_path[0] == '\0') {
            state.ebcs_load_error = "Pick a CSV log first.";
        } else {
            std::ifstream f{state.ebcs_log_path};
            if (!f) {
                state.ebcs_load_error = std::string{"Cannot open '"} + state.ebcs_log_path + "'";
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
                    state.ebcs_load_error = "CSV has no header.";
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
                            return st::log::ebcs::kNoColumn;
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
                        return st::log::ebcs::kNoColumn;
                    };
                    st::log::ebcs::PidMapping mapping;
                    mapping.timestamp_idx = resolve(state.ebcs_ts_col);
                    mapping.target_boost_idx = resolve(state.ebcs_target_col);
                    mapping.actual_boost_idx = resolve(state.ebcs_actual_col);
                    mapping.throttle_idx = resolve(state.ebcs_throttle_col);
                    mapping.wgdc_idx = resolve(state.ebcs_wgdc_col);
                    mapping.rpm_idx = resolve(state.ebcs_rpm_col);

                    st::log::ebcs::DetectorConfig cfg;
                    cfg.throttle_step_threshold_pct =
                        static_cast<double>(state.ebcs_throttle_step_pct);
                    cfg.target_boost_step_threshold = static_cast<double>(state.ebcs_target_step);
                    cfg.max_event_duration_s = static_cast<double>(state.ebcs_max_event_duration);
                    cfg.overshoot_warn_pct = static_cast<double>(state.ebcs_overshoot_warn_pct);
                    switch (state.ebcs_ts_unit) {
                    case 1:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixMillis;
                        break;
                    case 2:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixMicros;
                        break;
                    case 3:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::RowIndex;
                        break;
                    default:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixSeconds;
                        break;
                    }
                    auto const r =
                        st::log::ebcs::snapshot_from_csv(state.ebcs_log_path, mapping, cfg);
                    if (!r.has_value()) {
                        state.ebcs_load_error = r.error().to_string();
                        state.ebcs_snapshot.reset();
                    } else {
                        state.ebcs_snapshot = *r;
                        state.ebcs_compute_msg = std::to_string(r->good_event_count) + " good of " +
                                                 std::to_string(r->total_event_count) + " events";
                    }
                }
            }
        }
    }
    if (!state.ebcs_compute_msg.empty() && state.ebcs_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.ebcs_compute_msg.c_str());
    }

    if (state.ebcs_snapshot.has_value()) {
        auto const &snap = *state.ebcs_snapshot;
        ImGui::Separator();
        char const *quality_names[5] = {"Good", "Choppy", "Slow", "Spiked", "Aborted"};

        if (snap.good_event_count > 0) {
            ImGui::Text("Median rise time:        %.3f s", snap.median_rise_time_s);
            ImGui::Text("Median overshoot:        %.2f %%", snap.median_overshoot_pct);
            ImGui::Text("Median settling time:    %.3f s", snap.median_settling_time_s);
            ImGui::Text("Mean steady-state error: %+.3f", snap.mean_steady_state_error);
        }

        ImGui::Spacing();
        ImGui::Text("Suggestions:");
        for (auto const &line : snap.gain_suggestions) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str());
        }

        if (!snap.events.empty()) {
            ImGui::Spacing();
            ImGui::Text("Tip-in events:");
            if (ImGui::BeginTable("##ebcs_events", 8,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 180.0f))) {
                ImGui::TableSetupColumn("start s");
                ImGui::TableSetupColumn("target");
                ImGui::TableSetupColumn("peak");
                ImGui::TableSetupColumn("ss");
                ImGui::TableSetupColumn("rise s");
                ImGui::TableSetupColumn("os %");
                ImGui::TableSetupColumn("settle s");
                ImGui::TableSetupColumn("quality");
                ImGui::TableHeadersRow();
                for (auto const &ev : snap.events) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", ev.start_timestamp);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.2f", ev.target_boost);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.2f", ev.peak_boost);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.2f", ev.steady_state_boost);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", ev.rise_time_s);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", ev.overshoot_pct);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", ev.settling_time_s);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(quality_names[static_cast<std::size_t>(ev.quality)]);
                }
                ImGui::EndTable();
            }
        }
    } else if (state.ebcs_load_error.empty()) {
        text_subtle("No analysis yet — pick a log and click \"Analyze\".");
    }

    ImGui::End();
}

} // namespace st::ui
