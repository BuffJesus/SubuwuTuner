// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tools → Browse Definitions modal. A thin read-only window over
// st::defs::PackRegistry: pick a directory, see every pack the registry
// would surface (loose top-level, per-platform loose, zipped). Future
// commits wire this into the New-Project pack selector so the user can
// pick a pack-by-id instead of a file path.

#include "modals/modals.hpp"

#include "app_state.hpp"

#include "st/defs/pack_registry.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace st::ui {

void render_def_registry_modal(AppState &state) {
    if (state.show_def_registry_modal) {
        ImGui::OpenPopup("Browse Definitions##def_registry_modal");
        state.show_def_registry_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700.0f, 520.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Browse Definitions##def_registry_modal",
                                nullptr, ImGuiWindowFlags_None)) {
        return;
    }

    ImGui::TextUnformatted("Definitions root:");
    float const avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(avail - 200.0f);
    ImGui::InputText("##def_reg_root", state.def_registry_root_input,
                     sizeof state.def_registry_root_input);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##def_reg_pick", ImVec2(80.0f, 0.0f))) {
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::PickFolder(out);
        if (r == NFD_OKAY) {
            std::snprintf(state.def_registry_root_input,
                          sizeof state.def_registry_root_input, "%s", out.get());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan##def_reg_scan", ImVec2(100.0f, 0.0f))) {
        state.def_registry_rows.clear();
        state.def_registry_warnings.clear();
        state.def_registry_scanned = true;
        auto reg_r = st::defs::PackRegistry::from_directory(
            std::filesystem::path{state.def_registry_root_input});
        if (!reg_r.has_value()) {
            state.status_msg = "Registry scan failed: " + reg_r.error().to_string();
        } else {
            for (auto const &w : reg_r->warnings()) {
                state.def_registry_warnings.push_back(w);
            }
            for (auto const &entry : reg_r->list_packs()) {
                AppState::DefRegistryRow row;
                row.id = entry.id;
                row.platform = entry.platform.empty() ? "—" : entry.platform;
                switch (entry.source) {
                case st::defs::PackEntry::Source::LooseTopLevel:
                    row.source_label = "loose-top";
                    break;
                case st::defs::PackEntry::Source::LoosePlatform:
                    row.source_label = "loose";
                    break;
                case st::defs::PackEntry::Source::Zipped:
                    row.source_label = "zipped";
                    break;
                }
                // Try to load + read the display name. Best-effort — if
                // parsing fails (corrupt or in-progress write), the row
                // still shows up with display_name = "(load failed)".
                auto def_r = reg_r->load_pack(entry.id);
                row.display_name = def_r.has_value()
                                       ? def_r->pack().display_name
                                       : std::string{"(load failed)"};
                state.def_registry_rows.push_back(std::move(row));
            }
            std::sort(state.def_registry_rows.begin(),
                      state.def_registry_rows.end(),
                      [](auto const &a, auto const &b) { return a.id < b.id; });
        }
    }

    if (state.def_registry_scanned) {
        ImGui::Separator();
        ImGui::Text("Found %zu pack(s)%s", state.def_registry_rows.size(),
                    state.def_registry_warnings.empty()
                        ? ""
                        : "  (warnings present)");
        if (!state.def_registry_warnings.empty()) {
            if (ImGui::TreeNode("warnings##def_reg_warn")) {
                for (auto const &w : state.def_registry_warnings) {
                    ImGui::TextWrapped("%s", w.c_str());
                }
                ImGui::TreePop();
            }
        }

        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##def_reg_filter", "filter by id or platform...",
                                 state.def_registry_filter,
                                 sizeof state.def_registry_filter);
        std::string filter_lc{state.def_registry_filter};
        std::transform(filter_lc.begin(), filter_lc.end(), filter_lc.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });

        ImGui::BeginChild("##def_reg_list", ImVec2(0.0f, -32.0f), true);
        if (ImGui::BeginTable("def_reg_table", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Id");
            ImGui::TableSetupColumn("Platform");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Display Name");
            ImGui::TableHeadersRow();
            for (auto const &row : state.def_registry_rows) {
                if (!filter_lc.empty()) {
                    auto contains = [&](std::string_view s) {
                        std::string lower{s};
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) {
                                           return static_cast<char>(std::tolower(c));
                                       });
                        return lower.find(filter_lc) != std::string::npos;
                    };
                    if (!contains(row.id) && !contains(row.platform)) {
                        continue;
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.id.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.platform.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(row.source_label.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(row.display_name.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    } else {
        ImGui::Separator();
        ImGui::TextDisabled("Click Scan to list packs at the chosen root.");
        ImGui::Dummy(ImVec2(0, 16));
        ImGui::TextWrapped(
            "The registry walks one level deep:\n"
            "  - <root>/<id>.toml         (loose, top-level)\n"
            "  - <root>/<platform>/<id>.toml  (loose, per-platform)\n"
            "  - <root>/<platform>.zip    (zipped archives)\n"
            "Loose files override zipped entries on id collision.");
    }

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
