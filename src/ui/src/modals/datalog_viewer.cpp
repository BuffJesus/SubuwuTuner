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

#include "st/library/datalog_csv.hpp"
#include "st/library/datalog_session.hpp"

#include "app_state.hpp"
#include "panels/panels.hpp"
#include "persistence.hpp"
#include "widgets/widgets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    char channel_filter[128]{};
    int profile_index{0};
    int x_axis_column{-2}; // -2=auto, -1=sample, otherwise a CSV column
    int derived_lhs{0};
    int derived_rhs{0};
    int derived_operation{0};
    char derived_name[96]{"Calculated channel"};
    char derived_unit[32]{};
    std::string derived_error;
    std::vector<st::library::datalog_session::DerivedChannel> derived_channels;
    char session_notes[1024]{};
    int marker_row{0};
    int range_first{0};
    int range_last{0};
    int trace_row{0};
    int trace_rpm_column{0};
    int trace_load_column{0};
    char marker_label[96]{"Event"};
    std::vector<st::library::datalog_session::Marker> markers;
    std::string session_message;
    std::vector<st::library::datalog_session::SignalProfile> user_profiles;
    int user_profile_index{0};
    char user_profile_name[96]{"My signal profile"};
    bool user_profiles_loaded{false};
    bool loaded{false};
};

struct SignalProfile {
    char const *name;
    std::vector<std::string_view> keywords;
};

std::vector<SignalProfile> const &signal_profiles() {
    static std::vector<SignalProfile> const profiles{
        {"Fueling / MAF", {"rpm", "maf", "af correction", "af learning", "lambda", "afr", "fuel"}},
        {"Boost control", {"rpm", "boost", "manifold", "target", "wgdc", "throttle"}},
        {"Ignition / knock", {"rpm", "load", "dam", "fbkc", "flkc", "knock", "timing"}},
        {"AVCS", {"rpm", "load", "avcs", "cam", "vvt", "oil temp"}},
        {"Cold start", {"time", "rpm", "coolant", "ect", "iat", "lambda", "afr", "idle"}},
        {"FA24 swap", {"rpm", "load", "maf", "lambda", "fuel", "boost", "cam", "hpfp"}},
    };
    return profiles;
}

State &state() {
    static State s;
    return s;
}

void reset_state(State &s) {
    s.source_path.clear();
    s.dl = {};
    s.plot_visible.clear();
    s.error.clear();
    s.channel_filter[0] = '\0';
    s.profile_index = 0;
    s.x_axis_column = -2;
    s.derived_lhs = 0;
    s.derived_rhs = 0;
    s.derived_operation = 0;
    std::snprintf(s.derived_name, sizeof s.derived_name, "%s", "Calculated channel");
    s.derived_unit[0] = '\0';
    s.derived_error.clear();
    s.derived_channels.clear();
    s.session_notes[0] = '\0';
    s.marker_row = 0;
    s.range_first = 0;
    s.range_last = 0;
    s.trace_row = 0;
    s.trace_rpm_column = 0;
    s.trace_load_column = 0;
    std::snprintf(s.marker_label, sizeof s.marker_label, "%s", "Event");
    s.markers.clear();
    s.session_message.clear();
    s.loaded = false;
}

bool contains_case_insensitive(std::string const &text, char const *needle) {
    if (needle[0] == '\0') {
        return true;
    }
    std::string lowered_text = text;
    std::string lowered_needle = needle;
    auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::transform(lowered_text.begin(), lowered_text.end(), lowered_text.begin(), lower);
    std::transform(lowered_needle.begin(), lowered_needle.end(), lowered_needle.begin(), lower);
    return lowered_text.find(lowered_needle) != std::string::npos;
}

void apply_signal_profile(State &s) {
    std::fill(s.plot_visible.begin(), s.plot_visible.end(), char{0});
    auto const &profile = signal_profiles()[static_cast<std::size_t>(s.profile_index)];
    for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
        for (auto const keyword : profile.keywords) {
            std::string const needle{keyword};
            if (contains_case_insensitive(s.dl.headers[i], needle.c_str())) {
                s.plot_visible[i] = char{1};
                break;
            }
        }
    }
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
    s.range_last = static_cast<int>(s.dl.row_count);
    for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
        if (contains_case_insensitive(s.dl.headers[i], "rpm") ||
            contains_case_insensitive(s.dl.headers[i], "engine speed")) {
            s.trace_rpm_column = static_cast<int>(i);
        }
        if (contains_case_insensitive(s.dl.headers[i], "load")) {
            s.trace_load_column = static_cast<int>(i);
        }
    }
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

std::optional<std::filesystem::path> sample_log_path(AppState const &app_state) {
    if (!app_state.demo_project_path.has_value()) {
        return std::nullopt;
    }
    auto const candidate = app_state.demo_project_path->parent_path() / "demo-knock-log.csv";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec)) {
        return std::nullopt;
    }
    return candidate;
}

std::optional<std::size_t> find_header(State const &s, std::string const &header) {
    auto const it = std::find(s.dl.headers.begin(), s.dl.headers.end(), header);
    if (it == s.dl.headers.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(s.dl.headers.begin(), it));
}

st::library::datalog_session::Session capture_session(State const &s,
                                                       std::filesystem::path const &session_path) {
    namespace ds = st::library::datalog_session;
    ds::Session result;
    std::error_code ec;
    auto relative = std::filesystem::relative(std::filesystem::path{s.source_path},
                                              session_path.parent_path(), ec);
    result.source_path = ec ? s.source_path : relative.string();
    for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
        if (s.plot_visible[i] != 0) {
            result.visible_headers.push_back(s.dl.headers[i]);
        }
    }
    if (s.x_axis_column == -2) {
        result.x_axis = ds::kAutoAxis;
    } else if (s.x_axis_column == -1) {
        result.x_axis = ds::kSampleAxis;
    } else {
        result.x_axis = s.dl.headers[static_cast<std::size_t>(s.x_axis_column)];
    }
    result.notes = s.session_notes;
    result.range_first = static_cast<std::size_t>(s.range_first);
    result.range_last = static_cast<std::size_t>(s.range_last);
    result.markers = s.markers;
    result.derived_channels = s.derived_channels;
    return result;
}

void save_session(State &s) {
    nfdu8filteritem_t const filters[] = {{"SubuwuTuner log session", "stlog.toml,toml"}};
    NFD::UniquePathU8 path;
    auto const dialog = NFD::SaveDialog(path, filters, 1, nullptr, "log-session.stlog.toml");
    if (dialog == NFD_CANCEL) {
        return;
    }
    if (dialog != NFD_OKAY || path == nullptr) {
        s.session_message = "The session save dialog failed.";
        return;
    }
    std::filesystem::path const session_path{path.get()};
    auto encoded = st::library::datalog_session::serialize(capture_session(s, session_path));
    if (!encoded.has_value()) {
        s.session_message = encoded.error().to_string();
        return;
    }
    std::ofstream out{session_path, std::ios::binary | std::ios::trunc};
    if (!out || !(out << *encoded)) {
        s.session_message = "Couldn't write " + session_path.string() + ".";
        return;
    }
    s.session_message = "Saved session to " + session_path.string();
}

void export_selection(State &s) {
    std::vector<std::size_t> columns;
    for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
        if (s.plot_visible[i] != 0) {
            columns.push_back(i);
        }
    }
    if (columns.empty()) {
        s.session_message = "Select at least one plotted channel before exporting.";
        return;
    }
    nfdu8filteritem_t const filters[] = {{"CSV", "csv"}};
    NFD::UniquePathU8 path;
    auto const dialog = NFD::SaveDialog(path, filters, 1, nullptr, "log-selection.csv");
    if (dialog == NFD_CANCEL) {
        return;
    }
    if (dialog != NFD_OKAY || path == nullptr) {
        s.session_message = "The CSV export dialog failed.";
        return;
    }
    auto const csv = st::library::datalog_csv::export_csv(
        s.dl, columns, static_cast<std::size_t>(s.range_first),
        static_cast<std::size_t>(s.range_last));
    std::ofstream out{path.get(), std::ios::binary | std::ios::trunc};
    if (!out || !(out << csv)) {
        s.session_message = "Couldn't write the selected CSV range.";
        return;
    }
    s.session_message = "Exported plotted channels and selected rows.";
}

void export_findings(State &s) {
    nfdu8filteritem_t const filters[] = {{"Markdown findings", "md"}};
    NFD::UniquePathU8 path;
    auto const dialog = NFD::SaveDialog(path, filters, 1, nullptr, "log-findings.md");
    if (dialog == NFD_CANCEL) {
        return;
    }
    if (dialog != NFD_OKAY || path == nullptr) {
        s.session_message = "The findings export dialog failed.";
        return;
    }
    auto const stats = st::library::datalog_csv::range_stats(
        s.dl, static_cast<std::size_t>(s.range_first), static_cast<std::size_t>(s.range_last));
    std::ostringstream report;
    report << "# Log findings\n\nSource: `" << s.source_path << "`\n\nRows: " << s.range_first
           << " to " << s.range_last << " (exclusive)\n\n";
    if (s.session_notes[0] != '\0') {
        report << "## Notes\n\n" << s.session_notes << "\n\n";
    }
    report << "## Plotted-channel statistics\n\n| Channel | Unit | Samples | Min | Max | Mean |\n"
              "|---|---:|---:|---:|---:|---:|\n";
    for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
        if (s.plot_visible[i] == 0) {
            continue;
        }
        auto const &row = stats[i];
        report << "| " << s.dl.metadata[i].display_name << " | " << s.dl.metadata[i].unit
               << " | " << row.sample_count << " | ";
        if (row.sample_count == 0) {
            report << "- | - | - |\n";
        } else {
            report << row.min << " | " << row.max << " | " << row.mean << " |\n";
        }
    }
    if (!s.markers.empty()) {
        report << "\n## Events\n\n";
        for (auto const &marker : s.markers) {
            if (marker.row >= static_cast<std::size_t>(s.range_first) &&
                marker.row < static_cast<std::size_t>(s.range_last)) {
                report << "- Sample " << marker.row << ": " << marker.label << '\n';
            }
        }
    }
    std::ofstream out{path.get(), std::ios::binary | std::ios::trunc};
    if (!out || !(out << report.str())) {
        s.session_message = "Couldn't write the findings report.";
        return;
    }
    s.session_message = "Exported log findings report.";
}

void load_session(State &s) {
    namespace ds = st::library::datalog_session;
    nfdu8filteritem_t const filters[] = {{"SubuwuTuner log session", "stlog.toml,toml"}};
    NFD::UniquePathU8 path;
    auto const dialog = NFD::OpenDialog(path, filters, 1);
    if (dialog == NFD_CANCEL) {
        return;
    }
    if (dialog != NFD_OKAY || path == nullptr) {
        s.session_message = "The session open dialog failed.";
        return;
    }
    std::filesystem::path const session_path{path.get()};
    std::ifstream in{session_path, std::ios::binary};
    std::stringstream buffer;
    if (!in || !(buffer << in.rdbuf())) {
        s.session_message = "Couldn't read " + session_path.string() + ".";
        return;
    }
    auto decoded = ds::parse(buffer.str());
    if (!decoded.has_value()) {
        s.session_message = decoded.error().to_string();
        return;
    }
    std::filesystem::path source{decoded->source_path};
    if (source.is_relative()) {
        source = session_path.parent_path() / source;
    }
    load_csv(s, source.lexically_normal().string());
    if (!s.error.empty()) {
        return;
    }

    std::vector<std::string> warnings;
    for (auto const &recipe : decoded->derived_channels) {
        auto lhs = find_header(s, recipe.lhs_header);
        auto rhs = find_header(s, recipe.rhs_header);
        if (!lhs.has_value() || !rhs.has_value()) {
            warnings.push_back("Skipped '" + recipe.name + "' (source channel missing)");
            continue;
        }
        if (st::library::datalog_csv::append_derived_channel(
                s.dl, recipe.name, recipe.unit, *lhs, *rhs, recipe.operation)) {
            s.plot_visible.push_back(char{0});
            s.derived_channels.push_back(recipe);
        }
    }
    std::fill(s.plot_visible.begin(), s.plot_visible.end(), char{0});
    for (auto const &header : decoded->visible_headers) {
        if (auto index = find_header(s, header); index.has_value()) {
            s.plot_visible[*index] = char{1};
        } else {
            warnings.push_back("Plot channel missing: " + header);
        }
    }
    if (decoded->x_axis == ds::kAutoAxis) {
        s.x_axis_column = -2;
    } else if (decoded->x_axis == ds::kSampleAxis) {
        s.x_axis_column = -1;
    } else if (auto axis = find_header(s, decoded->x_axis); axis.has_value()) {
        s.x_axis_column = static_cast<int>(*axis);
    } else {
        s.x_axis_column = -2;
        warnings.push_back("Axis channel missing: " + decoded->x_axis);
    }
    std::snprintf(s.session_notes, sizeof s.session_notes, "%s", decoded->notes.c_str());
    s.range_first = static_cast<int>(std::min(decoded->range_first, s.dl.row_count));
    s.range_last = decoded->range_last == 0
                       ? static_cast<int>(s.dl.row_count)
                       : static_cast<int>(std::min(decoded->range_last, s.dl.row_count));
    s.markers.clear();
    for (auto const &marker : decoded->markers) {
        if (marker.row < s.dl.row_count) {
            s.markers.push_back(marker);
        } else {
            warnings.push_back("Marker outside the log was skipped: " + marker.label);
        }
    }
    s.session_message = warnings.empty() ? "Session loaded." : warnings.front();
    if (warnings.size() > 1) {
        s.session_message += " (and " + std::to_string(warnings.size() - 1) + " more warning(s))";
    }
}

std::size_t nearest_axis_cell(std::vector<double> const &axis, double value) {
    std::size_t best = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < axis.size(); ++i) {
        double const distance = std::abs(axis[i] - value);
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

void trace_sample_to_table(State &s, AppState &app_state) {
    if (!app_state.project.has_value() || !app_state.current_table_data.has_value() ||
        app_state.selected_table_id.empty()) {
        s.session_message = "Open a 2D RPM/load table before tracing a sample.";
        return;
    }
    auto const row = static_cast<std::size_t>(s.trace_row);
    auto const rpm_column = static_cast<std::size_t>(s.trace_rpm_column);
    auto const load_column = static_cast<std::size_t>(s.trace_load_column);
    if (row >= s.dl.row_count || rpm_column >= s.dl.data.size() ||
        load_column >= s.dl.data.size()) {
        s.session_message = "The trace sample or source channel is unavailable.";
        return;
    }
    float const rpm = s.dl.data[rpm_column][row];
    float const load = s.dl.data[load_column][row];
    if (std::isnan(rpm) || std::isnan(load)) {
        s.session_message = "That sample has no numeric RPM/load pair.";
        return;
    }
    auto const &data = *app_state.current_table_data;
    auto const *table = app_state.project->definition().find_table(app_state.selected_table_id);
    if (table == nullptr || data.axis_x.empty() || data.axis_y.empty() || data.values.empty()) {
        s.session_message = "The active table does not expose a 2D axis grid.";
        return;
    }
    bool const rpm_on_x = table->axis_x.has_value() &&
                          contains_case_insensitive(*table->axis_x, "rpm");
    bool const rpm_on_y = table->axis_y.has_value() &&
                          contains_case_insensitive(*table->axis_y, "rpm");
    bool const load_on_x = table->axis_x.has_value() &&
                           contains_case_insensitive(*table->axis_x, "load");
    bool const load_on_y = table->axis_y.has_value() &&
                           contains_case_insensitive(*table->axis_y, "load");
    if ((!rpm_on_x || !load_on_y) && (!rpm_on_y || !load_on_x)) {
        s.session_message = "The active table's axes are not identified as RPM and load.";
        return;
    }
    std::size_t const selected_row =
        nearest_axis_cell(data.axis_y, rpm_on_y ? static_cast<double>(rpm)
                                                : static_cast<double>(load));
    std::size_t const selected_column =
        nearest_axis_cell(data.axis_x, rpm_on_x ? static_cast<double>(rpm)
                                                : static_cast<double>(load));
    app_state.selection.click(selected_row, selected_column, false);
    app_state.view_mode = TableViewMode::Grid;
    s.session_message = "Mapped sample " + std::to_string(row) + " (RPM " +
                        std::to_string(static_cast<int>(rpm)) + ", load " +
                        std::to_string(load) + ") to table cell [" +
                        std::to_string(selected_row) + ", " + std::to_string(selected_column) +
                        "].";
    app_state.status_msg = s.session_message;
}

std::filesystem::path user_profiles_path() {
    return config_dir_root() / "log-profiles.toml";
}

void load_user_profiles(State &s) {
    if (s.user_profiles_loaded) {
        return;
    }
    s.user_profiles_loaded = true;
    std::ifstream in{user_profiles_path(), std::ios::binary};
    if (!in) {
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto decoded = st::library::datalog_session::parse_profiles(buffer.str());
    if (decoded.has_value()) {
        s.user_profiles = std::move(*decoded);
    } else {
        s.session_message = "Saved signal profiles could not be loaded: " +
                            decoded.error().to_string();
    }
}

void save_user_profiles(State &s) {
    auto encoded = st::library::datalog_session::serialize_profiles(s.user_profiles);
    if (!encoded.has_value()) {
        s.session_message = encoded.error().to_string();
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(user_profiles_path().parent_path(), ec);
    std::ofstream out{user_profiles_path(), std::ios::binary | std::ios::trunc};
    if (!out || !(out << *encoded)) {
        s.session_message = "Couldn't save signal profiles.";
        return;
    }
    s.session_message = "Saved signal profiles.";
}

void apply_user_profile(State &s) {
    if (s.user_profile_index < 0 ||
        static_cast<std::size_t>(s.user_profile_index) >= s.user_profiles.size()) {
        return;
    }
    std::fill(s.plot_visible.begin(), s.plot_visible.end(), char{0});
    for (auto const &header : s.user_profiles[static_cast<std::size_t>(s.user_profile_index)]
                                  .visible_headers) {
        if (auto index = find_header(s, header); index.has_value()) {
            s.plot_visible[*index] = char{1};
        }
    }
}

} // namespace

void render_log_explorer_panel(AppState &app_state) {
    if (!app_state.show_log_explorer_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Log Explorer", &app_state.show_log_explorer_panel)) {
        ImGui::End();
        return;
    }

    auto &s = state();
    load_user_profiles(s);
    ImGui::TextWrapped(
        "Open a CSV datalog, search its signals, compare channels, and inspect import quality. "
        "Decompress .csv.gz files first (gunzip / 7-Zip / `python -m gzip`).");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Open CSV\xE2\x80\xA6##dl_open")) {
        pick_and_load(s);
    }
    if (auto const sample = sample_log_path(app_state); sample.has_value()) {
        ImGui::SameLine();
        if (ImGui::Button("Load sample log##dl_sample")) {
            load_csv(s, sample->string());
            s.session_message = "Loaded the bundled synthetic knock log.";
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open a hardware-free synthetic pull so you can explore this "
                              "workspace immediately.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open session\xE2\x80\xA6##dl_session_open")) {
        load_session(s);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.loaded || !s.error.empty());
    if (ImGui::Button("Save session\xE2\x80\xA6##dl_session_save")) {
        save_session(s);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear session##dl_clear")) {
        reset_state(s);
    }
    if (!s.session_message.empty()) {
        ImGui::TextWrapped("%s", s.session_message.c_str());
    }
    ImGui::Spacing();

    if (!s.loaded) {
        ImGui::TextDisabled("Open one of your CSV logs or load the bundled sample to explore "
                            "plots, ranges, markers, and findings without hardware.");
        ImGui::End();
        return;
    }
    if (!s.error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "Error: %s", s.error.c_str());
        ImGui::End();
        return;
    }

    bool const panel_keyboard_focus =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive();
    if (panel_keyboard_focus && s.dl.row_count != 0) {
        int const last_row = static_cast<int>(s.dl.row_count - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
            s.trace_row = std::max(0, s.trace_row - 10);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
            s.trace_row = std::min(last_row, s.trace_row + 10);
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
            s.markers.push_back({static_cast<std::size_t>(s.trace_row), "Keyboard marker"});
            s.session_message = "Added a marker at sample " + std::to_string(s.trace_row) + ".";
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            trace_sample_to_table(s, app_state);
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            s.range_first = s.trace_row;
            s.range_last = std::max(s.range_last, s.range_first + 1);
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            s.range_last = s.trace_row + 1;
            s.range_first = std::min(s.range_first, s.range_last - 1);
        }
    }

    ImGui::Text("Source:   %s", s.source_path.c_str());
    ImGui::Text("Channels: %zu  Rows: %zu", s.dl.headers.size(), s.dl.row_count);
    if (s.dl.row_count == 0) {
        ImGui::TextColored(chip_fg_caution(),
                           "This CSV has channel headers but no sample rows to analyze.");
    }
    text_subtle("Keyboard: Page Up/Down moves the trace sample; Ctrl+Home/End sets the range; "
                "Ctrl+M marks it; Ctrl+G maps it.");
    if (s.dl.malformed_row_count != 0 || s.dl.invalid_cell_count != 0) {
        ImGui::TextColored(chip_fg_caution(),
                           "Import warning: %zu irregular row%s, %zu non-numeric cell%s",
                           s.dl.malformed_row_count, s.dl.malformed_row_count == 1 ? "" : "s",
                           s.dl.invalid_cell_count, s.dl.invalid_cell_count == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Irregular rows were padded or truncated. Non-numeric values are "
                              "shown as gaps and excluded from statistics.");
        }
    }
    ImGui::Spacing();

    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputInt("First row##dl_range_first", &s.range_first)) {
        s.range_first = std::clamp(s.range_first, 0, static_cast<int>(s.dl.row_count));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputInt("Last row (exclusive)##dl_range_last", &s.range_last)) {
        s.range_last = std::clamp(s.range_last, 0, static_cast<int>(s.dl.row_count));
    }
    if (s.range_last < s.range_first) {
        std::swap(s.range_first, s.range_last);
    }
    ImGui::SameLine();
    if (ImGui::Button("Full range##dl_range_full")) {
        s.range_first = 0;
        s.range_last = static_cast<int>(s.dl.row_count);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export plotted range\xE2\x80\xA6##dl_range_export")) {
        export_selection(s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export findings\xE2\x80\xA6##dl_findings_export")) {
        export_findings(s);
    }
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Map-cell trace")) {
        auto trace_channel_combo = [&s](char const *id, int &selection) {
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::BeginCombo(id,
                                  s.dl.headers[static_cast<std::size_t>(selection)].c_str())) {
                for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
                    if (s.dl.stats[i].sample_count == 0) {
                        continue;
                    }
                    bool const selected = selection == static_cast<int>(i);
                    if (ImGui::Selectable(s.dl.headers[i].c_str(), selected)) {
                        selection = static_cast<int>(i);
                    }
                }
                ImGui::EndCombo();
            }
        };
        ImGui::TextUnformatted("RPM channel");
        ImGui::SameLine();
        trace_channel_combo("##dl_trace_rpm", s.trace_rpm_column);
        ImGui::SameLine();
        ImGui::TextUnformatted("Load channel");
        ImGui::SameLine();
        trace_channel_combo("##dl_trace_load", s.trace_load_column);
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputInt("Sample row##dl_trace_row", &s.trace_row)) {
            s.trace_row = std::clamp(
                s.trace_row, 0, static_cast<int>(s.dl.row_count == 0 ? 0 : s.dl.row_count - 1));
        }
        ImGui::SameLine();
        if (ImGui::Button("Select nearest cell in active table##dl_trace_apply")) {
            trace_sample_to_table(s, app_state);
        }
        ImGui::TextDisabled(
            "Uses the active table's named RPM/load axes and switches its view to Grid.");
    }
    ImGui::Spacing();

    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##dl_profile",
                          signal_profiles()[static_cast<std::size_t>(s.profile_index)].name)) {
        for (std::size_t i = 0; i < signal_profiles().size(); ++i) {
            bool const selected = static_cast<int>(i) == s.profile_index;
            if (ImGui::Selectable(signal_profiles()[i].name, selected)) {
                s.profile_index = static_cast<int>(i);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply signal profile##dl_profile_apply")) {
        apply_signal_profile(s);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190.0f);
    char const *x_axis_label =
        s.x_axis_column == -2
            ? "Auto time axis"
            : (s.x_axis_column == -1
                   ? "Sample number"
                   : s.dl.headers[static_cast<std::size_t>(s.x_axis_column)].c_str());
    if (ImGui::BeginCombo("##dl_x_axis", x_axis_label)) {
        if (ImGui::Selectable("Auto time axis", s.x_axis_column == -2)) {
            s.x_axis_column = -2;
        }
        if (ImGui::Selectable("Sample number", s.x_axis_column == -1)) {
            s.x_axis_column = -1;
        }
        for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
            if (s.dl.stats[i].sample_count == 0) {
                continue;
            }
            if (ImGui::Selectable(s.dl.headers[i].c_str(),
                                  s.x_axis_column == static_cast<int>(i))) {
                s.x_axis_column = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Choose the shared horizontal axis. Auto uses a recognized timestamp "
                          "column and otherwise falls back to sample number.");
    }
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Saved signal profiles")) {
        if (!s.user_profiles.empty()) {
            s.user_profile_index = std::clamp(
                s.user_profile_index, 0, static_cast<int>(s.user_profiles.size() - 1));
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo(
                    "Saved profile##dl_user_profile",
                    s.user_profiles[static_cast<std::size_t>(s.user_profile_index)].name.c_str())) {
                for (std::size_t i = 0; i < s.user_profiles.size(); ++i) {
                    bool const selected = s.user_profile_index == static_cast<int>(i);
                    if (ImGui::Selectable(s.user_profiles[i].name.c_str(), selected)) {
                        s.user_profile_index = static_cast<int>(i);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply##dl_user_profile_apply")) {
                apply_user_profile(s);
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete##dl_user_profile_delete")) {
                s.user_profiles.erase(s.user_profiles.begin() + s.user_profile_index);
                s.user_profile_index = std::max(0, s.user_profile_index - 1);
                save_user_profiles(s);
            }
        } else {
            ImGui::TextDisabled("No user-authored profiles saved yet.");
        }
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("Profile name##dl_user_profile_name", s.user_profile_name,
                         sizeof s.user_profile_name);
        ImGui::SameLine();
        if (ImGui::Button("Save current plotted channels##dl_user_profile_save")) {
            if (s.user_profile_name[0] == '\0') {
                s.session_message = "Give the signal profile a name.";
            } else {
                st::library::datalog_session::SignalProfile profile;
                profile.name = s.user_profile_name;
                for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
                    if (s.plot_visible[i] != 0) {
                        profile.visible_headers.push_back(s.dl.headers[i]);
                    }
                }
                auto const existing = std::find_if(
                    s.user_profiles.begin(), s.user_profiles.end(),
                    [&](auto const &candidate) { return candidate.name == profile.name; });
                if (existing == s.user_profiles.end()) {
                    s.user_profiles.push_back(std::move(profile));
                    s.user_profile_index = static_cast<int>(s.user_profiles.size() - 1);
                } else {
                    *existing = std::move(profile);
                    s.user_profile_index =
                        static_cast<int>(std::distance(s.user_profiles.begin(), existing));
                }
                save_user_profiles(s);
            }
        }
    }
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Session notes and markers")) {
        ImGui::TextUnformatted("Session notes");
        ImGui::InputTextMultiline("##dl_session_notes", s.session_notes, sizeof s.session_notes,
                                  ImVec2(-1.0f, 70.0f));
        ImGui::TextUnformatted("Marker sample");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("##dl_marker_row", &s.marker_row);
        s.marker_row = std::clamp(s.marker_row, 0,
                                  static_cast<int>(s.dl.row_count == 0 ? 0 : s.dl.row_count - 1));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##dl_marker_label", "Marker label", s.marker_label,
                                 sizeof s.marker_label);
        ImGui::SameLine();
        if (ImGui::Button("Add marker##dl_marker_add") && s.dl.row_count != 0) {
            s.markers.push_back(
                {static_cast<std::size_t>(s.marker_row), std::string{s.marker_label}});
        }
        for (std::size_t i = 0; i < s.markers.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("Sample %zu: %s", s.markers[i].row, s.markers[i].label.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                s.markers.erase(s.markers.begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Derived channel")) {
        auto channel_combo = [&s](char const *id, int &selection) {
            ImGui::SetNextItemWidth(190.0f);
            char const *label = s.dl.headers[static_cast<std::size_t>(selection)].c_str();
            if (ImGui::BeginCombo(id, label)) {
                for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
                    bool const selected = selection == static_cast<int>(i);
                    if (ImGui::Selectable(s.dl.headers[i].c_str(), selected)) {
                        selection = static_cast<int>(i);
                    }
                }
                ImGui::EndCombo();
            }
        };
        ImGui::TextUnformatted("Source A");
        ImGui::SameLine();
        channel_combo("##dl_derived_lhs", s.derived_lhs);
        ImGui::SameLine();
        static char const *const operations[]{"A - B", "A / B", "(A - B) / B %"};
        ImGui::SetNextItemWidth(135.0f);
        ImGui::Combo("##dl_derived_op", &s.derived_operation, operations, 3);
        ImGui::SameLine();
        ImGui::TextUnformatted("Source B");
        ImGui::SameLine();
        channel_combo("##dl_derived_rhs", s.derived_rhs);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##dl_derived_name", "Channel name", s.derived_name,
                                 sizeof s.derived_name);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputTextWithHint("##dl_derived_unit", "Unit", s.derived_unit,
                                 sizeof s.derived_unit);
        ImGui::SameLine();
        if (ImGui::Button("Add calculated channel##dl_derived_add")) {
            static constexpr st::library::datalog_csv::DerivedOperation kOperations[]{
                st::library::datalog_csv::DerivedOperation::Subtract,
                st::library::datalog_csv::DerivedOperation::Ratio,
                st::library::datalog_csv::DerivedOperation::PercentError,
            };
            if (s.derived_name[0] == '\0') {
                s.derived_error = "Give the calculated channel a name.";
            } else {
                auto const recipe = st::library::datalog_session::DerivedChannel{
                    s.derived_name, s.derived_unit,
                    s.dl.headers[static_cast<std::size_t>(s.derived_lhs)],
                    s.dl.headers[static_cast<std::size_t>(s.derived_rhs)],
                    kOperations[static_cast<std::size_t>(s.derived_operation)]};
                auto const added = st::library::datalog_csv::append_derived_channel(
                    s.dl, s.derived_name, s.derived_unit, static_cast<std::size_t>(s.derived_lhs),
                    static_cast<std::size_t>(s.derived_rhs),
                    kOperations[static_cast<std::size_t>(s.derived_operation)]);
                if (added.has_value()) {
                    s.plot_visible.push_back(char{1});
                    s.derived_channels.push_back(recipe);
                    s.derived_error.clear();
                } else {
                    s.derived_error = "One of the selected source channels is unavailable.";
                }
            }
        }
        if (!s.derived_error.empty()) {
            ImGui::TextColored(chip_fg_caution(), "%s", s.derived_error.c_str());
        }
    }
    ImGui::Spacing();

    auto const selected_stats = st::library::datalog_csv::range_stats(
        s.dl, static_cast<std::size_t>(s.range_first), static_cast<std::size_t>(s.range_last));

    // Two-pane: stats table on the left, plot on the right. The
    // table is the selection surface — clicking a row swaps the
    // plotted channel.
    if (ImGui::BeginChild("##dl_split_left", ImVec2(420.0f, -1.0f), true)) {
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
            std::fill(s.plot_visible.begin(), s.plot_visible.end(), char{0});
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##dl_filter", "Search channels...", s.channel_filter,
                                 sizeof s.channel_filter);
        ImGui::Spacing();
        if (ImGui::BeginTable("##dl_stats", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed, 35.0f);
            ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, 45.0f);
            ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Mean", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < s.dl.headers.size(); ++i) {
                if (!contains_case_insensitive(s.dl.headers[i], s.channel_filter)) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableSetColumnIndex(0);
                bool checked = s.plot_visible[i] != 0;
                if (ImGui::Checkbox("##show", &checked)) {
                    s.plot_visible[i] = checked ? char{1} : char{0};
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(s.dl.metadata[i].display_name.c_str());
                auto const &st_row = selected_stats[i];
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", s.dl.metadata[i].unit.empty()
                                              ? "\xE2\x80\x94"
                                              : s.dl.metadata[i].unit.c_str());
                ImGui::TableSetColumnIndex(3);
                if (st_row.sample_count > 0) {
                    ImGui::Text("%.2f", st_row.min);
                } else {
                    ImGui::TextDisabled("\xE2\x80\x94");
                }
                ImGui::TableSetColumnIndex(4);
                if (st_row.sample_count > 0) {
                    ImGui::Text("%.2f", st_row.max);
                } else {
                    ImGui::TextDisabled("\xE2\x80\x94");
                }
                ImGui::TableSetColumnIndex(5);
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
    if (ImGui::BeginChild("##dl_split_right", ImVec2(0.0f, -1.0f), false)) {
        // Count visible channels first — if none, render the empty-
        // state hint instead of an empty ImPlot frame.
        std::size_t visible_count = 0;
        for (char c : s.plot_visible) {
            if (c != 0) {
                ++visible_count;
            }
        }
        if (visible_count == 0) {
            ImGui::TextDisabled("Tick the Plot column on the left to overlay channels.");
        } else if (ImPlot::BeginPlot("##dl_plot", ImVec2(-1.0f, -1.0f))) {
            std::optional<std::size_t> x_column;
            if (s.x_axis_column >= 0) {
                x_column = static_cast<std::size_t>(s.x_axis_column);
            } else if (s.x_axis_column == -2) {
                x_column = s.dl.time_column;
            }
            bool const column_axis = x_column.has_value() && *x_column < s.dl.data.size() &&
                                     s.dl.stats[*x_column].sample_count > 0;
            ImPlot::SetupAxis(ImAxis_X1, column_axis ? s.dl.headers[*x_column].c_str() : "sample");
            ImPlot::SetupAxis(ImAxis_Y1, "value");
            std::vector<float> xs;
            if (column_axis) {
                xs = s.dl.data[*x_column];
            } else {
                xs.reserve(s.dl.row_count);
                for (std::size_t i = 0; i < s.dl.row_count; ++i) {
                    xs.push_back(static_cast<float>(i));
                }
            }
            for (std::size_t i = 0; i < s.plot_visible.size(); ++i) {
                if (s.plot_visible[i] == 0) {
                    continue;
                }
                if (s.dl.stats[i].sample_count == 0) {
                    continue;
                }
                auto const &col = s.dl.data[i];
                auto const first = std::min(static_cast<std::size_t>(s.range_first), col.size());
                auto const last = std::min(static_cast<std::size_t>(s.range_last), col.size());
                if (last > first) {
                    ImPlot::PlotLine(s.dl.headers[i].c_str(), xs.data() + first,
                                     col.data() + first, static_cast<int>(last - first));
                }
            }
            if (!s.markers.empty()) {
                std::vector<float> marker_positions;
                marker_positions.reserve(s.markers.size());
                for (auto const &marker : s.markers) {
                    if (marker.row < xs.size() && marker.row >= static_cast<std::size_t>(s.range_first) &&
                        marker.row < static_cast<std::size_t>(s.range_last)) {
                        marker_positions.push_back(xs[marker.row]);
                    }
                }
                if (!marker_positions.empty()) {
                    ImPlot::PlotInfLines("Events", marker_positions.data(),
                                         static_cast<int>(marker_positions.size()));
                }
            }
            ImPlot::EndPlot();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace st::ui
