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

#include "st/audit.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
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

void load_audit_log(AppState &state) {
    state.audit_entries.clear();
    state.audit_error_msg.clear();
    state.audit_loaded = false;
    if (!state.project.has_value()) {
        state.audit_error_msg = "No project loaded — open a .stune project to view its audit log.";
        return;
    }
    auto const log_path = state.project->dir() / "audit.log";
    if (!std::filesystem::exists(log_path)) {
        state.audit_loaded = true; // genuinely empty, not an error
        return;
    }
    auto r = st::audit::read_all(log_path);
    if (!r.has_value()) {
        state.audit_error_msg = "Read failed: " + r.error().to_string();
        return;
    }
    state.audit_entries = std::move(*r);
    state.audit_loaded = true;
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
    // keep the cached entries until the user clicks Refresh — a long
    // log shouldn't re-read on every panel toggle.
    if (!state.audit_loaded && state.audit_error_msg.empty()) {
        load_audit_log(state);
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
    ImGui::Checkbox("Newest first", &state.audit_newest_first);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Sort by timestamp descending (default) or ascending.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##audit_filter", "Filter (kind/source/description)…",
                             state.audit_filter, sizeof state.audit_filter);

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
    ImGui::Separator();

    if (state.audit_entries.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 24.0f));
        render_empty_state("No audit entries.",
                           "Operations that flash, read, or modify the ECU "
                           "land here once the auto-subscribers ship.");
        ImGui::End();
        return;
    }

    // Build the visible-row index list once per render — filter +
    // sort. ImGuiTableSortSpecs would be overkill for one column.
    std::string_view const filter{state.audit_filter};
    std::vector<std::size_t> indices;
    indices.reserve(state.audit_entries.size());
    for (std::size_t i = 0; i < state.audit_entries.size(); ++i) {
        auto const &e = state.audit_entries[i];
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
    if (!ImGui::BeginTable("##audit_table", 5, flags)) {
        ImGui::End();
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time (UTC)", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("CRC", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableHeadersRow();

    char ts_buf[32];
    for (std::size_t idx : indices) {
        auto const &e = state.audit_entries[idx];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        format_iso_utc(e.timestamp_ns, ts_buf, sizeof ts_buf);
        ImGui::TextUnformatted(ts_buf);

        ImGui::TableNextColumn();
        auto const kind = st::audit::kind_name(e.kind);
        ImGui::TextUnformatted(kind.data(), kind.data() + kind.size());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.source.c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.description.c_str());
        if (!e.fields.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (auto const &[k, v] : e.fields) {
                ImGui::Text("%s = %s", k.c_str(), v.c_str());
            }
            ImGui::EndTooltip();
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
