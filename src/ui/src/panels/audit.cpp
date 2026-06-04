// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Audit log viewer panel (analyst Issue #8). Reads the open project's
// `<project>/audit.log` via st::audit::read_all and renders the entries
// in a sortable, filterable table. v1 is read-only — entries are
// produced by the CLI today (and module-level auto-subscribers across
// transport / ecu / flash / log are the follow-up that lights up the
// "no caller code needed" promise from the audit module's header).
//
// Layout:
//   - Toolbar: path display, Refresh, Newest-first toggle, filter input
//   - Status: "<N> entries, <K> with bad checksum" or load error
//   - Table: timestamp | kind | source | description | checksum chip
//
// Tampering is surfaced explicitly per the audit module's design — the
// CRC32 is checked on read and entries with bad checksums get a red
// chip rather than being silently dropped, so a corrupted line in the
// middle of the log is visible instead of confusing the reader with a
// "missing event" mystery.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "actions.hpp" // jump_to_table
#include "st/audit.hpp"

#include <imgui.h>
#include <implot.h>
#include <nfd.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace st::ui {

namespace {

// Format a UTC ISO-8601 second-precision string from nanoseconds
// since epoch. The audit log stores ns; the UI doesn't need ns
// resolution to be useful. Buffer must be at least 25 bytes.
void format_iso_utc(std::int64_t ns, char *buf, std::size_t buf_size) {
    if (buf_size == 0) {
        return;
    }
    std::time_t const secs = ns / 1'000'000'000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    std::strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// Case-insensitive substring search. Used by the filter — `needle`
// empty matches everything, so the no-filter path stays trivial.
bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    auto const lo = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lo(hay[i + j]) != lo(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// Composite key for the pin sidecar. audit.log entries are uniquely
// identifiable by (timestamp_ns, crc32_on_disk) — the timestamp is
// nanosecond-resolution and the CRC32 protects against same-ns
// duplicate insertions surviving as the same pin target.
std::string audit_pin_key(st::audit::Entry const &e) {
    std::string out;
    out.reserve(28);
    out.append(std::to_string(e.timestamp_ns));
    out.push_back(':');
    out.append(std::to_string(e.checksum_on_disk));
    return out;
}

void load_audit_pinned(AppState &state) {
    state.audit_pinned_keys.clear();
    if (!state.project.has_value()) {
        return;
    }
    auto const sidecar = state.project->dir() / "audit.pinned";
    std::ifstream in{sidecar};
    if (!in) {
        return; // absent sidecar = no pins, not an error
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        state.audit_pinned_keys.insert(std::move(line));
    }
}

void save_audit_pinned(AppState const &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto const sidecar = state.project->dir() / "audit.pinned";
    std::ofstream out{sidecar, std::ios::trunc};
    if (!out) {
        return; // best-effort — pins survive at runtime even if disk fails
    }
    for (auto const &k : state.audit_pinned_keys) {
        out << k << '\n';
    }
}

void load_audit_log(AppState &state) {
    state.audit_entries.clear();
    state.audit_error_msg.clear();
    state.audit_loaded = false;
    state.audit_log_mtime = std::filesystem::file_time_type{};
    if (!state.project.has_value()) {
        state.audit_error_msg = "No project loaded — open a .stune project to view its audit log.";
        return;
    }
    auto const log_path = state.project->dir() / "audit.log";
    if (!std::filesystem::exists(log_path)) {
        state.audit_loaded = true; // genuinely empty, not an error
        load_audit_pinned(state);
        return;
    }
    auto r = st::audit::read_all(log_path);
    if (!r.has_value()) {
        state.audit_error_msg = "Read failed: " + r.error().to_string();
        return;
    }
    state.audit_entries = std::move(*r);
    state.audit_loaded = true;
    load_audit_pinned(state);
    std::error_code ec;
    state.audit_log_mtime = std::filesystem::last_write_time(log_path, ec);
    if (ec) {
        state.audit_log_mtime = std::filesystem::file_time_type{};
    }
}

// Poll the audit log's mtime against the cached value; reload when it
// changed. Cheap (single stat per frame). Triggered each frame the
// panel is visible so events appended elsewhere in the GUI surface
// without the user having to click Refresh.
void maybe_auto_refresh_audit(AppState &state) {
    if (!state.audit_loaded || !state.project.has_value()) {
        return;
    }
    auto const log_path = state.project->dir() / "audit.log";
    std::error_code ec;
    if (!std::filesystem::exists(log_path, ec) || ec) {
        return;
    }
    auto const mtime_now = std::filesystem::last_write_time(log_path, ec);
    if (ec) {
        return;
    }
    if (mtime_now != state.audit_log_mtime) {
        load_audit_log(state);
    }
}

} // namespace

void render_audit_panel(AppState &state) {
    if (!state.show_audit_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Audit", &state.show_audit_panel)) {
        ImGui::End();
        return;
    }

    // First-open: auto-load if a project is open. Subsequent re-shows
    // keep the cached entries; an mtime poll below picks up changes
    // appended elsewhere in the GUI without forcing a full reload on
    // every frame.
    if (!state.audit_loaded && state.audit_error_msg.empty()) {
        load_audit_log(state);
    } else {
        maybe_auto_refresh_audit(state);
    }

    // ---- Header / toolbar -------------------------------------------
    if (state.project.has_value()) {
        auto const path = state.project->dir() / "audit.log";
        text_subtle("Log: %s", path.string().c_str());
    } else {
        text_subtle("Log: (no project loaded)");
    }

    if (ImGui::Button("\xEE\x9D\xB3  Refresh")) { // E773 Refresh
        load_audit_log(state);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-read the project's audit.log from disk.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.project.has_value());
    if (ImGui::Button("Open log location##audit_reveal")) {
        // Open the project dir in the OS file explorer, selecting the
        // audit.log file when possible. Windows: explorer /select,
        // <path>. macOS: open -R. Linux: xdg-open the parent.
        if (state.project.has_value()) {
            auto const log_path = state.project->dir() / "audit.log";
            std::string cmd;
#if defined(_WIN32)
            cmd = "explorer /select,\"" + log_path.string() + "\"";
#elif defined(__APPLE__)
            cmd = "open -R \"" + log_path.string() + "\"";
#else
            cmd = "xdg-open \"" + state.project->dir().string() + "\"";
#endif
            // std::system inherits the parent's stdio + blocks until
            // the shell returns. Explorer/Finder/xdg-open spawn and
            // detach quickly, so this is fine for a one-shot reveal.
            (void)std::system(cmd.c_str());
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Reveal <project>/audit.log in the OS file\n"
                          "browser. Useful for archiving / mailing /\n"
                          "tail-following from a terminal.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state.audit_entries.empty());
    if (ImGui::Button("Export NDJSON…##audit_export")) {
        NFD::UniquePath out;
        nfdfilteritem_t const filters[] = {{"NDJSON", "ndjson"}, {"Log", "log"}};
        nfdresult_t const r =
            NFD::SaveDialog(out, filters, 2, nullptr, "audit-export.ndjson");
        if (r == NFD_OKAY) {
            std::filesystem::path const target{out.get()};
            std::ofstream fh{target, std::ios::binary};
            if (!fh) {
                state.audit_error_msg = "Export: cannot open " + target.string();
            } else {
                // Re-serialize each cached entry — keeps the wire shape
                // identical to st::audit::AuditLog::append output, and
                // also recomputes the CRC32 so a tampered on-disk line
                // gets exported with a fresh checksum (or rejected up
                // front if checksum_valid is false; we still write it
                // so the user has the same data they were viewing).
                for (auto const &e : state.audit_entries) {
                    fh << st::audit::serialize_entry(e) << '\n';
                }
                if (!fh) {
                    state.audit_error_msg = "Export: write failed";
                } else {
                    enqueue_toast(state, ToastKind::Success,
                                  "Exported " + std::to_string(state.audit_entries.size()) +
                                      " entries to " + target.string());
                }
            }
        } else if (r == NFD_ERROR) {
            state.audit_error_msg = std::string{"Export dialog: "} + NFD::GetError();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Write every cached entry to a .ndjson file (one\n"
                          "JSON object per line). Useful for sharing the\n"
                          "audit timeline with support / e-tuner / CI.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Newest first", &state.audit_newest_first);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Sort by timestamp descending (default) or ascending.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##audit_filter", "Filter (kind/source/description)…",
                             state.audit_filter, sizeof state.audit_filter);
    ImGui::SameLine();
    if (ImGui::Button("\xEE\x9D\x84  Clear##audit_filter_clear")) { // E704 Clear
        state.audit_filter[0] = '\0';
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear the filter input.");
    }
    ImGui::SameLine();
    // Pinned-only toggle. Surfaces just the entries the user starred
    // for review — composes with the text + chip + range filters
    // (pinned-only applies AFTER them).
    {
        bool const active = state.audit_show_pinned_only;
        if (active) {
            push_primary_button_colors();
        }
        // ★ = pinned-only active; ☆ = show all. Same glyph vocabulary
        // as the per-row toggle.
        char const *label = active ? "\xE2\x98\x85  Pinned"
                                   : "\xE2\x98\x86  Pinned";
        if (ImGui::Button(label)) {
            state.audit_show_pinned_only = !state.audit_show_pinned_only;
        }
        if (active) {
            pop_primary_button_colors();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(active
                                  ? "Showing %zu pinned entries. Click to show all."
                                  : "Show only pinned entries (%zu pinned across "
                                    "this project).",
                              state.audit_pinned_keys.size());
        }
    }

    // Kind-filter chips. One per kind actually present in the loaded
    // entries — keeps the toolbar from showing 18 unused buttons on a
    // 3-event log. Click a chip → it becomes the filter string;
    // re-click to revert. The same chip → text-input round-trip is
    // intentional: the text filter already does substring matching
    // against kind names, so chips are a discoverability layer on top.
    if (!state.audit_entries.empty()) {
        std::vector<std::string_view> kinds_seen;
        for (auto const &e : state.audit_entries) {
            auto const kn = st::audit::kind_name(e.kind);
            bool dup = false;
            for (auto k : kinds_seen) {
                if (k == kn) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                kinds_seen.push_back(kn);
            }
        }
        std::sort(kinds_seen.begin(), kinds_seen.end());
        for (auto kn : kinds_seen) {
            std::string const kn_str{kn};
            bool const active = (std::string_view{state.audit_filter} == kn);
            if (active) {
                push_primary_button_colors();
            }
            ImGui::PushID(kn_str.c_str());
            if (ImGui::SmallButton(kn_str.c_str())) {
                if (active) {
                    state.audit_filter[0] = '\0';
                } else {
                    std::snprintf(state.audit_filter, sizeof state.audit_filter, "%s",
                                  kn_str.c_str());
                }
            }
            ImGui::PopID();
            if (active) {
                pop_primary_button_colors();
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    // ---- Status line ------------------------------------------------
    if (!state.audit_error_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.audit_error_msg.c_str());
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    std::size_t bad_checksum = 0;
    for (auto const &e : state.audit_entries) {
        if (!e.checksum_valid)
            ++bad_checksum;
    }
    if (bad_checksum > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::Text("%zu entries, %zu with bad checksum (possible tampering / corruption)",
                    state.audit_entries.size(), bad_checksum);
        ImGui::PopStyleColor();
    } else {
        text_subtle("%zu entries", state.audit_entries.size());
    }

    // Timeline sparkline — bar chart of entry count per time bucket
    // across the visible range. Bucket count is fixed at 40 so the
    // sparkline is dense enough to read but doesn't dominate the
    // panel. Skips when there are too few entries to plot meaningfully
    // (one bar would just be noise).
    if (state.audit_entries.size() >= 3) {
        std::int64_t t_min = state.audit_entries.front().timestamp_ns;
        std::int64_t t_max = t_min;
        for (auto const &e : state.audit_entries) {
            if (e.timestamp_ns < t_min)
                t_min = e.timestamp_ns;
            if (e.timestamp_ns > t_max)
                t_max = e.timestamp_ns;
        }
        if (t_max > t_min) {
            constexpr int kBuckets = 40;
            std::array<double, kBuckets> counts{};
            std::array<double, kBuckets> xs{};
            auto const span = static_cast<double>(t_max - t_min);
            for (auto const &e : state.audit_entries) {
                auto const rel = static_cast<double>(e.timestamp_ns - t_min);
                int b = static_cast<int>(rel * kBuckets / span);
                if (b < 0)
                    b = 0;
                if (b >= kBuckets)
                    b = kBuckets - 1;
                counts[static_cast<std::size_t>(b)] += 1.0;
            }
            for (int i = 0; i < kBuckets; ++i) {
                xs[static_cast<std::size_t>(i)] = static_cast<double>(i);
            }
            ImPlotFlags const flags = ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                                      ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText |
                                      ImPlotFlags_NoBoxSelect;
            if (ImPlot::BeginPlot("##audit_sparkline", ImVec2(-FLT_MIN, 60.0f), flags)) {
                ImPlot::SetupAxes(nullptr, nullptr,
                                  ImPlotAxisFlags_NoTickLabels |
                                      ImPlotAxisFlags_NoTickMarks |
                                      ImPlotAxisFlags_NoGridLines |
                                      ImPlotAxisFlags_NoMenus,
                                  ImPlotAxisFlags_AutoFit |
                                      ImPlotAxisFlags_NoTickLabels |
                                      ImPlotAxisFlags_NoTickMarks |
                                      ImPlotAxisFlags_NoGridLines |
                                      ImPlotAxisFlags_NoMenus);
                ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, kBuckets - 0.5, ImPlotCond_Always);
                ImPlotSpec bar_spec;
                bar_spec.FillColor = ImVec4(0.55f, 0.35f, 0.85f, 0.70f);
                bar_spec.LineColor = ImVec4(0.55f, 0.35f, 0.85f, 1.0f);
                ImPlot::PlotBars("##bars", xs.data(), counts.data(), kBuckets, 0.85,
                                 bar_spec);
                ImPlot::EndPlot();
            }
            // Range labels under the sparkline.
            auto const iso_short = [](std::int64_t ns) -> std::string {
                std::time_t const t = ns / 1'000'000'000;
                std::tm tm{};
#if defined(_WIN32)
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                char buf[24];
                std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
                return std::string{buf};
            };
            text_subtle("%s  →  %s  (UTC)", iso_short(t_min).c_str(),
                        iso_short(t_max).c_str());
        }
    }
    ImGui::Separator();

    if (state.audit_entries.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 24.0f));
        render_empty_state("No audit entries.",
                           "Operations that flash, read, or modify the ECU "
                           "land here once the auto-subscribers ship.");
        ImGui::End();
        return;
    }

    // Time-range slider — [start, end] clip on top of text/chip
    // filters. Values are normalized [0..1] across the full span;
    // converted to ns at filter time. Drag either knob to narrow.
    {
        ImGui::PushItemWidth(140.0f);
        ImGui::DragFloatRange2("Range", &state.audit_range_start, &state.audit_range_end,
                               0.005f, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##audit_range_reset")) {
            state.audit_range_start = 0.0f;
            state.audit_range_end = 1.0f;
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reset to the full timestamp span.");
        }
    }

    // Convert range knobs to nanosecond bounds.
    std::int64_t range_t_lo = std::numeric_limits<std::int64_t>::min();
    std::int64_t range_t_hi = std::numeric_limits<std::int64_t>::max();
    if (state.audit_range_start > 0.0f || state.audit_range_end < 1.0f) {
        std::int64_t t_min = state.audit_entries.front().timestamp_ns;
        std::int64_t t_max = t_min;
        for (auto const &e : state.audit_entries) {
            if (e.timestamp_ns < t_min)
                t_min = e.timestamp_ns;
            if (e.timestamp_ns > t_max)
                t_max = e.timestamp_ns;
        }
        if (t_max > t_min) {
            auto const span = static_cast<double>(t_max - t_min);
            range_t_lo =
                t_min + static_cast<std::int64_t>(
                            static_cast<double>(state.audit_range_start) * span);
            range_t_hi =
                t_min + static_cast<std::int64_t>(
                            static_cast<double>(state.audit_range_end) * span);
        }
    }

    // Build the visible-row index list once per render — filter +
    // sort. ImGuiTableSortSpecs would be overkill for one column.
    std::string_view const filter{state.audit_filter};
    std::vector<std::size_t> indices;
    indices.reserve(state.audit_entries.size());
    for (std::size_t i = 0; i < state.audit_entries.size(); ++i) {
        auto const &e = state.audit_entries[i];
        if (e.timestamp_ns < range_t_lo || e.timestamp_ns > range_t_hi) {
            continue;
        }
        if (!filter.empty()) {
            auto const kind = st::audit::kind_name(e.kind);
            if (!contains_ci(kind, filter) && !contains_ci(e.source, filter) &&
                !contains_ci(e.description, filter)) {
                continue;
            }
        }
        indices.push_back(i);
    }
    if (state.audit_newest_first) {
        std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            return state.audit_entries[a].timestamp_ns > state.audit_entries[b].timestamp_ns;
        });
    } else {
        std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            return state.audit_entries[a].timestamp_ns < state.audit_entries[b].timestamp_ns;
        });
    }

    // ---- Table ------------------------------------------------------
    ImGuiTableFlags const flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##audit_table", 6, flags)) {
        ImGui::End();
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("\xE2\x98\x86", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("Time (UTC)", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("CRC", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableHeadersRow();

    char ts_buf[32];
    for (std::size_t idx : indices) {
        auto const &e = state.audit_entries[idx];
        std::string const pin_key = audit_pin_key(e);
        bool const is_pinned = state.audit_pinned_keys.contains(pin_key);
        // Pinned-only filter applies after sort + chip + range + text
        // filters so the user can compose: e.g. "pinned AND emissions
        // edits in last hour".
        if (state.audit_show_pinned_only && !is_pinned) {
            continue;
        }
        ImGui::TableNextRow();
        // Pin column. Star toggle on the leftmost cell so it's the
        // first thing the eye lands on. Always-enabled — even tampered
        // entries are pinnable (the user might want to flag a
        // bad-CRC row for later forensic review).
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(idx) ^ 0x70696E00);
        char const *glyph = is_pinned ? "\xE2\x98\x85" : "\xE2\x98\x86";
        if (ImGui::SmallButton(glyph)) {
            if (is_pinned) {
                state.audit_pinned_keys.erase(pin_key);
            } else {
                state.audit_pinned_keys.insert(pin_key);
            }
            save_audit_pinned(state);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(is_pinned
                                  ? "Unpin: this entry won't be highlighted next session."
                                  : "Pin: keep this entry flagged for review across sessions.\n"
                                    "Pinned IDs persist to <project>/audit.pinned.");
        }
        ImGui::PopID();

        ImGui::TableNextColumn();
        format_iso_utc(e.timestamp_ns, ts_buf, sizeof ts_buf);
        ImGui::TextUnformatted(ts_buf);

        ImGui::TableNextColumn();
        auto const kind = st::audit::kind_name(e.kind);
        ImGui::TextUnformatted(kind.data(), kind.data() + kind.size());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.source.c_str());

        ImGui::TableNextColumn();
        // Description rendered as a Selectable so double-click can drive
        // a "jump to table" action when the entry's fields carry a
        // table id. Most entries don't (project.opened etc.) and the
        // selectable is just inert decoration for them.
        std::string const *table_field = nullptr;
        for (auto const &[k, v] : e.fields) {
            if (k == "table" && !v.empty()) {
                table_field = &v;
                break;
            }
        }
        ImGui::PushID(static_cast<int>(idx));
        if (ImGui::Selectable(e.description.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (table_field != nullptr && ImGui::IsMouseDoubleClicked(0)) {
                jump_to_table(state, *table_field);
            }
        }
        // Right-click context menu — single-entry NDJSON copy. Useful
        // for quoting one event in a support thread or chat.
        if (ImGui::BeginPopupContextItem("##audit_row_ctx")) {
            // Copy disabled on tampered entries — serialize_entry
            // recomputes CRC32, so emitting a bad-checksum entry that
            // way would launder the corruption. Forensic users who
            // need to share the original bytes can hand the audit.log
            // file directly via the "Open log location" reveal.
            ImGui::BeginDisabled(!e.checksum_valid);
            if (ImGui::MenuItem("Copy as NDJSON")) {
                auto const line = st::audit::serialize_entry(e);
                ImGui::SetClipboardText(line.c_str());
            }
            ImGui::EndDisabled();
            if (!e.checksum_valid && ImGui::IsItemHovered(
                                          ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "Disabled: this entry's stored CRC32 doesn't match.\n"
                    "Copying via serialize_entry would emit a fresh CRC\n"
                    "and hide the tampering. Share the raw audit.log\n"
                    "file instead (Open log location button).");
            }
            if (table_field != nullptr) {
                ImGui::Separator();
                if (ImGui::MenuItem("Jump to table")) {
                    jump_to_table(state, *table_field);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) {
            if (!e.fields.empty()) {
                ImGui::BeginTooltip();
                for (auto const &[k, v] : e.fields) {
                    ImGui::Text("%s = %s", k.c_str(), v.c_str());
                }
                if (table_field != nullptr) {
                    ImGui::Separator();
                    text_subtle("Double-click → open '%s' in editor.",
                                table_field->c_str());
                }
                ImGui::EndTooltip();
            } else if (table_field != nullptr) {
                ImGui::SetTooltip("Double-click → open '%s' in editor.",
                                  table_field->c_str());
            }
        }

        ImGui::TableNextColumn();
        if (e.checksum_valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
            ImGui::TextUnformatted("ok");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
            ImGui::TextUnformatted("BAD");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("On-disk CRC32 doesn't match the recomputed value.\n"
                                  "This entry has been tampered with or corrupted.");
            }
        }
    }
    ImGui::EndTable();

    ImGui::End();
}

} // namespace st::ui
