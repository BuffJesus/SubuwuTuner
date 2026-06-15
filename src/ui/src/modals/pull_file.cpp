// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Generic "Pull single file..." dialog. Takes an arbitrary AP path,
// runs the cmd 0x20/0x21 ReadFile pipeline via the existing
// ets_panel_read_file_sync helper, saves to a user-selected local
// destination. Useful for one-off pulls of files outside the
// canonical /maps + /datalog + /presets + /images + /settings +
// /backupcksum surface — debugging, fixture capture, or files under
// /dump/ that aren't the marriage backup.
//
// Workflow-gated. When ST_HAVE_AP_WORKFLOW is undefined the modal
// renders a clear "workflow off" notice in place of the action.

#include "modals/modals.hpp"
#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace st::ui {

namespace {

struct State {
    std::string status; // success or error message
    bool ok{false};
};

State &state() {
    static State s;
    return s;
}

void reset_state(State &s) {
    s.status.clear();
    s.ok = false;
}

void run_pull(AppState &app, State &s) {
    s.status.clear();
    s.ok = false;
    if (app.pull_file_ap_path[0] == '\0') {
        s.status = "Enter an AP path first (e.g. /maps/MyTune.ptm).";
        return;
    }
#ifndef ST_HAVE_AP_WORKFLOW
    s.status =
        "AP workflow surface is off in this build. Reconfigure with "
        "-DST_ENABLE_COBB_AP_WORKFLOW=ON to enable AP reads.";
    return;
#else
    if (!ets_panel_has_channel()) {
        s.status =
            "No AccessPort channel open. Connect via the AccessPort "
            "Browser panel first.";
        return;
    }
    std::string err;
    auto bytes = ets_panel_read_file_sync(app.pull_file_ap_path, err);
    if (!err.empty()) {
        s.status = "Pull failed: " + err;
        return;
    }
    // Default the output filename to the basename of the AP path.
    std::string const ap_path{app.pull_file_ap_path};
    auto const slash = ap_path.find_last_of('/');
    std::string const default_name =
        (slash == std::string::npos) ? ap_path : ap_path.substr(slash + 1);
    NFD::UniquePathU8 out_path;
    nfdu8filteritem_t const filters[] = {{"Any file", "*"}};
    auto const r = NFD::SaveDialog(out_path, filters, 1, nullptr,
                                     default_name.c_str());
    if (r == NFD_CANCEL) {
        s.status =
            "Pull completed but no save destination was chosen. "
            "The pulled bytes are discarded.";
        return;
    }
    if (r != NFD_OKAY || out_path == nullptr) {
        s.status = std::string{"File dialog: "} + NFD::GetError();
        return;
    }
    std::filesystem::path const dest{out_path.get()};
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::ofstream f{dest, std::ios::binary};
    if (!f) {
        s.status =
            "Couldn't open " + dest.string() + " for write.";
        return;
    }
    f.write(reinterpret_cast<char const *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    s.ok = true;
    s.status =
        "Saved " + std::to_string(bytes.size()) +
        " bytes -> " + dest.string();
#endif
}

} // namespace

void render_pull_file_modal(AppState &app_state) {
    if (app_state.show_pull_file_modal) {
        ImGui::OpenPopup(
            "\xEE\xA0\x84  Pull file from AP##pull_file_modal");
        app_state.show_pull_file_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 280.0f),
                                         ImVec2(840.0f, 480.0f));
    if (!ImGui::BeginPopupModal(
            "\xEE\xA0\x84  Pull file from AP##pull_file_modal", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto &s = state();
    ImGui::TextWrapped(
        "Pull an arbitrary file from the connected AccessPort. "
        "Useful for one-off captures outside the standard /maps + "
        "/datalog + /presets + /images surface (e.g. files under "
        "/dump/ or future paths the canonical browser doesn't cover).");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("AP path:");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pull_file_ap_path", app_state.pull_file_ap_path,
                      sizeof(app_state.pull_file_ap_path));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Absolute path on the AP — e.g. /maps/Stage1.ptm,\n"
            "/datalog/some_log.csv.gz, /dump/<calid>_enc.rom, ...");
    }
    ImGui::Spacing();

#ifndef ST_HAVE_AP_WORKFLOW
    ImGui::TextColored(
        chip_fg_caution(),
        "AP workflow surface is OFF in this build. The Pull button\n"
        "will refuse — reconfigure with -DST_ENABLE_COBB_AP_WORKFLOW=ON.");
#else
    if (!ets_panel_has_channel()) {
        ImGui::TextColored(
            chip_fg_caution(),
            "AccessPort not connected — open the AccessPort Browser\n"
            "panel and connect first.");
    }
#endif

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Pull...##pull_file_go")) {
        run_pull(app_state, s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close##pull_file_close")) {
        reset_state(s);
        ImGui::CloseCurrentPopup();
    }
    if (!s.status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(s.ok ? chip_fg_ok() : chip_fg_danger(),
                            "%s", s.status.c_str());
    }
    ImGui::EndPopup();
}

} // namespace st::ui
