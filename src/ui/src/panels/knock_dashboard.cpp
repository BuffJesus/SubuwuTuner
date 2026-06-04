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

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
    track_help_context(state, AppState::HelpContext::KnockDashboard);

    preview_pill();
    ImGui::SameLine();
    text_subtle("Per-cylinder knock from a CSV datalog. "
                "See docs/05-improvements.md §11.");
    glossary_tooltip_for(state, "Datalog");
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
        ImGui::SameLine();
        // Auto-detect — peek at the CSV header row and fill any column
        // fields that match common patterns. Doesn't override
        // user-typed values; only fills empties. Saves the typing
        // pass for the common case where the log uses standard
        // RomRaider / EcuTek field names.
        ImGui::BeginDisabled(state.knock_log_path[0] == '\0');
        if (ImGui::SmallButton("Auto-detect")) {
            std::ifstream in{state.knock_log_path, std::ios::binary};
            if (in) {
                std::string header_line;
                std::getline(in, header_line);
                if (!header_line.empty() && header_line.back() == '\r') {
                    header_line.pop_back();
                }
                // Split on comma; trim whitespace; lowercase compare.
                std::vector<std::string> headers;
                std::size_t s = 0;
                while (s <= header_line.size()) {
                    std::size_t e = header_line.find(',', s);
                    if (e == std::string::npos)
                        e = header_line.size();
                    std::string h = header_line.substr(s, e - s);
                    while (!h.empty() && (h.front() == ' ' || h.front() == '\t' ||
                                          h.front() == '"'))
                        h.erase(h.begin());
                    while (!h.empty() && (h.back() == ' ' || h.back() == '\t' ||
                                          h.back() == '"'))
                        h.pop_back();
                    headers.push_back(std::move(h));
                    if (e == header_line.size())
                        break;
                    s = e + 1;
                }
                auto const find_match = [&](auto pred) -> char const * {
                    for (auto const &h : headers) {
                        std::string lower = h;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (pred(lower))
                            return h.c_str();
                    }
                    return nullptr;
                };
                std::size_t matches = 0;
                auto fill = [&](char *buf, std::size_t cap, char const *src) {
                    if (buf[0] != '\0' || src == nullptr)
                        return;
                    std::snprintf(buf, cap, "%s", src);
                    ++matches;
                };
                // RPM = column containing "rpm" or "engine speed".
                fill(state.knock_rpm_col, sizeof state.knock_rpm_col,
                     find_match([](std::string const &h) {
                         return h.find("rpm") != std::string::npos ||
                                h.find("engine speed") != std::string::npos;
                     }));
                // Load = column containing "load" (but not "fuel load
                // estimate" or similar derivatives — first hit wins).
                fill(state.knock_load_col, sizeof state.knock_load_col,
                     find_match([](std::string const &h) {
                         return h.find("load") != std::string::npos;
                     }));
                // Per-cylinder FBKC / FLKC.
                for (int c = 0; c < state.knock_cylinder_count; ++c) {
                    std::string const tag = std::to_string(c + 1);
                    fill(state.knock_fbkc_cols[c], sizeof state.knock_fbkc_cols[c],
                         find_match([&tag](std::string const &h) {
                             return h.find("fbkc") != std::string::npos &&
                                    h.find(tag) != std::string::npos;
                         }));
                    fill(state.knock_flkc_cols[c], sizeof state.knock_flkc_cols[c],
                         find_match([&tag](std::string const &h) {
                             return h.find("flkc") != std::string::npos &&
                                    h.find(tag) != std::string::npos;
                         }));
                }
                state.knock_load_error = matches > 0
                                             ? "Auto-detect: filled " +
                                                   std::to_string(matches) + " column field(s)."
                                             : "Auto-detect: no header matches found.";
            } else {
                state.knock_load_error = "Auto-detect: cannot open " +
                                          std::string{state.knock_log_path};
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Peek at the CSV's header row and fill empty column\n"
                "fields with matches against common patterns (rpm,\n"
                "load, fbkc_N, flkc_N). Only touches empty fields —\n"
                "your typed values are preserved.");
        }
        ImGui::SameLine();
        // Save / Load preset — persist the current column mapping to
        // a small text file the user picks. Tuners reuse the same
        // logger across many sessions; this skips the re-typing pass.
        if (ImGui::SmallButton("Save preset…##knock_save_preset")) {
            NFD::UniquePath out;
            nfdfilteritem_t const filters[] = {{"Knock preset", "knockcols"}};
            if (NFD::SaveDialog(out, filters, 1, nullptr, "default.knockcols") ==
                NFD_OKAY) {
                std::ofstream fh{out.get(), std::ios::binary};
                if (fh) {
                    fh << "# subuwutuner.knock-cols.v1\n";
                    fh << "cylinders=" << state.knock_cylinder_count << "\n";
                    fh << "rpm=" << state.knock_rpm_col << "\n";
                    fh << "load=" << state.knock_load_col << "\n";
                    for (int c = 0; c < state.knock_cylinder_count; ++c) {
                        fh << "fbkc_" << (c + 1) << "=" << state.knock_fbkc_cols[c] << "\n";
                        fh << "flkc_" << (c + 1) << "=" << state.knock_flkc_cols[c] << "\n";
                    }
                    state.knock_load_error = "Saved preset to " + std::string{out.get()};
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Save the current column mapping to a .knockcols file\n"
                              "for reuse across sessions.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Load preset…##knock_load_preset")) {
            NFD::UniquePath out;
            nfdfilteritem_t const filters[] = {{"Knock preset", "knockcols"}};
            if (NFD::OpenDialog(out, filters, 1) == NFD_OKAY) {
                std::ifstream fh{out.get(), std::ios::binary};
                std::string line;
                int loaded = 0;
                while (std::getline(fh, line)) {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty() || line.front() == '#')
                        continue;
                    auto const eq = line.find('=');
                    if (eq == std::string::npos)
                        continue;
                    auto const key = line.substr(0, eq);
                    auto const val = line.substr(eq + 1);
                    if (key == "cylinders") {
                        try {
                            state.knock_cylinder_count =
                                std::clamp(std::stoi(val), 1, 6);
                        } catch (...) {
                        }
                    } else if (key == "rpm") {
                        std::snprintf(state.knock_rpm_col, sizeof state.knock_rpm_col,
                                      "%s", val.c_str());
                    } else if (key == "load") {
                        std::snprintf(state.knock_load_col, sizeof state.knock_load_col,
                                      "%s", val.c_str());
                    } else if (key.starts_with("fbkc_")) {
                        try {
                            int const idx = std::stoi(key.substr(5)) - 1;
                            if (idx >= 0 && idx < 6) {
                                std::snprintf(state.knock_fbkc_cols[idx],
                                              sizeof state.knock_fbkc_cols[idx],
                                              "%s", val.c_str());
                            }
                        } catch (...) {
                        }
                    } else if (key.starts_with("flkc_")) {
                        try {
                            int const idx = std::stoi(key.substr(5)) - 1;
                            if (idx >= 0 && idx < 6) {
                                std::snprintf(state.knock_flkc_cols[idx],
                                              sizeof state.knock_flkc_cols[idx],
                                              "%s", val.c_str());
                            }
                        } catch (...) {
                        }
                    }
                    ++loaded;
                }
                state.knock_load_error =
                    "Loaded " + std::to_string(loaded) + " preset key(s) from " +
                    std::string{out.get()};
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Load a previously saved column mapping.\n"
                              "Overwrites the current field values.");
        }
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
    if (ImGui::Button("\xEE\x9D\xA8  Compute snapshot")) {
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

    // Export snapshot CSV — one summary row per cylinder plus per-
    // cylinder strip-sample columns. Useful for sharing the
    // before-tune knock state with a co-tuner or attaching to a
    // forum thread.
    if (state.knock_snapshot.has_value()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Export CSV##knock_export")) {
            NFD::UniquePath out;
            nfdfilteritem_t const filters[] = {{"CSV", "csv"}};
            nfdresult_t const r =
                NFD::SaveDialog(out, filters, 1, nullptr, "knock-snapshot.csv");
            if (r == NFD_OKAY) {
                std::filesystem::path const target{out.get()};
                std::ofstream fh{target, std::ios::binary};
                if (!fh) {
                    state.knock_load_error =
                        "Export: cannot open " + target.string();
                } else {
                    auto const &snap = *state.knock_snapshot;
                    fh << "# subuwutuner.knock-snapshot.v1\n";
                    fh << "# window_seconds=" << snap.window_seconds
                       << ", samples_considered=" << snap.samples_considered
                       << ", samples_gated_out=" << snap.samples_gated_out << "\n";
                    fh << "cyl,current_flkc,current_fbkc,mean_flkc_window,"
                          "min_flkc_window,event_count_window,delta_from_cyl_mean,"
                          "strip_flkc_count,strip_fbkc_count\n";
                    int const cyls = static_cast<int>(snap.cylinder_count);
                    for (int c = 0; c < cyls; ++c) {
                        auto const &p = snap.per_cyl[static_cast<std::size_t>(c)];
                        char row[256];
                        std::snprintf(row, sizeof row,
                                      "%d,%g,%g,%g,%g,%u,%g,%zu,%zu\n",
                                      c + 1, p.current_flkc, p.current_fbkc,
                                      p.mean_flkc_window, p.min_flkc_window,
                                      p.event_count_window, p.delta_from_cyl_mean,
                                      p.strip_flkc.size(), p.strip_fbkc.size());
                        fh << row;
                    }
                    // Per-strip rows — wide-format. Each line is
                    // (cyl, sample_idx, flkc, fbkc) so the file can be
                    // sliced in spreadsheets / pandas without parsing
                    // a nested column layout.
                    fh << "\n# per-sample strip data\n";
                    fh << "cyl,sample_idx,flkc,fbkc\n";
                    for (int c = 0; c < cyls; ++c) {
                        auto const &p = snap.per_cyl[static_cast<std::size_t>(c)];
                        std::size_t const n =
                            std::max(p.strip_flkc.size(), p.strip_fbkc.size());
                        for (std::size_t i = 0; i < n; ++i) {
                            double const flkc = i < p.strip_flkc.size() ? p.strip_flkc[i] : 0.0;
                            double const fbkc = i < p.strip_fbkc.size() ? p.strip_fbkc[i] : 0.0;
                            char row[160];
                            std::snprintf(row, sizeof row, "%d,%zu,%g,%g\n", c + 1, i,
                                          flkc, fbkc);
                            fh << row;
                        }
                    }
                    if (!fh) {
                        state.knock_load_error = "Export: write failed";
                    } else {
                        enqueue_toast(state, ToastKind::Success,
                                      "Wrote " + target.string());
                    }
                }
            } else if (r == NFD_ERROR) {
                state.knock_load_error =
                    std::string{"Export dialog: "} + NFD::GetError();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write the current snapshot as CSV — per-cylinder\n"
                              "summary block + per-sample strip data block.");
        }
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
