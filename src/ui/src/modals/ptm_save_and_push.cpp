// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// T31 — "Save & Push to AP" modal. One button that runs the full Edit
// → Export → Push flow end-to-end:
//
//   1/4 Diffing edits   — patches are already coalesced in
//                          ptm_patches.toml at `ptm import` time; this
//                          step just validates the project shape.
//   2/4 Building .ptm   — shared build_ptm_bytes pipeline (XML + bzip2
//                          + AES + XTEA). Same bytes ptm_export emits.
//   3/4 Uploading       — cmd 0x22 PutFile setup + cmd 0x23 data + cmd
//                          0x05 NAND barrier, via the open ETS browser
//                          channel.
//   4/4 Verifying       — decrypt the produced bytes in memory; surface
//                          any inconsistency before claiming success.
//
// Async-on-IO design: the build phase runs synchronously on the click
// frame (fast — ~50 ms for a 4 KB patch set); the upload + verify run
// on a worker thread so the GUI keeps repainting during the ~3 s USB
// round-trip. A polled step atomic drives the progress bar.
//
// Gated at build time on ST_HAVE_AP_WORKFLOW. When the flag is off the
// modal still renders, surfaces a clear "workflow surface disabled"
// notice, and offers a single "Save .ptm to disk only" fallback that
// routes through the existing Export modal — keeping the offline path
// reachable for users who want to author tunes without ever opening a
// USB device.

#include "modals/modals.hpp"
#include "modals/ptm_pipeline.hpp"
#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/devices/ets/ptm_cipher.hpp"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace st::ui {

namespace {

enum class SapStep : int {
    Idle = 0,
    Building = 1,
    Uploading = 2,
    Verifying = 3,
    Done = 4,
    Failed = 5,
};

// File-static work state. Held outside AppState so the heavy bits
// (worker future, the built .ptm bytes) don't bloat the AppState
// struct and the std::atomic + std::future move-semantics don't fight
// AppState's value-type assumptions.
struct WorkState {
    std::atomic<int> step{static_cast<int>(SapStep::Idle)};
    std::string tune_name; // copy used by worker thread
    std::string ap_path;   // success: "/maps/<name>" on Done
    std::string error;     // failure: message on Failed
    std::vector<std::uint8_t> built_bytes;
    std::future<void> work;
};

WorkState &work() {
    static WorkState s;
    return s;
}

void reset_work() {
    auto &w = work();
    // Best-effort wait on a stale worker so we don't lose its result
    // mid-respawn. work() is the only thread that sets step/error/etc.
    if (w.work.valid()) {
        // Polling here would block the click frame. Instead: only
        // restart if the previous run reached Done/Failed (terminal).
        // If still in flight, the caller should have disabled the
        // button — but as defense-in-depth we no-op.
        int const cur = w.step.load(std::memory_order_acquire);
        if (cur != static_cast<int>(SapStep::Done) &&
            cur != static_cast<int>(SapStep::Failed) &&
            cur != static_cast<int>(SapStep::Idle)) {
            return;
        }
    }
    w.step.store(static_cast<int>(SapStep::Idle),
                  std::memory_order_release);
    w.tune_name.clear();
    w.ap_path.clear();
    w.error.clear();
    w.built_bytes.clear();
}

// Default tune-name suggestion when the user opens the modal. Pattern:
// `<base-rom-id>_edited_<YYYYmmdd-HHMMSS>.ptm`. Falls back to a
// timestamp-only name when no project / metadata is available.
std::string suggest_tune_name(AppState const &state) {
    char ts[32];
    std::time_t const now = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string stem = "edited";
    if (state.ptm_imported_vehicle.has_value() &&
        !state.ptm_imported_vehicle->empty()) {
        stem = *state.ptm_imported_vehicle;
    } else if (state.project.has_value()) {
        stem = state.project->dir().filename().string();
    }
    return stem + "_edited_" + ts + ".ptm";
}

bool parse_seed_local(std::string_view s, std::uint32_t &out) {
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
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        v = v * static_cast<std::uint64_t>(base) +
            static_cast<std::uint64_t>(d);
        if (v > 0xFFFFFFFFULL)
            return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

void start_save_and_push(AppState &state) {
    reset_work();
    auto &w = work();

    if (state.ptm_sap_tune_name[0] == '\0') {
        w.error = "Pick a tune name first.";
        w.step.store(static_cast<int>(SapStep::Failed),
                      std::memory_order_release);
        return;
    }
    std::uint32_t seed = 0;
    if (!parse_seed_local(state.ptm_sap_seed_hex, seed)) {
        w.error = "Bad seed value (expected hex or decimal u32).";
        w.step.store(static_cast<int>(SapStep::Failed),
                      std::memory_order_release);
        return;
    }
    // Step 1+2: build the bytes on the click frame. Fast, and lets
    // build-time failures (missing project, malformed patches) fail
    // fast before we spin up the worker thread + USB plumbing.
    w.step.store(static_cast<int>(SapStep::Building),
                  std::memory_order_release);
    std::string err;
    auto bytes = build_ptm_bytes(state, seed, err);
    if (!bytes.has_value()) {
        w.error = std::move(err);
        w.step.store(static_cast<int>(SapStep::Failed),
                      std::memory_order_release);
        return;
    }
    w.built_bytes = std::move(*bytes);
    w.tune_name = state.ptm_sap_tune_name;

#ifndef ST_HAVE_AP_WORKFLOW
    // Workflow surface off — declare success on the build-only path.
    // ptm_export modal is the right answer for users who want to save
    // to disk; for parity we surface a Done state here so the modal's
    // success buttons trigger the user toward that flow.
    w.error =
        "AP workflow surface is off in this build (reconfigure with "
        "-DST_ENABLE_COBB_AP_WORKFLOW=ON to enable upload). The .ptm "
        "was NOT pushed.";
    w.step.store(static_cast<int>(SapStep::Failed),
                  std::memory_order_release);
    return;
#else
    if (!ets_panel_has_channel()) {
        w.error =
            "No AccessPort channel open. Connect via the AccessPort "
            "Browser panel first, then retry Save & Push.";
        w.step.store(static_cast<int>(SapStep::Failed),
                      std::memory_order_release);
        return;
    }
    // Step 3+4: async on a worker thread. The push blocks on the
    // panel's channel (~2-5 s for typical /maps/ payloads), then the
    // verify path runs decrypt_ptm on the in-memory bytes.
    w.step.store(static_cast<int>(SapStep::Uploading),
                  std::memory_order_release);
    w.work = std::async(std::launch::async, [&w]() {
        // Hand the bytes to the panel's push helper. The panel runs
        // its own channel worker; this call blocks until the cmd 0x22
        // + cmd 0x23 + cmd 0x05 round-trip completes.
        std::vector<std::uint8_t> bytes_copy = w.built_bytes;
        auto result = ets_panel_push_to_maps(w.tune_name,
                                              std::move(bytes_copy));
        if (!result.ok) {
            w.error = std::move(result.error);
            w.step.store(static_cast<int>(SapStep::Failed),
                          std::memory_order_release);
            return;
        }
        w.ap_path = std::move(result.ap_path);
        // Step 4: verify by decrypting the bytes we just pushed.
        // Matches the round-trip test the analyst's tooling runs
        // before declaring a custom .ptm shippable.
        w.step.store(static_cast<int>(SapStep::Verifying),
                      std::memory_order_release);
        auto decrypted =
            st::devices::ets::cipher::decrypt_ptm(w.built_bytes);
        if (!decrypted.has_value()) {
            w.error = "Pushed OK but local round-trip decrypt failed: " +
                      decrypted.error().to_string() +
                      " — the .ptm reached the AP but may not be "
                      "loadable. Pull it back via the AP Browser to "
                      "investigate.";
            w.step.store(static_cast<int>(SapStep::Failed),
                          std::memory_order_release);
            return;
        }
        w.step.store(static_cast<int>(SapStep::Done),
                      std::memory_order_release);
    });
#endif
}

void render_step_row(int step_idx, int current_step, char const *label) {
    bool const past = current_step > step_idx;
    bool const here = current_step == step_idx;
    bool const failed = current_step == static_cast<int>(SapStep::Failed);
    char marker = ' ';
    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (failed && here) {
        marker = 'x';
        col = chip_fg_danger();
    } else if (past) {
        marker = '+';
        col = chip_fg_ok();
    } else if (here) {
        marker = '>';
        col = chip_fg_caution();
    }
    ImGui::TextColored(col, "  %c  %d/4  %s", marker, step_idx, label);
}

} // namespace

void render_ptm_save_and_push_modal(AppState &state) {
    if (state.show_ptm_save_and_push_modal) {
        // Pre-fill the tune-name field on first open so the user has a
        // sensible default. Wipes the prior session's input on each
        // open — the modal is a per-edit-cycle wizard, not a sticky
        // form.
        if (state.ptm_sap_tune_name[0] == '\0') {
            auto suggested = suggest_tune_name(state);
            std::strncpy(state.ptm_sap_tune_name, suggested.c_str(),
                          sizeof(state.ptm_sap_tune_name) - 1);
            state.ptm_sap_tune_name[sizeof(state.ptm_sap_tune_name) - 1] =
                '\0';
        }
        reset_work();
        ImGui::OpenPopup("\xEE\x9D\x8E  Save & Push to AP##ptm_sap_modal");
        state.show_ptm_save_and_push_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 360.0f),
                                         ImVec2(900.0f, 640.0f));
    if (!ImGui::BeginPopupModal(
            "\xEE\x9D\x8E  Save & Push to AP##ptm_sap_modal", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped(
        "Build the loaded project's edits into a fresh .ptm and push it "
        "directly to your connected AccessPort's /maps/ folder. The AP "
        "doesn't auto-flash — the new map appears alongside your "
        "existing tunes and waits for you to select it.");
    ImGui::Separator();
    ImGui::Spacing();

    if (!state.project.has_value()) {
        ImGui::TextColored(chip_fg_muted(), "Open a project first.");
        ImGui::Spacing();
        if (ImGui::Button("Close")) {
            reset_work();
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

    auto &w = work();
    int const step = w.step.load(std::memory_order_acquire);
    bool const in_flight = step != static_cast<int>(SapStep::Idle) &&
                            step != static_cast<int>(SapStep::Done) &&
                            step != static_cast<int>(SapStep::Failed);

    ImGui::Text("Tune name (.ptm):");
    ImGui::SetNextItemWidth(-1.0f);
    if (in_flight) {
        ImGui::BeginDisabled();
    }
    ImGui::InputText("##sap_tune_name", state.ptm_sap_tune_name,
                      sizeof(state.ptm_sap_tune_name));
    ImGui::Spacing();
    ImGui::Text("XTEA seed (hex or decimal u32):");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##sap_seed", state.ptm_sap_seed_hex,
                      sizeof(state.ptm_sap_seed_hex));
    if (in_flight) {
        ImGui::EndDisabled();
    }

#ifdef ST_HAVE_AP_WORKFLOW
    if (!ets_panel_has_channel()) {
        ImGui::Spacing();
        ImGui::TextColored(
            chip_fg_caution(),
            "AccessPort not connected — open the AccessPort Browser "
            "panel and connect first.");
    }
#else
    ImGui::Spacing();
    ImGui::TextColored(
        chip_fg_caution(),
        "AP workflow surface is OFF in this build. Save & Push will "
        "build the .ptm but cannot upload — reconfigure with "
        "-DST_ENABLE_COBB_AP_WORKFLOW=ON to enable upload.");
#endif

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Progress");
    render_step_row(1, step, "Diffing edits");
    render_step_row(2, step, "Building .ptm");
    render_step_row(3, step, "Uploading to AP");
    render_step_row(4, step, "Verifying");

    if (!w.error.empty() && step == static_cast<int>(SapStep::Failed)) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "Failed: %s", w.error.c_str());
    }
    if (step == static_cast<int>(SapStep::Done)) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_ok(),
                            "Pushed %zu bytes to AP:%s",
                            w.built_bytes.size(), w.ap_path.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool const can_start = !in_flight;
#ifdef ST_HAVE_AP_WORKFLOW
    bool const have_channel = ets_panel_has_channel();
#else
    bool const have_channel = false;
#endif
    if (!can_start || !have_channel) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save & Push")) {
        start_save_and_push(state);
    }
    if (!can_start || !have_channel) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    // "Set as next flash" is gated on T25 tunerflash auth surface RE.
    // Pre-stage so the user sees it's coming + understands why it's
    // not live yet; per the handoff's success-state callout.
    ImGui::BeginDisabled();
    if (ImGui::Button("Set as next flash")) {
        // intentionally unreachable until T25 lands
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Tell the AP to flash this .ptm next time the user kicks\n"
            "off a flash. Gated on T25 (tunerflash auth surface RE) —\n"
            "until we know whether the AP accepts custom-authored\n"
            ".ptm content, this button stays inert.");
    }
    ImGui::SameLine();
    if (step == static_cast<int>(SapStep::Done) &&
        !state.show_ap3_browser_panel) {
        if (ImGui::Button("Show in AP browser")) {
            state.show_ap3_browser_panel = true;
            reset_work();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Close")) {
        reset_work();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
