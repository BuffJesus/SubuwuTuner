// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// PTM Export modal — reverse of the Import wizard. Reads the loaded
// project's [ptm_metadata] block from project.toml and the [[patch]]
// entries from ptm_patches.toml, rebuilds the inner PrivateData XML,
// runs the lot through encrypt_ptm, and writes the resulting bytes to
// a user-chosen path.
//
// Gated at build time on ST_ENABLE_COBB_AP_PTM_REWRITE. When OFF,
// encrypt_ptm returns NotImplemented and the modal surfaces the
// message inline.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/devices/ap3/ptm_cipher.hpp"
#include "st/library/ptm_xml_builder.hpp"

#include <toml++/toml.hpp>

#include <imgui.h>
#include <nfd.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace st::ui {

namespace {

void pick_save(char *buf, std::size_t buf_size) {
    NFD::UniquePath out_path;
    nfdfilteritem_t filters[1] = {{"COBB tune", "ptm"}};
    auto const result = NFD::SaveDialog(out_path, filters, 1, nullptr, "tune.ptm");
    if (result == NFD_OKAY && out_path != nullptr) {
        std::strncpy(buf, out_path.get(), buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

bool parse_seed(std::string_view s, std::uint32_t &out) {
    std::uint64_t v = 0;
    auto p = s.data();
    auto e = s.data() + s.size();
    int base = 10;
    if (s.size() >= 2 && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        p += 2;
    }
    for (; p < e; ++p) {
        char c = *p;
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
        if (v > 0xFFFFFFFFULL) return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

void clear_state(AppState &s) {
    s.ptm_export_error.clear();
    s.ptm_export_success.clear();
}

void run_export(AppState &s) {
    clear_state(s);
    if (!s.project.has_value()) {
        s.ptm_export_error = "No project open.";
        return;
    }
    if (s.ptm_export_out_path[0] == '\0') {
        s.ptm_export_error = "Pick an output .ptm path.";
        return;
    }
    std::uint32_t seed = 0;
    if (!parse_seed(s.ptm_export_seed_hex, seed)) {
        s.ptm_export_error = "Bad --seed value (expected hex or decimal u32).";
        return;
    }

    auto const proj_dir = s.project->dir();
    auto const proj_toml = proj_dir / "project.toml";
    auto const patches_toml = proj_dir / "ptm_patches.toml";

    if (!std::filesystem::exists(patches_toml)) {
        s.ptm_export_error = "ptm_patches.toml missing — only projects from `ptm import` "
                              "(CLI or GUI wizard) can be exported back to a .ptm.";
        return;
    }

    toml::table proj;
    try {
        proj = toml::parse_file(proj_toml.string());
    } catch (toml::parse_error const &e) {
        s.ptm_export_error = std::string{"project.toml parse: "} + e.description().data();
        return;
    }
    auto const *meta = proj["ptm_metadata"].as_table();
    if (meta == nullptr) {
        s.ptm_export_error = "project.toml is missing [ptm_metadata].";
        return;
    }
    std::string const vendor_id  = (*meta)["vendor_id"].value_or<std::string>("");
    std::string const vehicle_id = (*meta)["vehicle_id"].value_or<std::string>("");
    std::uint32_t const lock_mask = (*meta)["lock_mask"].value_or<std::uint32_t>(0);
    std::string const rom_sum    = (*meta)["rom_sum"].value_or<std::string>("");
    std::string const save_date  = (*meta)["save_date_time"].value_or<std::string>("");

    toml::table pat;
    try {
        pat = toml::parse_file(patches_toml.string());
    } catch (toml::parse_error const &e) {
        s.ptm_export_error = std::string{"ptm_patches.toml parse: "} + e.description().data();
        return;
    }
    auto const *arr = pat["patch"].as_array();
    if (arr == nullptr) {
        s.ptm_export_error = "ptm_patches.toml has no [[patch]] entries.";
        return;
    }
    std::vector<st::library::PtmExportPatch> patches;
    patches.reserve(arr->size());
    for (auto const &node : *arr) {
        auto const *p = node.as_table();
        if (p == nullptr) {
            continue;
        }
        auto const rom_off = (*p)["rom_offset"].value_or<std::int64_t>(-1);
        auto const ram_off = (*p)["ram_offset"].value_or<std::int64_t>(0);
        auto const length  = (*p)["length"].value_or<std::int64_t>(-1);
        auto const b64     = (*p)["bytes_b64"].value_or<std::string>("");
        if (rom_off < 0 || length < 0 || b64.empty()) {
            continue;
        }
        patches.push_back({static_cast<std::uint32_t>(rom_off),
                           static_cast<std::int32_t>(ram_off),
                           static_cast<std::uint32_t>(length),
                           b64});
    }
    // Mirror of the CLI's 16a32b7 refuse: every [[patch]] entry was
    // malformed and dropped. Continuing would emit a syntactically
    // valid .ptm with zero patches — a do-nothing tune the user could
    // accidentally push to their AP.
    if (patches.empty()) {
        s.ptm_export_error =
            "Every [[patch]] entry in ptm_patches.toml was malformed and "
            "dropped. The project file may have been hand-edited or "
            "corrupted; re-import the source .ptm.";
        return;
    }

    auto const inner = st::library::build_ptm_inner_xml(
        vendor_id, vehicle_id, lock_mask, rom_sum, save_date, patches);
    auto const outer = st::library::build_ptm_outer_xml(
        vendor_id, vehicle_id, lock_mask, rom_sum, save_date);

    auto encrypted = st::devices::ap3::cipher::encrypt_ptm(inner, outer, seed);
    if (!encrypted.has_value()) {
        s.ptm_export_error = "encrypt_ptm: " + encrypted.error().to_string();
        return;
    }

    std::ofstream out{s.ptm_export_out_path, std::ios::binary};
    if (!out) {
        s.ptm_export_error = std::string{"Cannot open "} + s.ptm_export_out_path;
        return;
    }
    out.write(reinterpret_cast<char const *>(encrypted->data()),
              static_cast<std::streamsize>(encrypted->size()));
    s.ptm_export_success = std::string{"Wrote "} +
                           std::to_string(encrypted->size()) + " bytes to " +
                           s.ptm_export_out_path;
}

} // namespace

void render_ptm_export_modal(AppState &state) {
    if (state.show_ptm_export_modal) {
        ImGui::OpenPopup("\xEE\x9D\x8E  Export .ptm##ptm_export_modal");
        state.show_ptm_export_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 320.0f), ImVec2(900.0f, 600.0f));
    if (!ImGui::BeginPopupModal("\xEE\x9D\x8E  Export .ptm##ptm_export_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("Re-pack the loaded project back into a .ptm tune file. Requires "
                       "the project to have been created by `ptm import` (has a "
                       "[ptm_metadata] block + ptm_patches.toml).");
    ImGui::Separator();
    ImGui::Spacing();

    if (!state.project.has_value()) {
        ImGui::TextColored(chip_fg_muted(), "Open a project first.");
        ImGui::Spacing();
        if (ImGui::Button("Close")) {
            clear_state(state);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Project: %s", state.project->dir().string().c_str());
    if (state.ptm_imported_vendor.has_value()) {
        ImGui::Text("Vendor:  %s", state.ptm_imported_vendor->c_str());
    }
    if (state.ptm_imported_vehicle.has_value()) {
        ImGui::Text("Vehicle: %s", state.ptm_imported_vehicle->c_str());
    }
    ImGui::Spacing();

    ImGui::Text("Output .ptm path:");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##out_path", state.ptm_export_out_path,
                     sizeof(state.ptm_export_out_path));
    ImGui::SameLine();
    if (ImGui::Button("Save as\xE2\x80\xA6")) {
        pick_save(state.ptm_export_out_path, sizeof(state.ptm_export_out_path));
    }

    ImGui::Spacing();
    ImGui::Text("XTEA seed (hex or decimal u32, default 0x12345678):");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##seed", state.ptm_export_seed_hex, sizeof(state.ptm_export_seed_hex));

    if (!state.ptm_export_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "%s", state.ptm_export_error.c_str());
    }
    if (!state.ptm_export_success.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_ok(), "%s", state.ptm_export_success.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool const can_export = state.ptm_export_out_path[0] != '\0';
    if (!can_export) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Export")) {
        run_export(state);
    }
    if (!can_export) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        clear_state(state);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
