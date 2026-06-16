// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Diagnostics modal — GUI surface for the install-health checks that
// `subuwutuner-cli doctor` provides at the CLI. Reimplements the
// checks against the same domain-layer functions the CLI uses; does
// NOT shell out to the CLI binary (so it works in dev trees where
// only the GUI is built, and avoids subprocess + JSON-parse cost).
//
// Each row is a traffic-light + label + result text + optional hint.
// Refresh button re-runs every check. Modal opens via the menubar
// (Tools → Run Diagnostics…) or the command palette (future entry).

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/config.hpp"
#include "st/core/version.hpp"
#include "st/transport/ets_channel.hpp"

#include <imgui.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace st::ui {

namespace {

enum class CheckStatus { Ok, Warn, Fail, Info };

struct CheckRow {
    std::string label;
    CheckStatus status{CheckStatus::Info};
    std::string detail;
    std::string hint; // optional — empty = no hint line
};

struct DoctorReport {
    std::vector<CheckRow> rows;
    bool any_fail{false};
    bool any_warn{false};
};

// Count TOML files under a directory tree (depth-bounded — we don't
// want to spider an arbitrary tree). Returns count, with `failed`
// incremented for entries that exist but aren't loadable.
struct PackCount {
    std::size_t toml_files{0};
    bool exists{false};
};

PackCount count_packs_under(std::filesystem::path const &root) {
    PackCount c;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return c;
    }
    c.exists = true;
    // Recursive walk capped at depth 3 — the convention is
    // <root>/<platform>/<cid>.toml. Deeper than 3 is misuse.
    for (auto it = std::filesystem::recursive_directory_iterator{root, ec};
         it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().extension() == ".toml") {
            ++c.toml_files;
        }
    }
    return c;
}

DoctorReport run_checks() {
    DoctorReport rep;
    auto push = [&](std::string label, CheckStatus status, std::string detail,
                    std::string hint = {}) {
        if (status == CheckStatus::Fail) rep.any_fail = true;
        if (status == CheckStatus::Warn) rep.any_warn = true;
        rep.rows.push_back({std::move(label), status, std::move(detail), std::move(hint)});
    };

    // 1. Build identity — always pass; informational.
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "v%.*s, built " __DATE__ " " __TIME__,
                      static_cast<int>(st::Version::string().size()),
                      st::Version::string().data());
        push("SubuwuTuner build", CheckStatus::Info, buf);
    }

    // 2. Compiler — informational.
    {
#if defined(__clang__)
        char buf[40];
        std::snprintf(buf, sizeof buf, "Clang %d.%d.%d",
                      __clang_major__, __clang_minor__, __clang_patchlevel__);
        push("Compiler", CheckStatus::Info, buf);
#elif defined(__GNUC__)
        char buf[40];
        std::snprintf(buf, sizeof buf, "GCC %d.%d.%d",
                      __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
        push("Compiler", CheckStatus::Info, buf);
#elif defined(_MSC_VER)
        char buf[40];
        std::snprintf(buf, sizeof buf, "MSVC %d", _MSC_VER);
        push("Compiler", CheckStatus::Info, buf);
#else
        push("Compiler", CheckStatus::Info, "unknown");
#endif
    }

    // 3. .ptm cipher build state.
    {
#ifdef ST_ETS_HAVE_CIPHER
        push(".ptm cipher (build flag)", CheckStatus::Ok,
             "ON — ptm inspect/diff/import available.");
#else
        push(".ptm cipher (build flag)", CheckStatus::Warn,
             "OFF — `.ptm` decode disabled.",
             "Rebuild with -DST_ENABLE_COBB_AP_CIPHER=ON to enable.");
#endif
    }

    // 4. Config file loadability.
    {
        auto cfg = st::config::Config::load();
        if (cfg.has_value()) {
            push("User config", CheckStatus::Ok,
                 cfg->source_path().string());
        } else {
            push("User config", CheckStatus::Warn,
                 cfg.error().to_string(),
                 "Defaults are in effect. Settings → Save creates a fresh "
                 "config file.");
        }
    }

    // 5. Definitions root — existence + pack count.
    {
        auto cfg = st::config::Config::load();
        auto const root = cfg.has_value()
                              ? cfg->paths().definitions_root
                              : st::config::Config::defaults().paths().definitions_root;
        auto const packs = count_packs_under(root);
        if (!packs.exists) {
            push("Definitions root", CheckStatus::Warn,
                 root.string() + " (does not exist)",
                 "See docs/install.md for the per-platform convention dir + "
                 "how to drop your pack.toml there.");
        } else if (packs.toml_files == 0) {
            push("Definitions root", CheckStatus::Warn,
                 root.string() + " (0 TOML files)",
                 "Drop your definition pack TOML(s) into this dir, or pass "
                 "--def <path> explicitly to the CLI.");
        } else {
            char buf[64];
            std::snprintf(buf, sizeof buf, " (%zu TOML files)", packs.toml_files);
            push("Definitions root", CheckStatus::Ok, root.string() + buf);
        }
    }

    // 6. ROM-dump root.
    {
        auto cfg = st::config::Config::load();
        auto const root = cfg.has_value()
                              ? cfg->paths().rom_dump_root
                              : st::config::Config::defaults().paths().rom_dump_root;
        std::error_code ec;
        if (std::filesystem::is_directory(root, ec) && !ec) {
            push("ROM dump root", CheckStatus::Ok, root.string());
        } else {
            push("ROM dump root", CheckStatus::Info,
                 root.string() + " (does not exist)",
                 "Created on first ROM pull. Settings → Paths to change.");
        }
    }

    // 7. AccessPort presence — fast libusb-device-list scan.
    {
        bool const detected = st::transport::ets::detect_present();
        if (detected) {
            push("AccessPort", CheckStatus::Ok,
                 "enumerated on USB (VID 0x1A84 PID 0x0121)");
        } else {
            push("AccessPort", CheckStatus::Info,
                 "not enumerated",
                 "If you have an AP plugged in: on Windows, run Zadig to "
                 "rebind to WinUSB (docs/install.md). On Linux, install the "
                 "udev rule. On macOS, no setup required.");
        }
    }

    return rep;
}

ImVec4 dot_color(CheckStatus s) {
    switch (s) {
    case CheckStatus::Ok:
        return chip_fg_ok();
    case CheckStatus::Warn:
        return chip_fg_caution();
    case CheckStatus::Fail:
        return chip_fg_danger();
    case CheckStatus::Info:
    default:
        return chip_fg_muted();
    }
}

char const *dot_glyph(CheckStatus s) {
    switch (s) {
    case CheckStatus::Ok:
        return "\xE2\x97\x8F"; // ● black circle
    case CheckStatus::Warn:
        return "\xE2\x97\x8F";
    case CheckStatus::Fail:
        return "\xE2\x97\x8F";
    case CheckStatus::Info:
    default:
        return "\xE2\x97\x8B"; // ○ white circle
    }
}

} // namespace

void render_doctor_modal(AppState &state) {
    // File-static cache so the modal can re-run on Refresh without
    // re-running the (cheap, ~5ms) checks every frame.
    static std::optional<DoctorReport> cached;
    if (state.show_doctor_modal) {
        ImGui::OpenPopup("\xEE\xA2\xB0  Diagnostics##doctor_modal");
        state.show_doctor_modal = false;
        cached = run_checks(); // fresh on every open
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(820.0f, 720.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA2\xB0  Diagnostics##doctor_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!cached.has_value()) {
        cached = run_checks();
    }
    auto const &rep = *cached;

    // Header — overall status summary.
    if (rep.any_fail) {
        chip("Status: failing check(s)", chip_fg_danger(), chip_bg_danger());
    } else if (rep.any_warn) {
        chip("Status: advisory", chip_fg_caution(), chip_bg_caution());
    } else {
        chip("Status: healthy", chip_fg_ok(), chip_bg_ok());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh##doctor_refresh")) {
        cached = run_checks();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Re-run every check. Cheap (<10 ms).");
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    if (ImGui::BeginTable("##doctor_rows", 3,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 20.0f);
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch);
        for (auto const &r : rep.rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(dot_color(r.status), "%s", dot_glyph(r.status));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.label.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", r.detail.c_str());
            if (!r.hint.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("\xE2\x86\xB3 %s", r.hint.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_subtle("Equivalent to `subuwutuner-cli doctor`. Run that from a shell "
                "with --json for a CI-friendly snapshot.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    bool const close_clicked = ImGui::Button("Close", ImVec2(120.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dismiss.  (Esc)");
    }
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
