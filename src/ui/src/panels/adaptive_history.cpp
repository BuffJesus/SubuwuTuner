// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Adaptive-learning history visualizer. Same shape as the knock panel:
// load CSV, map columns, compute a snapshot via st::log::adaptive,
// render time-series charts. Domain typing lets us share zero code
// with the knock panel without anything feeling forced — they're
// different shapes of data.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/ai/drift.hpp"
#include "st/log/adaptive_history.hpp"

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

void render_adaptive_history_panel(AppState &state) {
    if (!state.show_adaptive_history_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Adaptive History###Adaptive History (Preview)",
                      &state.show_adaptive_history_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Long-cycle LTFT / DAM / idle-adapt drift. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    ImGui::Text("Log:");
    glossary_tooltip_for(state, "Datalog");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##ah_log_path", state.ah_log_path, sizeof state.ah_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##ah_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.ah_log_path, sizeof state.ah_log_path, "%s", out_path.get());
            state.ah_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.ah_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.ah_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.ah_load_error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Timestamp col", state.ah_ts_col, sizeof state.ah_ts_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("LTFT col", state.ah_ltft_col, sizeof state.ah_ltft_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("DAM col", state.ah_dam_col, sizeof state.ah_dam_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("IdleAdapt col", state.ah_iac_col, sizeof state.ah_iac_col);
        text_subtle("Leave a column name blank to skip that signal.");
    }

    if (ImGui::CollapsingHeader("Bucketing")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Bucket seconds", &state.ah_bucket_seconds, 60.0f, 3600.0f, "%.1f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("86400 = 1 day  ·  3600 = 1 hour  ·  0 = no bucketing");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        char const *ts_units[] = {"seconds", "millis", "micros", "rows"};
        ImGui::Combo("Timestamp unit", &state.ah_ts_unit, ts_units, 4);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Min samples / bucket", &state.ah_min_samples_per_bucket, 1, 5);
        if (state.ah_min_samples_per_bucket < 0)
            state.ah_min_samples_per_bucket = 0;
    }

    ImGui::Spacing();
    if (ImGui::Button("\xEE\x9D\xA8  Compute snapshot##ah")) {
        state.ah_load_error.clear();
        if (state.ah_log_path[0] == '\0') {
            state.ah_load_error = "Pick a CSV log first.";
        } else {
            // Read header to resolve column names → indices.
            std::ifstream f{state.ah_log_path};
            if (!f) {
                state.ah_load_error = std::string{"Cannot open '"} + state.ah_log_path + "'";
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
                    state.ah_load_error = "CSV has no header.";
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
                            return st::log::adaptive::kNoColumn;
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
                        return st::log::adaptive::kNoColumn;
                    };
                    st::log::adaptive::ColumnMapping mapping;
                    mapping.timestamp_idx = resolve(state.ah_ts_col);
                    mapping
                        .signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Ltft)] =
                        resolve(state.ah_ltft_col);
                    mapping
                        .signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Dam)] =
                        resolve(state.ah_dam_col);
                    mapping.signal_idx[static_cast<std::size_t>(
                        st::log::adaptive::SignalKind::IdleAdapt)] = resolve(state.ah_iac_col);

                    st::log::adaptive::BucketConfig cfg;
                    cfg.bucket_seconds = static_cast<double>(state.ah_bucket_seconds);
                    cfg.min_samples_per_bucket =
                        static_cast<std::uint32_t>(state.ah_min_samples_per_bucket);
                    switch (state.ah_ts_unit) {
                    case 1:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMillis;
                        break;
                    case 2:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMicros;
                        break;
                    case 3:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::RowIndex;
                        break;
                    default:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixSeconds;
                        break;
                    }
                    auto const r =
                        st::log::adaptive::snapshot_from_csv(state.ah_log_path, mapping, cfg);
                    if (!r.has_value()) {
                        state.ah_load_error = r.error().to_string();
                        state.ah_snapshot.reset();
                    } else {
                        state.ah_snapshot = *r;
                        state.ah_compute_msg =
                            std::to_string(r->total_samples) + " samples across " +
                            std::to_string(r->time_span_seconds / 86400.0).substr(0, 5) + " days.";
                    }
                }
            }
        }
    }
    if (!state.ah_compute_msg.empty() && state.ah_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.ah_compute_msg.c_str());
    }

    if (state.ah_snapshot.has_value()) {
        auto const &snap = *state.ah_snapshot;
        ImGui::Separator();

        char const *signal_names[3] = {"LTFT", "DAM", "IdleAdapt"};
        // Summary table
        if (ImGui::BeginTable("##ah_summary", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Signal");
            ImGui::TableSetupColumn("Buckets");
            ImGui::TableSetupColumn("Mean");
            ImGui::TableSetupColumn("Min");
            ImGui::TableSetupColumn("Max");
            ImGui::TableSetupColumn("Drift / day");
            ImGui::TableHeadersRow();
            for (std::size_t k = 0; k < st::log::adaptive::kSignalCount; ++k) {
                auto const &ser = snap.series[k];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(signal_names[k]);
                if (!ser.has_data) {
                    for (int c = 0; c < 5; ++c) {
                        ImGui::TableNextColumn();
                        text_subtle("—");
                    }
                    continue;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", ser.points.size());
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.overall_mean);
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.overall_min);
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.overall_max);
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.drift_per_second * 86400.0);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();

        // Drift diagnosis (Tier-1 rule classifier, st::ai::drift).
        // Runs once per render frame — `classify` is pure and fast
        // (no allocations beyond the returned struct), no caching
        // needed. Renders as a chip + collapsible evidence so the
        // panel doesn't grow tall by default. Confidence drives the
        // chip color; description sits next to it.
        {
            auto const diag = st::ai::drift::classify(snap);
            char const *cause_label = diag.cause.empty() ? "no_signal"
                                                          : diag.cause.c_str();
            ImVec4 chip_fg;
            ImVec4 chip_bg;
            using C = st::ai::drift::Confidence;
            switch (diag.confidence) {
            case C::Likely:
                chip_fg = chip_fg_danger();
                chip_bg = chip_bg_danger();
                break;
            case C::Possible:
                chip_fg = chip_fg_caution();
                chip_bg = chip_bg_caution();
                break;
            case C::Ambiguous:
                chip_fg = chip_fg_warn();
                chip_bg = chip_bg_warn();
                break;
            case C::NoSignal:
            default:
                chip_fg = chip_fg_muted();
                chip_bg = chip_bg_muted();
                break;
            }
            // Diagnosis chip + confidence chip side-by-side; the
            // description prose follows on the same line.
            char chip_label[96];
            std::snprintf(chip_label, sizeof chip_label, "Diagnosis: %s",
                          cause_label);
            chip(chip_label, chip_fg, chip_bg);
            ImGui::SameLine();
            char conf_label[64];
            auto const cn = st::ai::drift::confidence_name(diag.confidence);
            std::snprintf(conf_label, sizeof conf_label, "%.*s",
                          static_cast<int>(cn.size()), cn.data());
            chip(conf_label, chip_fg, chip_bg);
            ImGui::SameLine();
            text_subtle("%s", diag.description.c_str());

            // Evidence + alternatives + recommended-checks land in a
            // collapsible so the chip line stays uncluttered when the
            // user just wants the headline. Default-closed for
            // NoSignal (nothing to investigate) and Possible
            // (one-piece evidence is in the description); default-
            // open for Likely / Ambiguous where the user benefits
            // from seeing alternatives + checks up front.
            bool const open_default =
                diag.confidence == C::Likely || diag.confidence == C::Ambiguous;
            if (open_default) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
            }
            if (ImGui::CollapsingHeader("Evidence + recommended checks##ah_drift")) {
                if (!diag.evidence.empty()) {
                    ImGui::TextUnformatted("Evidence:");
                    for (auto const &e : diag.evidence) {
                        ImGui::BulletText("%s", e.c_str());
                    }
                }
                if (!diag.alternatives.empty()) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Alternatives:");
                    for (auto const &alt : diag.alternatives) {
                        ImGui::BulletText("%s", alt.c_str());
                    }
                }
                if (!diag.recommended_checks.empty()) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Recommended checks:");
                    for (auto const &c : diag.recommended_checks) {
                        ImGui::BulletText("%s", c.c_str());
                    }
                }
            }
            ImGui::Spacing();
        }

        // One time-series plot per signal that has data.
        ImVec2 const avail = ImGui::GetContentRegionAvail();
        float const plot_w = avail.x;
        float const plot_h = 140.0f;
        for (std::size_t k = 0; k < st::log::adaptive::kSignalCount; ++k) {
            auto const &ser = snap.series[k];
            if (!ser.has_data || ser.points.empty())
                continue;
            ImGui::Text("%s", signal_names[k]);
            // Build parallel x/y arrays (ImPlot doesn't take struct
            // arrays without explicit stride/offset; this is cleaner).
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(ser.points.size());
            ys.reserve(ser.points.size());
            for (auto const &p : ser.points) {
                xs.push_back(p.bucket_center);
                ys.push_back(p.mean);
            }
            char plot_id[32];
            std::snprintf(plot_id, sizeof plot_id, "##ah_plot_%zu", k);
            if (ImPlot::BeginPlot(plot_id, ImVec2(plot_w, plot_h),
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                                      ImPlotFlags_NoMouseText)) {
                ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit,
                                  ImPlotAxisFlags_AutoFit);
                ImPlot::PlotLine(signal_names[k], xs.data(), ys.data(),
                                 static_cast<int>(xs.size()));
                ImPlot::EndPlot();
            }
            ImGui::Spacing();
        }
    } else if (state.ah_load_error.empty()) {
        text_subtle("No snapshot yet — pick a log and click \"Compute snapshot\".");
    }

    ImGui::End();
}

} // namespace st::ui
