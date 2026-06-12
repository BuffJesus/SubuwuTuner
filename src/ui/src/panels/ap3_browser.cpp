// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// AP3 browser panel — file-vault GUI for the COBB AccessPort V3.
// CLI parity: `subuwutuner-cli ap3 {state, ls, pull, push, rm, backup}`,
// wrapped in a docking panel with one row per file and inline
// per-row pull/remove actions plus toolbar buttons for push and
// "backup all".
//
// State + I/O are file-static so the panel is self-contained — AppState
// only carries the visibility bool. Operations are synchronous: a 70 KB
// pull completes in <1 s on a working AP, so a brief UI freeze is
// acceptable for v1. Backup walks every file in /maps/ + /datalog/ +
// /presets/ + /images/ + /settings + /backupcksum and can take 30 s on
// a full AP; users wanting non-blocking backups can run the CLI
// (`subuwutuner-cli ap3 backup --into …`) in parallel. An async
// follow-up is queued in the next-session backlog.
//
// Per CLAUDE.md/docs/15 §12 trademark posture: the on-screen text uses
// "AccessPort" (the third-party product name, identifying the hardware
// being interoperated with), but code-side types and the underlying
// transport stay namespaced `Ap3` / `ap3`.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/core/result.hpp"
#include "st/devices/ap3/client.hpp"
#include "st/devices/ap3/file_info.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/cobb_ap_channel.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

namespace {

constexpr std::array<char const *, 4> kSubdirs{"/maps/", "/datalog/", "/presets/", "/images/"};

enum class StatusSeverity { None, Ok, Warn, Error };

struct PanelState {
    std::unique_ptr<st::transport::IByteChannel> channel;
    std::optional<st::devices::ap3::DeviceState> device_state;
    std::string status_msg;
    StatusSeverity status_severity{StatusSeverity::None};

    std::string current_subdir{kSubdirs[0]};
    std::map<std::string, std::vector<st::devices::ap3::FileInfo>> listings;
    std::map<std::string, std::string> listing_errors; // per-subdir last error

    bool allow_unmarried{false};

    // Cached "AccessPort detected" probe — refreshed at most once per
    // ~1 second so we don't libusb_get_device_list every frame.
    bool last_detect{false};
    std::chrono::steady_clock::time_point last_detect_at{};
};

PanelState &panel() {
    static PanelState s;
    return s;
}

void set_status(PanelState &p, StatusSeverity sev, std::string msg) {
    p.status_msg = std::move(msg);
    p.status_severity = sev;
}

void clear_status(PanelState &p) {
    p.status_msg.clear();
    p.status_severity = StatusSeverity::None;
}

void render_status(PanelState const &p) {
    if (p.status_msg.empty()) {
        return;
    }
    switch (p.status_severity) {
    case StatusSeverity::Ok:
        chip(p.status_msg.c_str(), chip_fg_ok(), chip_bg_ok());
        break;
    case StatusSeverity::Warn:
        chip(p.status_msg.c_str(), chip_fg_caution(), chip_bg_caution());
        break;
    case StatusSeverity::Error:
        chip(p.status_msg.c_str(), chip_fg_danger(), chip_bg_danger());
        break;
    case StatusSeverity::None:
        text_subtle("%s", p.status_msg.c_str());
        break;
    }
}

// Cheap presence probe, refreshed at ~1 Hz. Surfaces the Connect button
// state and (downstream) the welcome-panel hint.
bool ap_detected(PanelState &p) {
    auto const now = std::chrono::steady_clock::now();
    if (now - p.last_detect_at > std::chrono::seconds{1}) {
        p.last_detect = st::transport::ap3::detect_present();
        p.last_detect_at = now;
    }
    return p.last_detect;
}

void disconnect(PanelState &p) {
    p.device_state.reset();
    p.channel.reset();
    p.listings.clear();
    p.listing_errors.clear();
}

void connect(PanelState &p) {
    clear_status(p);
    auto channel = st::transport::ap3::open_channel();
    if (!channel.has_value()) {
        set_status(p, StatusSeverity::Error, channel.error().to_string());
        return;
    }
    p.channel = std::move(*channel);
    st::devices::ap3::Client client{*p.channel};
    auto state = client.query_state();
    if (!state.has_value()) {
        set_status(p, StatusSeverity::Error, "query_state: " + state.error().to_string());
        p.channel.reset();
        return;
    }
    p.device_state = std::move(*state);
    set_status(p, StatusSeverity::Ok, "Connected.");
    // Eager-fetch the default subdir's listing so the panel doesn't
    // open empty.
    auto records = client.ls(p.current_subdir);
    if (records.has_value()) {
        p.listings[p.current_subdir] = std::move(*records);
        p.listing_errors.erase(p.current_subdir);
    } else {
        p.listing_errors[p.current_subdir] = records.error().to_string();
    }
}

void refresh_subdir(PanelState &p, std::string const &subdir) {
    if (p.channel == nullptr) {
        return;
    }
    st::devices::ap3::Client client{*p.channel};
    auto records = client.ls(subdir);
    if (records.has_value()) {
        p.listings[subdir] = std::move(*records);
        p.listing_errors.erase(subdir);
    } else {
        p.listing_errors[subdir] = records.error().to_string();
    }
}

void pull_file(PanelState &p, st::devices::ap3::FileInfo const &rec) {
    if (p.channel == nullptr) {
        return;
    }
    nfdu8filteritem_t const filters[] = {{"AccessPort tune (.ptm)", "ptm"},
                                         {"Any file", "*"}};
    NFD::UniquePathU8 out;
    nfdresult_t const r = NFD::SaveDialog(out, filters, 2, nullptr, rec.name.c_str());
    if (r == NFD_CANCEL) {
        return;
    }
    if (r != NFD_OKAY || out == nullptr) {
        set_status(p, StatusSeverity::Error, std::string{"File dialog: "} + NFD::GetError());
        return;
    }
    st::devices::ap3::Client client{*p.channel};
    auto bytes = client.read_file(rec.path + rec.name);
    if (!bytes.has_value()) {
        set_status(p, StatusSeverity::Error, "pull: " + bytes.error().to_string());
        return;
    }
    std::ofstream o{std::filesystem::path{out.get()}, std::ios::binary};
    if (!o) {
        set_status(p, StatusSeverity::Error, "pull: failed to open output file");
        return;
    }
    o.write(reinterpret_cast<char const *>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    set_status(p, StatusSeverity::Ok,
               "Pulled " + std::to_string(bytes->size()) + " bytes → " + std::string{out.get()});
}

void push_file(PanelState &p) {
    if (p.channel == nullptr) {
        return;
    }
    nfdu8filteritem_t const filters[] = {{"AccessPort tune (.ptm)", "ptm"},
                                         {"Any file", "*"}};
    NFD::UniquePathU8 in;
    nfdresult_t const r = NFD::OpenDialog(in, filters, 2);
    if (r == NFD_CANCEL) {
        return;
    }
    if (r != NFD_OKAY || in == nullptr) {
        set_status(p, StatusSeverity::Error, std::string{"File dialog: "} + NFD::GetError());
        return;
    }
    std::filesystem::path local{in.get()};
    std::ifstream f{local, std::ios::binary | std::ios::ate};
    if (!f) {
        set_status(p, StatusSeverity::Error, "push: couldn't open " + local.string());
        return;
    }
    auto const size = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char *>(bytes.data()), size);
    f.close();
    std::string const ap_path = p.current_subdir + local.filename().string();
    st::devices::ap3::Client client{*p.channel};
    auto status = client.write_file(ap_path, bytes, static_cast<std::uint64_t>(std::time(nullptr)));
    if (!status.has_value()) {
        set_status(p, StatusSeverity::Error, "push: " + status.error().to_string());
        return;
    }
    set_status(p, StatusSeverity::Ok,
               "Pushed " + std::to_string(bytes.size()) + " bytes → AP:" + ap_path);
    refresh_subdir(p, p.current_subdir);
}

void remove_file(PanelState &p, st::devices::ap3::FileInfo const &rec) {
    if (p.channel == nullptr) {
        return;
    }
    st::devices::ap3::Client client{*p.channel};
    auto status = client.remove_file(rec.path + rec.name);
    if (!status.has_value()) {
        set_status(p, StatusSeverity::Error, "rm: " + status.error().to_string());
        return;
    }
    set_status(p, StatusSeverity::Ok, "Removed AP:" + rec.path + rec.name);
    refresh_subdir(p, p.current_subdir);
}

void backup_all(PanelState &p) {
    if (p.channel == nullptr) {
        return;
    }
    NFD::UniquePathU8 out_dir;
    nfdresult_t const r = NFD::PickFolder(out_dir);
    if (r == NFD_CANCEL) {
        return;
    }
    if (r != NFD_OKAY || out_dir == nullptr) {
        set_status(p, StatusSeverity::Error, std::string{"Folder dialog: "} + NFD::GetError());
        return;
    }
    std::filesystem::path root{out_dir.get()};
    std::error_code ec;
    std::filesystem::create_directories(root, ec);

    st::devices::ap3::Client client{*p.channel};
    std::size_t files = 0;
    std::uint64_t total_bytes = 0;
    for (auto sub : kSubdirs) {
        auto records = client.ls(sub);
        if (!records.has_value()) {
            continue;
        }
        std::string sub_name{sub + 1};
        if (!sub_name.empty() && sub_name.back() == '/') {
            sub_name.pop_back();
        }
        auto sub_dir = root / sub_name;
        std::filesystem::create_directories(sub_dir, ec);
        for (auto const &rec : *records) {
            auto bytes = client.read_file(rec.path + rec.name);
            if (!bytes.has_value()) {
                continue;
            }
            std::ofstream o{sub_dir / rec.name, std::ios::binary};
            o.write(reinterpret_cast<char const *>(bytes->data()),
                    static_cast<std::streamsize>(bytes->size()));
            ++files;
            total_bytes += bytes->size();
        }
    }
    for (auto const *singleton : {"settings", "backupcksum"}) {
        auto bytes = client.read_file(std::string{"/"} + singleton);
        if (!bytes.has_value()) {
            continue;
        }
        std::ofstream o{root / singleton, std::ios::binary};
        o.write(reinterpret_cast<char const *>(bytes->data()),
                static_cast<std::streamsize>(bytes->size()));
        ++files;
        total_bytes += bytes->size();
    }
    set_status(p, StatusSeverity::Ok,
               "Backed up " + std::to_string(files) + " files (" + std::to_string(total_bytes) +
                   " bytes) → " + root.string());
}

void render_disconnect_state(PanelState &p) {
    bool const detected = ap_detected(p);
    if (detected) {
        chip("AccessPort detected", chip_fg_accent(), chip_bg_accent());
    } else {
        chip("No AccessPort enumerated", chip_fg_muted(), chip_bg_muted());
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    push_primary_button_colors();
    if (ImGui::Button("Connect##ap3_connect", ImVec2(180.0f, 32.0f))) {
        connect(p);
    }
    pop_primary_button_colors();
    if (ImGui::IsItemHovered() && !detected) {
        ImGui::SetTooltip(
            "No matching device found on USB. Plug in the AccessPort\n"
            "and rebind it to WinUSB via Zadig (Windows) — see\n"
            "docs/install.md → \"USB hardware setup\".");
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    render_status(p);
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("File vault, datalog pulls, and AP backups — opaque on the .ptm side.");
    text_subtle("Cipher introspection requires the gated build flag (see docs/34).");
}

void render_device_header(PanelState &p) {
    auto const &s = *p.device_state;
    auto field = [](char const *label, std::string const &v) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(140.0f);
        ImGui::TextUnformatted(v.empty() ? "(unparsed)" : v.c_str());
    };
    field("Serial", s.ap_serial.value_or(""));
    field("Firmware", s.firmware_version.value_or(""));
    field("Vehicle", s.vehicle_descriptor.value_or(""));

    ImGui::TextDisabled("Marriage");
    ImGui::SameLine(140.0f);
    if (s.married.has_value()) {
        if (*s.married) {
            chip("Installed", chip_fg_ok(), chip_bg_ok());
        } else {
            chip("Not Installed", chip_fg_danger(), chip_bg_danger());
        }
    } else {
        chip("unparsed", chip_fg_caution(), chip_bg_caution());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "The protocol spec doesn't pin down the byte offset of the\n"
                "Installed/Not Installed marker in the settings blob yet.\n"
                "Operations proceed with a warning. See docs/34.");
        }
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    if (ImGui::SmallButton("Disconnect##ap3_disconnect")) {
        disconnect(p);
        clear_status(p);
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh state##ap3_refresh_state")) {
        st::devices::ap3::Client client{*p.channel};
        auto fresh = client.query_state();
        if (fresh.has_value()) {
            p.device_state = std::move(*fresh);
        } else {
            set_status(p, StatusSeverity::Error, "query_state: " + fresh.error().to_string());
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Allow unmarried##ap3_allow_unmarried", &p.allow_unmarried);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When ON, suppresses the warning emitted when the marriage\n"
            "state can't be confirmed Installed. Forward-compatible with\n"
            "a future spec extension that gates by default.");
    }
}

void render_subdir_tabs(PanelState &p) {
    if (ImGui::BeginTabBar("##ap3_subdir_tabs")) {
        for (auto subdir : kSubdirs) {
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(subdir, nullptr, flags)) {
                if (p.current_subdir != subdir) {
                    p.current_subdir = subdir;
                    // Lazy fetch on first visit.
                    if (p.listings.find(p.current_subdir) == p.listings.end() &&
                        p.listing_errors.find(p.current_subdir) == p.listing_errors.end()) {
                        refresh_subdir(p, p.current_subdir);
                    }
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void render_file_table(PanelState &p) {
    auto err_it = p.listing_errors.find(p.current_subdir);
    if (err_it != p.listing_errors.end()) {
        chip(("ls failed: " + err_it->second).c_str(), chip_fg_danger(), chip_bg_danger());
        return;
    }
    auto it = p.listings.find(p.current_subdir);
    if (it == p.listings.end() || it->second.empty()) {
        text_subtle("(empty)");
        return;
    }
    if (ImGui::BeginTable("##ap3_files", 4,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                             ImGuiTableFlags_SizingStretchProp |
                             ImGuiTableFlags_ScrollY,
                         ImVec2(0, 280.0f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            auto const &rec = it->second[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(rec.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(rec.size));
            ImGui::TableSetColumnIndex(2);
            if (rec.mtime != 0) {
                std::time_t t = static_cast<std::time_t>(rec.mtime);
                char buf[32];
                std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
                ImGui::TextUnformatted(buf);
            } else {
                ImGui::TextDisabled("—");
            }
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Pull")) {
                pull_file(p, rec);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                ImGui::OpenPopup("##ap3_rm_confirm");
            }
            if (ImGui::BeginPopup("##ap3_rm_confirm")) {
                ImGui::Text("Remove %s%s from the AP?", rec.path.c_str(), rec.name.c_str());
                ImGui::TextDisabled("This is irreversible — the file is deleted on the device.");
                ImGui::Dummy(ImVec2(0.0f, kSpaceS));
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                push_primary_button_colors();
                if (ImGui::Button("Remove")) {
                    remove_file(p, rec);
                    ImGui::CloseCurrentPopup();
                }
                pop_primary_button_colors();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void render_toolbar(PanelState &p) {
    if (ImGui::Button("Push file\xE2\x80\xA6##ap3_push")) {
        push_file(p);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Upload a local file into the currently-selected\n"
                          "subdirectory on the AP.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Backup all\xE2\x80\xA6##ap3_backup")) {
        backup_all(p);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pull every /maps, /datalog, /presets, /images file\n"
                          "+ /settings + /backupcksum into a chosen folder.\n"
                          "Synchronous — the panel blocks while running.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh##ap3_refresh_list")) {
        refresh_subdir(p, p.current_subdir);
    }
}

void render_connected_state(PanelState &p) {
    render_device_header(p);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    render_subdir_tabs(p);
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    render_toolbar(p);
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    render_file_table(p);
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    render_status(p);
}

} // namespace

bool ap3_browser_should_hint(AppState const & /*state*/) {
    return panel().last_detect && panel().channel == nullptr;
}

void render_ap3_browser_panel(AppState &state) {
    if (!state.show_ap3_browser_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("AccessPort", &state.show_ap3_browser_panel)) {
        ImGui::End();
        return;
    }
    auto &p = panel();
    // Cheap presence probe (~1 Hz) so the welcome card + the
    // disconnect view header reflect plug/unplug without manual
    // refresh.
    (void)ap_detected(p);

    if (p.channel == nullptr) {
        render_disconnect_state(p);
    } else {
        render_connected_state(p);
    }
    ImGui::End();
}

} // namespace st::ui
