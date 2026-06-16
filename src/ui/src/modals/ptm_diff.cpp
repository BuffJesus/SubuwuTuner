// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// PTM Diff modal — pick two .ptm files, decode both, compute the
// envelope + per-layer (or per-table) delta, render as tables.
//
// Mirrors `subuwutuner-cli ptm diff`. Uses the shared
// st::library::compute_tune_diff function so the CLI and GUI views
// agree by construction.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"
#include "st/devices/ets/architectural_classifier.hpp"
#include "st/devices/ets/ptm_cipher.hpp"
#include "st/library/interpret.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/library/tune_diff.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace st::ui {

namespace {

std::vector<std::uint8_t> read_all(std::filesystem::path const &p) {
    std::ifstream f{p, std::ios::binary | std::ios::ate};
    if (!f) {
        return {};
    }
    auto const sz = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char *>(bytes.data()), sz);
    return bytes;
}

void pick_file(char *buf, std::size_t buf_size, char const *filter_name, char const *filter_ext) {
    NFD::UniquePath out_path;
    nfdfilteritem_t filters[1] = {{filter_name, filter_ext}};
    auto const result = NFD::OpenDialog(out_path, filters, 1);
    if (result == NFD_OKAY && out_path != nullptr) {
        std::strncpy(buf, out_path.get(), buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

void pick_dir(char *buf, std::size_t buf_size) {
    NFD::UniquePath out_path;
    auto const result = NFD::PickFolder(out_path);
    if (result == NFD_OKAY && out_path != nullptr) {
        std::strncpy(buf, out_path.get(), buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

void clear_result(AppState &s) {
    s.ptm_diff_have_result = false;
    s.ptm_diff_error.clear();
    s.ptm_diff_by_layer.clear();
    s.ptm_diff_by_table.clear();
    s.ptm_diff_interpretation.clear();
}

bool decode_one(char const *path, st::library::DecodedPtm &out, std::string &err) {
    auto const bytes = read_all(path);
    if (bytes.empty()) {
        err = std::string{"Cannot read "} + path;
        return false;
    }
    auto contents = st::devices::ets::cipher::decrypt_ptm(bytes);
    if (!contents.has_value()) {
        err = std::string{"decrypt_ptm: "} + contents.error().to_string();
        return false;
    }
    auto decoded = st::library::decode_ptm_xml(contents->inner_xml);
    if (!decoded.has_value()) {
        err = std::string{"decode_ptm_xml: "} + decoded.error().to_string();
        return false;
    }
    out = std::move(*decoded);
    return true;
}

void run_diff(AppState &s) {
    clear_result(s);
    if (s.ptm_diff_a_path[0] == '\0' || s.ptm_diff_b_path[0] == '\0') {
        s.ptm_diff_error = "Pick two .ptm files.";
        return;
    }
    st::library::DecodedPtm a, b;
    if (!decode_one(s.ptm_diff_a_path, a, s.ptm_diff_error)) {
        return;
    }
    if (!decode_one(s.ptm_diff_b_path, b, s.ptm_diff_error)) {
        return;
    }

    std::vector<st::library::TableRange> ranges;
    if (s.ptm_diff_def_path[0] != '\0') {
        auto def = st::Definition::from_file(std::filesystem::path{s.ptm_diff_def_path});
        if (def.has_value()) {
            ranges = st::library::compute_table_ranges(*def);
        }
    }

    auto const result = st::library::compute_tune_diff(a, b, ranges);
    s.ptm_diff_a_total = result.a_total_patches;
    s.ptm_diff_b_total = result.b_total_patches;
    s.ptm_diff_shared = result.shared_patches;
    s.ptm_diff_a_only = result.a_only_patches;
    s.ptm_diff_b_only = result.b_only_patches;
    s.ptm_diff_changed_at_shared = result.changed_at_shared;
    s.ptm_diff_changed_bytes = result.changed_bytes;
    for (auto const &ld : result.by_layer) {
        AppState::PtmDiffRow row;
        row.label = std::string{st::devices::ets::layer_label(ld.layer)};
        row.a_only = ld.a_only_patches;
        row.b_only = ld.b_only_patches;
        row.shared = ld.shared_patches;
        row.changed = ld.changed_at_shared;
        row.changed_bytes = ld.changed_bytes;
        s.ptm_diff_by_layer.push_back(std::move(row));
    }
    for (auto const &td : result.by_table) {
        AppState::PtmDiffRow row;
        row.label = td.table_name;
        row.a_only = td.a_only_patches;
        row.b_only = td.b_only_patches;
        row.shared = td.shared_patches;
        row.changed = td.changed_at_shared;
        row.changed_bytes = td.changed_bytes;
        s.ptm_diff_by_table.push_back(std::move(row));
    }
    s.ptm_diff_interpretation = st::library::interpret_diff(a, b, result);
    s.ptm_diff_have_result = true;
}

void render_diff_table(char const *id, std::vector<AppState::PtmDiffRow> const &rows) {
    if (rows.empty()) {
        return;
    }
    if (ImGui::BeginTable(id, 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 200.0f))) {
        ImGui::TableSetupColumn("Bucket", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("a_only", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("b_only", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("shared", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("changed", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Δbytes", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (auto const &row : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", row.a_only);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", row.b_only);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", row.shared);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", row.changed);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%llu", static_cast<unsigned long long>(row.changed_bytes));
        }
        ImGui::EndTable();
    }
}

} // namespace

void render_ptm_diff_modal(AppState &state) {
    if (state.show_ptm_diff_modal) {
        ImGui::OpenPopup("\xEE\xA0\x84  Diff .ptm##ptm_diff_modal");
        state.show_ptm_diff_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 480.0f), ImVec2(1100.0f, 860.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA0\x84  Diff .ptm##ptm_diff_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("Compare two .ptm tune files. Decodes both, builds the union of "
                       "patch addresses, and shows per-layer (or per-table when a def-pack "
                       "is picked) delta counts + changed-byte tallies.");
    ImGui::Separator();
    ImGui::Spacing();

    // A
    ImGui::Text("File A:");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##diff_a", state.ptm_diff_a_path, sizeof(state.ptm_diff_a_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##diff_a_btn")) {
        pick_file(state.ptm_diff_a_path, sizeof(state.ptm_diff_a_path), "COBB tune", "ptm");
    }

    // B
    ImGui::Spacing();
    ImGui::Text("File B:");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##diff_b", state.ptm_diff_b_path, sizeof(state.ptm_diff_b_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##diff_b_btn")) {
        pick_file(state.ptm_diff_b_path, sizeof(state.ptm_diff_b_path), "COBB tune", "ptm");
    }

    // Def-pack
    ImGui::Spacing();
    ImGui::Text("Definition pack (optional, enables by-table view):");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##diff_def", state.ptm_diff_def_path, sizeof(state.ptm_diff_def_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##diff_def_btn")) {
        pick_dir(state.ptm_diff_def_path, sizeof(state.ptm_diff_def_path));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Compute")) {
        run_diff(state);
    }
    if (!state.ptm_diff_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(chip_fg_danger(), "%s", state.ptm_diff_error.c_str());
    }

    if (state.ptm_diff_have_result) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Envelope");
        ImGui::BulletText("A total: %u   B total: %u   Shared: %u",
                          state.ptm_diff_a_total, state.ptm_diff_b_total,
                          state.ptm_diff_shared);
        ImGui::BulletText("A only:  %u   B only:  %u   Changed at shared: %u",
                          state.ptm_diff_a_only, state.ptm_diff_b_only,
                          state.ptm_diff_changed_at_shared);
        ImGui::BulletText("Changed bytes: %llu",
                          static_cast<unsigned long long>(state.ptm_diff_changed_bytes));

        ImGui::Spacing();
        ImGui::TextUnformatted("By layer");
        render_diff_table("##diff_by_layer", state.ptm_diff_by_layer);

        if (!state.ptm_diff_by_table.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("By table");
            render_diff_table("##diff_by_table", state.ptm_diff_by_table);
        } else if (state.ptm_diff_def_path[0] == '\0') {
            ImGui::Spacing();
            ImGui::TextColored(chip_fg_muted(),
                               "Pick a definition pack to enable the by-table view.");
        }

        if (!state.ptm_diff_interpretation.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("What changed");
            for (auto const &line : state.ptm_diff_interpretation) {
                ImGui::Bullet();
                ImGui::TextWrapped("%s", line.c_str());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        clear_result(state);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
