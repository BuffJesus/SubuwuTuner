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

#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

namespace {

struct ChannelStats {
    double min{std::numeric_limits<double>::quiet_NaN()};
    double max{std::numeric_limits<double>::quiet_NaN()};
    double mean{std::numeric_limits<double>::quiet_NaN()};
    std::size_t sample_count{0};
};

struct State {
    std::string source_path;
    std::vector<std::string> headers;
    // Column-major storage: data[col][row]. Easier for per-channel
    // iteration (stats, plot) than row-major.
    std::vector<std::vector<float>> data;
    std::vector<ChannelStats> stats;
    // Per-channel "show in plot" flag. ImPlot can cycle colors
    // through several overlaid series without trouble, so multi-
    // channel overlay falls out naturally — checkboxes in the stats
    // table swap channels in/out of the plot. Use std::vector<char>
    // rather than std::vector<bool> so the per-row bind in the
    // Checkbox call works without the bool-bitfield workaround.
    std::vector<char> plot_visible;
    std::size_t row_count{0};
    std::string error;
    bool loaded{false};
};

State &state() {
    static State s;
    return s;
}

void reset_state(State &s) {
    s.source_path.clear();
    s.headers.clear();
    s.data.clear();
    s.stats.clear();
    s.plot_visible.clear();
    s.row_count = 0;
    s.error.clear();
    s.loaded = false;
}

void compute_stats(State &s) {
    s.stats.assign(s.headers.size(), ChannelStats{});
    for (std::size_t col = 0; col < s.data.size(); ++col) {
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        double sum = 0.0;
        std::size_t n = 0;
        for (float v : s.data[col]) {
            if (std::isnan(v)) {
                continue;
            }
            double const d = static_cast<double>(v);
            if (d < min_v) {
                min_v = d;
            }
            if (d > max_v) {
                max_v = d;
            }
            sum += d;
            ++n;
        }
        if (n > 0) {
            s.stats[col].min = min_v;
            s.stats[col].max = max_v;
            s.stats[col].mean = sum / static_cast<double>(n);
            s.stats[col].sample_count = n;
        }
    }
}

[[nodiscard]] std::vector<std::string_view>
split_csv_line(std::string_view line) {
    std::vector<std::string_view> cells;
    cells.reserve(16);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            std::size_t end = i;
            // Trim trailing CR for CRLF endings.
            std::string_view const cell = line.substr(start, end - start);
            std::string_view trimmed = cell;
            while (!trimmed.empty() && trimmed.back() == '\r') {
                trimmed.remove_suffix(1);
            }
            cells.push_back(trimmed);
            start = i + 1;
        }
    }
    return cells;
}

[[nodiscard]] float parse_cell(std::string_view cell) {
    if (cell.empty()) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    // strtod needs a null-terminated string; use a small stack buffer
    // for the common case + heap fallback for unusually long cells.
    char stack_buf[64];
    char *buf = stack_buf;
    std::string heap_buf;
    if (cell.size() >= sizeof stack_buf) {
        heap_buf.assign(cell.begin(), cell.end());
        buf = heap_buf.data();
    } else {
        std::memcpy(stack_buf, cell.data(), cell.size());
        stack_buf[cell.size()] = '\0';
    }
    char *end = nullptr;
    double const v = std::strtod(buf, &end);
    if (end == buf) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return static_cast<float>(v);
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
    // Split into lines.
    std::vector<std::string_view> lines;
    lines.reserve(1024);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(
                std::string_view{text}.substr(start, i - start));
            start = i + 1;
        }
    }
    // Drop trailing empty line (typical CSV final newline).
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.empty()) {
        s.error = "File is empty.";
        s.loaded = true;
        return;
    }
    // First line = headers.
    auto const header_cells = split_csv_line(lines[0]);
    s.headers.reserve(header_cells.size());
    for (auto const &cell : header_cells) {
        s.headers.emplace_back(cell);
    }
    s.data.assign(s.headers.size(), {});
    for (auto &col : s.data) {
        col.reserve(lines.size() - 1);
    }
    // Remaining lines = rows.
    for (std::size_t li = 1; li < lines.size(); ++li) {
        auto const cells = split_csv_line(lines[li]);
        for (std::size_t col = 0; col < s.data.size(); ++col) {
            float const v = col < cells.size()
                                  ? parse_cell(cells[col])
                                  : std::numeric_limits<float>::quiet_NaN();
            s.data[col].push_back(v);
        }
    }
    if (!s.data.empty()) {
        s.row_count = s.data[0].size();
    }
    compute_stats(s);
    // Default-plot the first non-empty channel so the user sees
    // something immediately. Multi-select via the checkboxes below.
    s.plot_visible.assign(s.headers.size(), char{0});
    for (std::size_t i = 0; i < s.stats.size(); ++i) {
        if (s.stats[i].sample_count > 0) {
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
    ImGui::Text("Channels: %zu  Rows: %zu", s.headers.size(), s.row_count);
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
                if (s.stats[i].sample_count > 0) {
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
            for (std::size_t i = 0; i < s.headers.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableSetColumnIndex(0);
                bool checked = s.plot_visible[i] != 0;
                if (ImGui::Checkbox("##show", &checked)) {
                    s.plot_visible[i] = checked ? char{1} : char{0};
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(s.headers[i].c_str());
                auto const &st_row = s.stats[i];
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
            xs.reserve(s.row_count);
            for (std::size_t i = 0; i < s.row_count; ++i) {
                xs.push_back(static_cast<float>(i));
            }
            for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
                if (s.plot_visible[i] == 0) {
                    continue;
                }
                if (s.stats[i].sample_count == 0) {
                    continue;
                }
                auto const &col = s.data[i];
                ImPlot::PlotLine(s.headers[i].c_str(), xs.data(),
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
