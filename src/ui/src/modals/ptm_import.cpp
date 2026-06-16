// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// PTM Import wizard modal. Picks a .ptm file (and base ROM, optional
// def-pack), decodes it inline via the cipher chain, shows a preview
// of the embedded metadata, and on commit writes a Project::open-
// compatible skeleton to disk:
//
//   <out_dir>/
//     project.toml      [project] + [ptm_metadata] + [project.definition]
//     source.bin        copy of the chosen base ROM
//     working.bin       source.bin with the .ptm's patches applied
//     edits.toml        schema_version=2 + cursor=N + one [[edit]] per
//                       imported patch (ByteEdit form, tagged "ptm_import"
//                       for batch-undo via History::undo_while_tag).
//     ptm_patches.toml  round-trip-preserving raw patch list
//
// On successful import the modal loads the resulting project into the
// current AppState — closes the loop from `ap3 pull` through to a
// loaded project ready to edit.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/core/crc32.hpp"
#include "st/defs.hpp"
#include "st/devices/ets/architectural_classifier.hpp"
#include "st/devices/ets/ptm_cipher.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/project.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
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

// Crude TOML string escape — same shape as the CLI's. Keeps the
// implementations independent so a CLI refactor doesn't ripple here.
std::string toml_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                (void)std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<int>(c));
                out.append(buf);
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
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
    s.ptm_import_preview_vendor.clear();
    s.ptm_import_preview_vehicle.clear();
    s.ptm_import_preview_patches = 0;
    s.ptm_import_preview_bytes = 0;
    s.ptm_import_error.clear();
    s.ptm_import_success.clear();
}

bool decode_preview(AppState &s) {
    clear_preview(s);
    if (s.ptm_import_ptm_path[0] == '\0') {
        s.ptm_import_error = "Pick a .ptm file first.";
        return false;
    }
    auto const ptm_bytes = read_all(s.ptm_import_ptm_path);
    if (ptm_bytes.empty()) {
        s.ptm_import_error = std::string{"Cannot read "} + s.ptm_import_ptm_path;
        return false;
    }
    auto contents = st::devices::ets::cipher::decrypt_ptm(ptm_bytes);
    if (!contents.has_value()) {
        s.ptm_import_error = std::string{"decrypt_ptm: "} +
                             contents.error().to_string();
        return false;
    }
    auto decoded = st::library::decode_ptm_xml(contents->inner_xml);
    if (!decoded.has_value()) {
        s.ptm_import_error = std::string{"decode_ptm_xml: "} +
                             decoded.error().to_string();
        return false;
    }
    s.ptm_import_preview_vendor = decoded->metadata.vendor_id;
    s.ptm_import_preview_vehicle = decoded->metadata.vehicle_id;
    s.ptm_import_preview_patches = static_cast<std::uint32_t>(decoded->patches.size());
    std::uint64_t total = 0;
    for (auto const &p : decoded->patches) {
        total += p.length;
    }
    s.ptm_import_preview_bytes = total;
    return true;
}

bool commit_import(AppState &s) {
    s.ptm_import_error.clear();
    s.ptm_import_success.clear();

    if (s.ptm_import_ptm_path[0] == '\0' || s.ptm_import_base_rom[0] == '\0' ||
        s.ptm_import_out_dir[0] == '\0') {
        s.ptm_import_error = "Pick a .ptm file, a base ROM, and an output directory.";
        return false;
    }
    auto const ptm_bytes = read_all(s.ptm_import_ptm_path);
    if (ptm_bytes.empty()) {
        s.ptm_import_error = "Cannot read .ptm file.";
        return false;
    }
    auto const base_bytes = read_all(s.ptm_import_base_rom);
    if (base_bytes.empty()) {
        s.ptm_import_error = "Cannot read base ROM file.";
        return false;
    }

    auto contents = st::devices::ets::cipher::decrypt_ptm(ptm_bytes);
    if (!contents.has_value()) {
        s.ptm_import_error = std::string{"decrypt_ptm: "} +
                             contents.error().to_string();
        return false;
    }
    auto decoded = st::library::decode_ptm_xml(contents->inner_xml);
    if (!decoded.has_value()) {
        s.ptm_import_error = std::string{"decode_ptm_xml: "} +
                             decoded.error().to_string();
        return false;
    }

    std::filesystem::path const out_dir{s.ptm_import_out_dir};
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        s.ptm_import_error = "create_directories: " + ec.message();
        return false;
    }

    auto const source_crc = st::crc32(base_bytes);
    // Apply patches to a working copy.
    auto working_bytes = base_bytes;
    std::size_t applied = 0, skipped = 0;
    for (auto const &p : decoded->patches) {
        if (p.bytes.empty() || p.rom_offset >= working_bytes.size() ||
            p.rom_offset + p.bytes.size() > working_bytes.size()) {
            ++skipped;
            continue;
        }
        std::memcpy(working_bytes.data() + p.rom_offset, p.bytes.data(), p.bytes.size());
        ++applied;
    }
    // Symmetric to the CLI's a4b36d6 refuse: every patch overflowed
    // the base ROM means the user picked the wrong base. Silently
    // writing an unmodified working.bin + a "succeeded" toast would be
    // misleading. Per the AppState's modal-error convention, set
    // ptm_import_error so render_ptm_import_modal surfaces it inline
    // (status bar gets hidden behind the modal — see feedback memory).
    if (applied == 0 && skipped > 0) {
        s.ptm_import_error =
            "Every patch (" + std::to_string(skipped) + ") falls outside the "
            "base ROM (" + std::to_string(base_bytes.size()) + " bytes). The "
            "selected base probably doesn't match this tune — check the "
            "vehicle_id against your base ROM's CID.";
        return false;
    }
    auto const working_crc = st::crc32(working_bytes);

    // Write source.bin + working.bin.
    {
        std::ofstream src{out_dir / "source.bin", std::ios::binary};
        src.write(reinterpret_cast<char const *>(base_bytes.data()),
                  static_cast<std::streamsize>(base_bytes.size()));
        std::ofstream wrk{out_dir / "working.bin", std::ios::binary};
        wrk.write(reinterpret_cast<char const *>(working_bytes.data()),
                  static_cast<std::streamsize>(working_bytes.size()));
    }
    // Write edits.toml — one [[edit]] per applied patch in ByteEdit form,
    // tagged "ptm_import" so the whole import can be reverted as one
    // transaction via History::undo_while_tag. Mirrors the CLI ptm-
    // import behavior so GUI- and CLI-imported projects have equivalent
    // history. Description here is minimal (no table-name lookup); CLI
    // import populates table_name + layer when --def is wired.
    {
        std::ofstream f{out_dir / "edits.toml"};
        f << "# Edit history for this SubuwuTuner project. Generated by\n";
        f << "# the GUI ptm-import modal — one [[edit]] per .ptm patch,\n";
        f << "# tagged \"ptm_import\" for batch undo.\n";
        f << "schema_version = 2\n";
        f << "cursor = " << applied << "\n";
        auto running = base_bytes;
        for (auto const &p : decoded->patches) {
            if (p.bytes.empty() || p.rom_offset >= running.size() ||
                p.rom_offset + p.bytes.size() > running.size()) {
                continue;
            }
            char addr_buf[24];
            (void)std::snprintf(addr_buf, sizeof(addr_buf), "0x%08X", p.rom_offset);
            f << "\n[[edit]]\n";
            f << "  description = \"ptm import: @" << addr_buf << " ("
              << p.length << "B)\"\n";
            f << "  tag         = \"ptm_import\"\n";
            for (std::size_t i = 0; i < p.bytes.size(); ++i) {
                auto const addr = static_cast<std::size_t>(p.rom_offset) + i;
                f << "  [[edit.byte_changes]]\n";
                f << "    address = " << addr << "\n";
                f << "    before  = " << static_cast<unsigned>(running[addr]) << "\n";
                f << "    after   = " << static_cast<unsigned>(p.bytes[i]) << "\n";
            }
            std::memcpy(running.data() + p.rom_offset, p.bytes.data(), p.bytes.size());
        }
    }
    // Write project.toml.
    {
        std::ofstream f{out_dir / "project.toml"};
        f << "[project]\n";
        f << "schema_version = 1\n";
        f << "display_name   = \"" << toml_escape(decoded->metadata.vehicle_id) << "\"\n";
        f << "notes          = \"imported from " << toml_escape(s.ptm_import_ptm_path) << "\"\n";
        f << "policy_profile = \"motorsport-only\"\n";
        f << "active_rom_id  = \"\"\n";
        f << "\n[project.source_rom]\n";
        f << "path  = \"source.bin\"\n";
        f << "crc32 = " << source_crc << "\n";
        f << "\n[project.working_rom]\n";
        f << "path  = \"working.bin\"\n";
        f << "crc32 = " << working_crc << "\n";
        f << "\n[project.definition]\n";
        if (s.ptm_import_def_path[0] != '\0') {
            auto const abs = std::filesystem::weakly_canonical(
                std::filesystem::path{s.ptm_import_def_path});
            f << "path = \"" << toml_escape(abs.string()) << "\"\n";
        } else {
            f << "path = \"\"  # def-pack not picked — set this before opening\n";
        }
        f << "\n[ptm_metadata]\n";
        f << "vendor_id      = \"" << toml_escape(decoded->metadata.vendor_id) << "\"\n";
        f << "vehicle_id     = \"" << toml_escape(decoded->metadata.vehicle_id) << "\"\n";
        f << "lock_mask      = " << decoded->metadata.lock_mask << "\n";
        f << "rom_sum        = \"" << toml_escape(decoded->metadata.rom_sum) << "\"\n";
        f << "save_date_time = \"" << toml_escape(decoded->metadata.save_date_time) << "\"\n";
        f << "imported_from  = \"" << toml_escape(s.ptm_import_ptm_path) << "\"\n";
    }
    // Write ptm_patches.toml.
    {
        std::ofstream f{out_dir / "ptm_patches.toml"};
        f << "# Generated by SubuwuTuner GUI ptm import. One [[patch]] per\n";
        f << "# <patch> element from the source .ptm's PrivateData XML.\n\n";
        for (auto const &p : decoded->patches) {
            auto const b64 = st::devices::ets::cipher::base64_encode(p.bytes);
            f << "[[patch]]\n";
            f << "rom_offset = " << p.rom_offset << "\n";
            f << "ram_offset = " << p.ram_offset << "\n";
            f << "length     = " << p.length << "\n";
            f << "bytes_b64  = \"" << b64 << "\"\n\n";
        }
    }

    // Try to open the project we just wrote. If the def-pack wasn't
    // picked the open will fail; surface the error inline so the user
    // can fix it without re-running.
    auto opened = st::Project::open(out_dir);
    if (!opened.has_value()) {
        s.ptm_import_error = "Wrote skeleton, but Project::open failed: " +
                             opened.error().to_string() +
                             "\nFill in [project.definition].path in project.toml and re-open.";
        s.ptm_import_success = "Wrote project skeleton to " + out_dir.string() +
                               "  (" + std::to_string(applied) + " patches applied, " +
                               std::to_string(skipped) + " skipped)";
        return false;
    }
    s.project = std::move(*opened);
    s.ptm_import_success = "Imported " + out_dir.string() + "  (" +
                           std::to_string(applied) + " patches applied, " +
                           std::to_string(skipped) + " skipped)";
    return true;
}

} // namespace

void render_ptm_import_modal(AppState &state) {
    if (state.show_ptm_import_modal) {
        ImGui::OpenPopup("\xEE\x86\x97  Import .ptm##ptm_import_modal");
        state.show_ptm_import_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 460.0f), ImVec2(900.0f, 800.0f));
    if (!ImGui::BeginPopupModal("\xEE\x86\x97  Import .ptm##ptm_import_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("Decode a COBB AccessPort .ptm tune file into a SubuwuTuner project. "
                       "Requires ST_ENABLE_COBB_AP_CIPHER=ON at build time.");
    ImGui::Separator();
    ImGui::Spacing();

    // .ptm file
    ImGui::Text(".ptm file:");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##ptm_path", state.ptm_import_ptm_path,
                     sizeof(state.ptm_import_ptm_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##ptm")) {
        pick_file(state.ptm_import_ptm_path, sizeof(state.ptm_import_ptm_path),
                  "COBB tune", "ptm");
    }

    // Base ROM
    ImGui::Spacing();
    ImGui::Text("Base ROM (the stock .bin the tune targets):");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##base_rom", state.ptm_import_base_rom,
                     sizeof(state.ptm_import_base_rom));
    ImGui::SameLine();
    if (ImGui::Button("Browse##rom")) {
        pick_file(state.ptm_import_base_rom, sizeof(state.ptm_import_base_rom),
                  "ROM image", "bin");
    }

    // Def-pack (optional but recommended)
    ImGui::Spacing();
    ImGui::Text("Definition pack (optional, but required to open the project):");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##def_path", state.ptm_import_def_path,
                     sizeof(state.ptm_import_def_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse##def")) {
        pick_dir(state.ptm_import_def_path, sizeof(state.ptm_import_def_path));
    }

    // Output dir
    ImGui::Spacing();
    ImGui::Text("Output directory:");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::InputText("##out_dir", state.ptm_import_out_dir,
                     sizeof(state.ptm_import_out_dir));
    ImGui::SameLine();
    if (ImGui::Button("Browse##out")) {
        pick_dir(state.ptm_import_out_dir, sizeof(state.ptm_import_out_dir));
    }

    // Preview button + result
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Decode preview")) {
        decode_preview(state);
    }
    if (!state.ptm_import_preview_vendor.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(chip_fg_ok(), "OK");
        ImGui::Spacing();
        ImGui::Text("Vendor:   %s", state.ptm_import_preview_vendor.c_str());
        ImGui::Text("Vehicle:  %s", state.ptm_import_preview_vehicle.c_str());
        ImGui::Text("Patches:  %u  (%llu bytes total)",
                    state.ptm_import_preview_patches,
                    static_cast<unsigned long long>(state.ptm_import_preview_bytes));
    }
    if (!state.ptm_import_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "Error: %s", state.ptm_import_error.c_str());
    }
    if (!state.ptm_import_success.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_ok(), "%s", state.ptm_import_success.c_str());
    }

    // Commit / Cancel
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool const can_commit = state.ptm_import_ptm_path[0] != '\0' &&
                            state.ptm_import_base_rom[0] != '\0' &&
                            state.ptm_import_out_dir[0] != '\0';
    if (!can_commit) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Import")) {
        if (commit_import(state)) {
            ImGui::CloseCurrentPopup();
        }
    }
    if (!can_commit) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        clear_preview(state);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
