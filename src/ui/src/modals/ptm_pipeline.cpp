// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "modals/ptm_pipeline.hpp"

#include "st/devices/ets/ptm_cipher.hpp"
#include "st/library/ptm_xml_builder.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace st::ui {

std::optional<std::vector<std::uint8_t>>
build_ptm_bytes(AppState &state, std::uint32_t seed, std::string &err_out) {
    err_out.clear();

    if (!state.project.has_value()) {
        err_out = "No project open.";
        return std::nullopt;
    }

    auto const proj_dir = state.project->dir();
    auto const proj_toml = proj_dir / "project.toml";
    auto const patches_toml = proj_dir / "ptm_patches.toml";

    if (!std::filesystem::exists(patches_toml)) {
        err_out = "ptm_patches.toml missing — only projects from `ptm import` "
                  "(CLI or GUI wizard) can be exported back to a .ptm.";
        return std::nullopt;
    }

    toml::table proj;
    try {
        proj = toml::parse_file(proj_toml.string());
    } catch (toml::parse_error const &e) {
        err_out = std::string{"project.toml parse: "} + e.description().data();
        return std::nullopt;
    }
    auto const *meta = proj["ptm_metadata"].as_table();
    if (meta == nullptr) {
        err_out = "project.toml is missing [ptm_metadata].";
        return std::nullopt;
    }
    std::string const vendor_id  = (*meta)["vendor_id"].value_or<std::string>("");
    std::string const vehicle_id = (*meta)["vehicle_id"].value_or<std::string>("");
    std::uint32_t const lock_mask =
        (*meta)["lock_mask"].value_or<std::uint32_t>(0);
    std::string const rom_sum   = (*meta)["rom_sum"].value_or<std::string>("");
    std::string const save_date = (*meta)["save_date_time"].value_or<std::string>("");

    toml::table pat;
    try {
        pat = toml::parse_file(patches_toml.string());
    } catch (toml::parse_error const &e) {
        err_out =
            std::string{"ptm_patches.toml parse: "} + e.description().data();
        return std::nullopt;
    }
    auto const *arr = pat["patch"].as_array();
    if (arr == nullptr) {
        err_out = "ptm_patches.toml has no [[patch]] entries.";
        return std::nullopt;
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
                           static_cast<std::uint32_t>(length), b64});
    }
    if (patches.empty()) {
        err_out =
            "Every [[patch]] entry in ptm_patches.toml was malformed and "
            "dropped. The project file may have been hand-edited or "
            "corrupted; re-import the source .ptm.";
        return std::nullopt;
    }

    for (auto const &[name, val] : {
             std::pair<char const *, std::string_view>{"vendorId", vendor_id},
             std::pair<char const *, std::string_view>{"vehicleId", vehicle_id},
             std::pair<char const *, std::string_view>{"romSum", rom_sum},
             std::pair<char const *, std::string_view>{"saveDateTime",
                                                       save_date},
         }) {
        if (auto const v =
                st::library::validate_ptm_metadata_field(name, val);
            !v.has_value()) {
            err_out =
                std::string{"Cannot export: "} + v.error().to_string();
            return std::nullopt;
        }
    }

    auto const inner = st::library::build_ptm_inner_xml(
        vendor_id, vehicle_id, lock_mask, rom_sum, save_date, patches);
    auto const outer = st::library::build_ptm_outer_xml(
        vendor_id, vehicle_id, lock_mask, rom_sum, save_date);

    auto encrypted =
        st::devices::ets::cipher::encrypt_ptm(inner, outer, seed);
    if (!encrypted.has_value()) {
        err_out = "encrypt_ptm: " + encrypted.error().to_string();
        return std::nullopt;
    }
    return std::move(*encrypted);
}

} // namespace st::ui
