// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// F6 — datalog viewer. Loads an already-decompressed CSV datalog
// (timestamp + multiple numeric columns) and shows per-channel
// stats (min / max / mean / count) plus a line plot for the
// currently-selected channel. The "decompress the .csv.gz first"
// step is left to the user for v1 — gunzip / 7-Zip / `python -m
// gzip` all work fine, and bringing in miniz inflate just for this
// surface would be more wiring than the wedge needs.
//
// Parser is deliberately minimal: comma-separated, first row is the
// column names, every subsequent cell is parsed as a float via
// strtod (empty cells become NaN, NaNs are excluded from stats).
// Quoted fields aren't supported — every datalog format we ship
// against is plain numeric CSV, no escaping needed.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/library/datalog_csv.hpp"

#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace st::ui {

namespace {

struct State {
    std::string source_path;
    st::library::datalog_csv::ParsedDatalog dl;
    // Per-channel "show in plot" flag. ImPlot can cycle colors
    // through several overlaid series without trouble, so multi-
    // channel overlay falls out naturally — checkboxes in the stats
    // table swap channels in/out of the plot. Use std::vector<char>
    // rather than std::vector<bool> so the per-row bind in the
    // Checkbox call works without the bool-bitfield workaround.
    std::vector<char> plot_visible;
    std::string error;
    bool loaded{false};
};

State &state() {
    static State s;
    return s;
}

void reset_state(State &s) {
    s.source_path.clear();
    s.dl = {};
    s.plot_visible.clear();
    s.error.clear();
    s.loaded = false;
}

void load_csv(State &s, std::string const &path) {
    reset_state(s);
    s.source_path = path;
    std::ifstream f{path, std::ios::binary};
    if (!f) {
        s.error = "Couldn't open " + path + " for read.";
        s.loaded = true;
        return;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string const text = buf.str();
    s.dl = st::library::datalog_csv::parse(text);
    if (s.dl.headers.empty()) {
        s.error = "File is empty or has no header row.";
        s.loaded = true;
        return;
    }
    s.plot_visible.assign(s.dl.headers.size(), char{0});
    for (std::size_t i = 0; i < s.dl.stats.size(); ++i) {
        if (s.dl.stats[i].sample_count > 0) {
            s.plot_visible[i] = char{1};
            break;
        }
    }
    s.loaded = true;
}

void pick_and_load(State &s) {
    nfdu8filteritem_t const filters[] = {
        {"CSV datalog", "csv"},
        {"Any file", "*"},
    };
    NFD::UniquePathU8 path;
    auto const r = NFD::OpenDialog(path, filters, 2);
    if (r == NFD_CANCEL) {
        return;
    }
    if (r != NFD_OKAY || path == nullptr) {
        return;
    }
    load_csv(s, std::string{path.get()});
}

} // namespace

void render_datalog_viewer_modal(AppState &app_state) {
    if (app_state.show_datalog_viewer_modal) {
        ImGui::OpenPopup("\xEE\xA0\x84  Datalog viewer##dl_viewer_modal");
        app_state.show_datalog_viewer_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(840.0f, 540.0f),
                                         ImVec2(1400.0f, 900.0f));
    if (!ImGui::BeginPopupModal(
            "\xEE\xA0\x84  Datalog viewer##dl_viewer_modal", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto &s = state();
    ImGui::TextWrapped(
        "Open a CSV datalog and inspect per-channel stats + a line plot. "
        "Decompress .csv.gz files first (gunzip / 7-Zip / `python -m gzip`).");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Open CSV\xE2\x80\xA6##dl_open")) {
        pick_and_load(s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close##dl_close")) {
        reset_state(s);
        ImGui::CloseCurrentPopup();
    }
    ImGui::Spacing();

    if (!s.loaded) {
        ImGui::TextDisabled(
            "Click \"Open CSV\" to load a datalog file.");
        ImGui::EndPopup();
        return;
    }
    if (!s.error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "Error: %s", s.error.c_str());
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Source:   %s", s.source_path.c_str());
    ImGui::Text("Channels: %zu  Rows: %zu", s.dl.headers.size(), s.dl.row_count);
    ImGui::Spacing();

    // Two-pane: stats table on the left, plot on the right. The
    // table is the selection surface — clicking a row swaps the
    // plotted channel.
    if (ImGui::BeginChild("##dl_split_left",
                           ImVec2(420.0f, 380.0f), true)) {
        // Quick "select all / none" affordances so users with a
        // dense log don't have to click every row.
        if (ImGui::SmallButton("All##dl_all")) {
            for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
                if (s.dl.stats[i].sample_count > 0) {
                    s.plot_visible[i] = char{1};
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None##dl_none")) {
            std::fill(s.plot_visible.begin(), s.plot_visible.end(),
                       char{0});
        }
        ImGui::Spacing();
        if (ImGui::BeginTable("##dl_stats", 5,
                               ImGuiTableFlags_Borders |
                                   ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Plot",
                                     ImGuiTableColumnFlags_WidthFixed,
                                     35.0f);
            ImGui::TableSetupColumn("Channel",
                                     ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Min",
                                     ImGuiTableColumnFlags_WidthFixed,
                                     60.0f);
            ImGui::TableSetupColumn("Max",
                                     ImGuiTableColumnFlags_WidthFixed,
                                     60.0f);
            ImGui::TableSetupColumn("Mean",
                                     ImGuiTableColumnFlags_WidthFixed,
                                     60.0f);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableSetColumnIndex(0);
                bool checked = s.plot_visible[i] != 0;
                if (ImGui::Checkbox("##show", &checked)) {
                    s.plot_visible[i] = checked ? char{1} : char{0};
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(s.dl.headers[i].c_str());
                auto const &st_row = s.dl.stats[i];
                ImGui::TableSetColumnIndex(2);
                if (st_row.sample_count > 0) {
                    ImGui::Text("%.2f", st_row.min);
                } else {
                    ImGui::TextDisabled("\xE2\x80\x94");
                }
                ImGui::TableSetColumnIndex(3);
                if (st_row.sample_count > 0) {
                    ImGui::Text("%.2f", st_row.max);
                } else {
                    ImGui::TextDisabled("\xE2\x80\x94");
                }
                ImGui::TableSetColumnIndex(4);
                if (st_row.sample_count > 0) {
                    ImGui::Text("%.2f", st_row.mean);
                } else {
                    ImGui::TextDisabled("\xE2\x80\x94");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##dl_split_right",
                           ImVec2(0.0f, 380.0f), false)) {
        // Count visible channels first — if none, render the empty-
        // state hint instead of an empty ImPlot frame.
        std::size_t visible_count = 0;
        for (char c : s.plot_visible) {
            if (c != 0) {
                ++visible_count;
            }
        }
        if (visible_count == 0) {
            ImGui::TextDisabled(
                "Tick the Plot column on the left to overlay channels.");
        } else if (ImPlot::BeginPlot("##dl_plot",
                                       ImVec2(-1.0f, -1.0f))) {
            ImPlot::SetupAxis(ImAxis_X1, "row");
            ImPlot::SetupAxis(ImAxis_Y1, "value");
            // Build an x axis of row indices once; reuse for every
            // overlaid channel.
            std::vector<float> xs;
            xs.reserve(s.dl.row_count);
            for (std::size_t i = 0; i < s.dl.row_count; ++i) {
                xs.push_back(static_cast<float>(i));
            }
            for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
                if (s.plot_visible[i] == 0) {
                    continue;
                }
                if (s.dl.stats[i].sample_count == 0) {
                    continue;
                }
                auto const &col = s.dl.data[i];
                ImPlot::PlotLine(s.dl.headers[i].c_str(), xs.data(),
                                  col.data(),
                                  static_cast<int>(col.size()));
            }
            ImPlot::EndPlot();
        }
    }
    ImGui::EndChild();

    ImGui::EndPopup();
}

} // namespace st::ui
