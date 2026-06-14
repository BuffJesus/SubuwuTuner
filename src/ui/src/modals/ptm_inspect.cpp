// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// PTM Inspect modal — read-only viewer for a `.ptm` file. Picks the
// file (and optional def-pack), runs the full cipher chain plus
// decoder plus architectural classifier plus table mapping, then
// renders identity / architectural breakdown / top tables modified.
//
// Distinct from the Import modal because users routinely want to
// "look at" a tune before committing to a project. Mirrors the CLI's
// `subuwutuner-cli ptm inspect`.

#include "modals/modals.hpp"
#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"
#include "st/devices/ets/architectural_classifier.hpp"
#include "st/devices/ets/ptm_cipher.hpp"
#include "st/library/interpret.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cctype>
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

void clear_preview(AppState &s) {
    s.ptm_inspect_have_preview = false;
    s.ptm_inspect_vendor.clear();
    s.ptm_inspect_vehicle.clear();
    s.ptm_inspect_lock_mask = 0;
    s.ptm_inspect_rom_sum.clear();
    s.ptm_inspect_patches = 0;
    s.ptm_inspect_total_bytes = 0;
    s.ptm_inspect_layers.clear();
    s.ptm_inspect_top_tables.clear();
    s.ptm_inspect_interpretation.clear();
    s.ptm_inspect_error.clear();
    s.ptm_inspect_vehicle_match = AppState::PtmMatch::Unknown;
    s.ptm_inspect_rom_sum_match = AppState::PtmMatch::Unknown;
    s.ptm_inspect_ap_vehicle_id.clear();
    s.ptm_inspect_ap_backupcksum.clear();
}

// Case-insensitive equality. MD5 strings from `backupcksum` arrive
// lowercase (per RE), but .ptm metadata sometimes encodes them
// uppercase depending on the vendor. Vehicle IDs are uppercase by
// convention but a defensive ignoring of case avoids surprise
// mismatches if a vendor diverges.
[[nodiscard]] bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

void run_decode(AppState &s) {
    clear_preview(s);
    if (s.ptm_inspect_ptm_path[0] == '\0') {
        s.ptm_inspect_error = "Pick a .ptm file first.";
        return;
    }
    auto const bytes = read_all(s.ptm_inspect_ptm_path);
    if (bytes.empty()) {
        s.ptm_inspect_error = std::string{"Cannot read "} + s.ptm_inspect_ptm_path;
        return;
    }
    auto contents = st::devices::ets::cipher::decrypt_ptm(bytes);
    if (!contents.has_value()) {
        s.ptm_inspect_error = "decrypt_ptm: " + contents.error().to_string();
        return;
    }
    auto decoded = st::library::decode_ptm_xml(contents->inner_xml);
    if (!decoded.has_value()) {
        s.ptm_inspect_error = "decode_ptm_xml: " + decoded.error().to_string();
        return;
    }
    s.ptm_inspect_vendor = decoded->metadata.vendor_id;
    s.ptm_inspect_vehicle = decoded->metadata.vehicle_id;
    s.ptm_inspect_lock_mask = decoded->metadata.lock_mask;
    s.ptm_inspect_rom_sum = decoded->metadata.rom_sum;
    s.ptm_inspect_patches = static_cast<std::uint32_t>(decoded->patches.size());
    std::uint64_t total = 0;
    for (auto const &p : decoded->patches) {
        total += p.length;
    }
    s.ptm_inspect_total_bytes = total;

    // Architectural classification.
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> lengths;
    offsets.reserve(decoded->patches.size());
    lengths.reserve(decoded->patches.size());
    for (auto const &p : decoded->patches) {
        offsets.push_back(p.rom_offset);
        lengths.push_back(p.length);
    }
    auto const classifications = st::devices::ets::classify_patches(
        st::devices::ets::default_lf79103p_layer_map(), offsets, lengths);
    auto const summary = st::devices::ets::summarize(classifications);
    for (auto const &row : summary) {
        s.ptm_inspect_layers.push_back(
            {std::string{st::devices::ets::layer_label(row.layer)},
             {row.patch_count, row.total_bytes}});
    }

    // Optional table mapping.
    std::vector<st::library::TableHit> hits;
    if (s.ptm_inspect_def_path[0] != '\0') {
        auto def = st::Definition::from_file(std::filesystem::path{s.ptm_inspect_def_path});
        if (def.has_value()) {
            auto const ranges = st::library::compute_table_ranges(*def);
            hits = st::library::aggregate_table_hits(*decoded, ranges);
            std::size_t const show = std::min<std::size_t>(hits.size(), 15);
            for (std::size_t i = 0; i < show; ++i) {
                s.ptm_inspect_top_tables.push_back(
                    {hits[i].table_name, {hits[i].patch_count, hits[i].total_bytes}});
            }
        }
    }
    s.ptm_inspect_interpretation =
        st::library::interpret_inspect(*decoded, hits);

    // F1 — compare against the connected AP's marriage + backup MD5.
    // Snapshot is nullopt when no AP is connected; in that case both
    // match fields stay Unknown and the modal renders the existing
    // identity bullets without the comparison chips.
    if (auto const snap = ets_status_snapshot(); snap.has_value()) {
        if (!snap->vehicle_id.empty() && !s.ptm_inspect_vehicle.empty()) {
            s.ptm_inspect_ap_vehicle_id = snap->vehicle_id;
            s.ptm_inspect_vehicle_match =
                iequal(snap->vehicle_id, s.ptm_inspect_vehicle)
                    ? AppState::PtmMatch::Match
                    : AppState::PtmMatch::Mismatch;
        }
        if (snap->device_backupcksum.has_value() &&
            !snap->device_backupcksum->empty() &&
            !s.ptm_inspect_rom_sum.empty()) {
            s.ptm_inspect_ap_backupcksum = *snap->device_backupcksum;
            s.ptm_inspect_rom_sum_match =
                iequal(*snap->device_backupcksum, s.ptm_inspect_rom_sum)
                    ? AppState::PtmMatch::Match
                    : AppState::PtmMatch::Mismatch;
        }
    }
    s.ptm_inspect_have_preview = true;
}

void render_match_chip(AppState::PtmMatch m, char const *ap_value) {
    switch (m) {
    case AppState::PtmMatch::Match:
        ImGui::SameLine();
        chip("matches AP", chip_fg_ok(), chip_bg_ok());
        break;
    case AppState::PtmMatch::Mismatch:
        ImGui::SameLine();
        chip("AP mismatch", chip_fg_caution(), chip_bg_caution());
        if (ImGui::IsItemHovered() && ap_value != nullptr) {
            ImGui::SetTooltip("AP reports: %s", ap_value);
        }
        break;
    case AppState::PtmMatch::Unknown:
        break;
    }
}

} // namespace

void render_ptm_inspect_modal(AppState &state) {
    if (state.show_ptm_inspect_modal) {
        ImGui::OpenPopup("\xEE\xA0\x84  Inspect .ptm##ptm_inspect_modal");
        state.show_ptm_inspect_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 420.0f), ImVec2(900.0f, 800.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA0\x84  Inspect .ptm##ptm_inspect_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("Decode a COBB AccessPort .ptm tune file and show its embedded "
                       "metadata + architectural breakdown. Read-only — no project mutation.");
    ImGui::Separator();
    ImGui::Spacing();

    // .ptm file
    ImGui::Text(".ptm file:");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##ptm_path", state.ptm_inspect_ptm_path,
                     sizeof(state.ptm_inspect_ptm_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##ptm_i")) {
        pick_file(state.ptm_inspect_ptm_path, sizeof(state.ptm_inspect_ptm_path),
                  "COBB tune", "ptm");
    }

    // Def-pack (optional)
    ImGui::Spacing();
    ImGui::Text("Definition pack (optional, enables top-tables view):");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##def_path", state.ptm_inspect_def_path,
                     sizeof(state.ptm_inspect_def_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##def_i")) {
        pick_dir(state.ptm_inspect_def_path, sizeof(state.ptm_inspect_def_path));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Decode")) {
        run_decode(state);
    }
    if (!state.ptm_inspect_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(chip_fg_danger(), "Error: %s", state.ptm_inspect_error.c_str());
    }

    if (state.ptm_inspect_have_preview) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Identity");
        ImGui::BulletText("Vendor:    %s", state.ptm_inspect_vendor.c_str());
        ImGui::BulletText("Vehicle:   %s", state.ptm_inspect_vehicle.c_str());
        render_match_chip(state.ptm_inspect_vehicle_match,
                          state.ptm_inspect_ap_vehicle_id.empty()
                              ? nullptr
                              : state.ptm_inspect_ap_vehicle_id.c_str());
        ImGui::BulletText("Lock mask: %u", state.ptm_inspect_lock_mask);
        ImGui::BulletText("ROM sum:   %s", state.ptm_inspect_rom_sum.c_str());
        render_match_chip(state.ptm_inspect_rom_sum_match,
                          state.ptm_inspect_ap_backupcksum.empty()
                              ? nullptr
                              : state.ptm_inspect_ap_backupcksum.c_str());
        ImGui::BulletText("Patches:   %u  (%llu bytes total)",
                          state.ptm_inspect_patches,
                          static_cast<unsigned long long>(state.ptm_inspect_total_bytes));

        if (!state.ptm_inspect_layers.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Architectural breakdown");
            if (ImGui::BeginTable("##layers", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Patches", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();
                for (auto const &row : state.ptm_inspect_layers) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.first.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", row.second.first);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(row.second.second));
                }
                ImGui::EndTable();
            }
        }

        if (!state.ptm_inspect_top_tables.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Top tables modified");
            // Find the max byte count so the bar width scales relative
            // to the heaviest row, giving the user an at-a-glance sense
            // of how dominant the top entries are.
            std::uint64_t max_bytes = 0;
            for (auto const &row : state.ptm_inspect_top_tables) {
                if (row.second.second > max_bytes) {
                    max_bytes = row.second.second;
                }
            }
            if (ImGui::BeginTable("##tables", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, 220.0f))) {
                ImGui::TableSetupColumn("Table", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Patches", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableHeadersRow();
                for (auto const &row : state.ptm_inspect_top_tables) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.first.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", row.second.first);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu",
                                static_cast<unsigned long long>(row.second.second));
                    ImGui::TableSetColumnIndex(3);
                    float const frac =
                        max_bytes == 0
                            ? 0.0f
                            : static_cast<float>(
                                  static_cast<double>(row.second.second) /
                                  static_cast<double>(max_bytes));
                    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), "");
                }
                ImGui::EndTable();
            }
        } else if (state.ptm_inspect_def_path[0] == '\0') {
            ImGui::Spacing();
            ImGui::TextColored(chip_fg_muted(),
                               "Pick a definition pack to see per-table breakdown.");
        }

        if (!state.ptm_inspect_interpretation.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Interpretation");
            for (auto const &line : state.ptm_inspect_interpretation) {
                ImGui::Bullet();
                ImGui::TextWrapped("%s", line.c_str());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        clear_preview(state);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
