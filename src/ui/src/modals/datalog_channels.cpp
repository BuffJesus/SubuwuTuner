// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// F6 (lite) — Datalog channel catalog. Renders every SSM PID + every
// switch (single-bit RAM flag) from the loaded definition pack in a
// filterable, sortable table. The wedge that closes the "what can I
// log" half of the F6 datalog brain without needing the .csv.gz
// decompress + classify stack.
//
// Filter is case-insensitive, matched against name + id + scaling +
// unit (any-field substring). Default-log channels are visually
// flagged so a user scanning the list can spot the ones already
// firing in their existing presets.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

namespace {

[[nodiscard]] bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    auto const lc = [](char c) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lc(hay[i + j]) != lc(needle[j])) {
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

[[nodiscard]] bool pid_matches(st::Pid const &p, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    return icontains(p.name, filter) || icontains(p.id, filter) ||
            icontains(p.scaling, filter) || icontains(p.unit, filter);
}

[[nodiscard]] bool switch_matches(st::Switch const &s,
                                    std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    return icontains(s.name, filter) || icontains(s.id, filter);
}

} // namespace

void render_datalog_channels_modal(AppState &state) {
    if (state.show_datalog_channels_modal) {
        ImGui::OpenPopup("\xEE\xA0\x80  Datalog channel catalog##dl_channels");
        state.show_datalog_channels_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 480.0f),
                                         ImVec2(1100.0f, 800.0f));
    if (!ImGui::BeginPopupModal(
            "\xEE\xA0\x80  Datalog channel catalog##dl_channels", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped(
        "Every SSM PID + RAM switch in the loaded definition pack. "
        "Use the filter to narrow by channel name, ID, scaling, or unit. "
        "Default-log channels are flagged.");
    ImGui::Separator();
    ImGui::Spacing();

    if (!state.project.has_value()) {
        ImGui::TextColored(chip_fg_muted(),
                            "Open a project to load its definition pack.");
        ImGui::Spacing();
        if (ImGui::Button("Close##dl_ch_close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    auto const &def_ref = state.project->definition();

    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##dl_ch_filter", state.datalog_channels_filter,
                      sizeof(state.datalog_channels_filter));
    std::string_view const filter{state.datalog_channels_filter};

    auto const &pids = def_ref.pids();
    auto const &switches = def_ref.switches();
    std::vector<std::size_t> visible_pids;
    visible_pids.reserve(pids.size());
    for (std::size_t i = 0; i < pids.size(); ++i) {
        if (pid_matches(pids[i], filter)) {
            visible_pids.push_back(i);
        }
    }
    std::vector<std::size_t> visible_switches;
    visible_switches.reserve(switches.size());
    for (std::size_t i = 0; i < switches.size(); ++i) {
        if (switch_matches(switches[i], filter)) {
            visible_switches.push_back(i);
        }
    }

    ImGui::Spacing();
    ImGui::Text("PIDs: %zu of %zu shown   Switches: %zu of %zu shown",
                 visible_pids.size(), pids.size(),
                 visible_switches.size(), switches.size());
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##dl_ch_tabs")) {
        if (ImGui::BeginTabItem("SSM PIDs")) {
            if (ImGui::BeginTable("##dl_ch_pids", 6,
                                   ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_Sortable,
                                   ImVec2(0.0f, 360.0f))) {
                ImGui::TableSetupColumn("Name",
                                         ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ID",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         180.0f);
                ImGui::TableSetupColumn("SSM addr",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         100.0f);
                ImGui::TableSetupColumn("Type",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         70.0f);
                ImGui::TableSetupColumn("Unit",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         60.0f);
                ImGui::TableSetupColumn("Default log",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         90.0f);
                ImGui::TableHeadersRow();
                for (std::size_t i : visible_pids) {
                    auto const &p = pids[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(p.name.c_str());
                    if (ImGui::IsItemHovered() && !p.scaling.empty()) {
                        ImGui::SetTooltip("Scaling: %s",
                                           p.scaling.c_str());
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(p.id.c_str());
                    ImGui::TableSetColumnIndex(2);
                    char addr[16];
                    std::snprintf(addr, sizeof addr, "0x%06zX",
                                   p.ssm_address);
                    ImGui::TextUnformatted(addr);
                    ImGui::TableSetColumnIndex(3);
                    auto const dt_sv = st::to_string(p.data_type);
                    std::string const dt_str{dt_sv.data(), dt_sv.size()};
                    ImGui::TextUnformatted(dt_str.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(p.unit.c_str());
                    ImGui::TableSetColumnIndex(5);
                    if (p.default_log) {
                        chip("yes", chip_fg_ok(), chip_bg_ok());
                    } else {
                        ImGui::TextDisabled("\xE2\x80\x94");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Switches")) {
            if (ImGui::BeginTable("##dl_ch_sw", 5,
                                   ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, 360.0f))) {
                ImGui::TableSetupColumn("Name",
                                         ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ID",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         180.0f);
                ImGui::TableSetupColumn("SSM addr",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         100.0f);
                ImGui::TableSetupColumn("Bit",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         50.0f);
                ImGui::TableSetupColumn("Default log",
                                         ImGuiTableColumnFlags_WidthFixed,
                                         90.0f);
                ImGui::TableHeadersRow();
                for (std::size_t i : visible_switches) {
                    auto const &s = switches[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(s.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(s.id.c_str());
                    ImGui::TableSetColumnIndex(2);
                    char addr[16];
                    std::snprintf(addr, sizeof addr, "0x%06zX",
                                   s.ssm_address);
                    ImGui::TextUnformatted(addr);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d", s.bit);
                    ImGui::TableSetColumnIndex(4);
                    if (s.default_log) {
                        chip("yes", chip_fg_ok(), chip_bg_ok());
                    } else {
                        ImGui::TextDisabled("\xE2\x80\x94");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Close##dl_ch_close3")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
