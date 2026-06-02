// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Flash modal — policy gate over the FlashPlan computed from the
// project's source vs working ROM delta. PendingFlash + build_pending_flash
// stay file-local: no cross-file callers. ECU send is CLI-only in this
// build (no in-app transport binding yet); the modal completes the
// "Verify policy" leg of the workflow.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/flash.hpp"
#include "st/policy.hpp"

#include <imgui.h>

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace st::ui {
namespace {

// Build the FlashPlan + PolicyDecision for the currently-loaded project.
// Returns nullopt if there's no project, no delta, or a size mismatch.
struct PendingFlash {
    st::flash::FlashPlan plan;
    st::flash::PolicyDecision decision;
    std::size_t total_bytes;
};

std::optional<PendingFlash> build_pending_flash(AppState const &state) {
    if (!state.project.has_value())
        return std::nullopt;
    auto const &proj = *state.project;
    if (proj.source_rom().size() != proj.working_rom().size())
        return std::nullopt;

    constexpr std::uint32_t kSectorSize = 0x1000;
    constexpr std::uint32_t kBaseAddress = 0;
    auto const sectors = st::flash::Flasher::compute_delta(
        proj.source_rom().data(), proj.working_rom().data(), kSectorSize, kBaseAddress);
    if (sectors.empty())
        return std::nullopt;

    PendingFlash pf;
    pf.total_bytes = 0;
    pf.plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite sw;
        sw.sector = s;
        std::size_t const off = static_cast<std::size_t>(s.address - kBaseAddress);
        sw.data.assign(proj.working_rom().data().begin() + static_cast<std::ptrdiff_t>(off),
                       proj.working_rom().data().begin() +
                           static_cast<std::ptrdiff_t>(off + s.length));
        pf.total_bytes += s.length;
        pf.plan.writes.push_back(std::move(sw));
    }
    pf.decision = st::flash::evaluate_plan_policy(pf.plan, proj.definition(),
                                                  proj.source_rom().data(), proj.policy_profile());
    return pf;
}

} // namespace

void render_flash_modal(AppState &state) {
    if (state.show_flash_modal) {
        ImGui::OpenPopup("\xEE\xA5\x85  Flash...##flash_modal");
        state.show_flash_modal = false;
        // Reset the typed-phrase gate every time the modal opens —
        // an old confirmation must not carry across sessions.
        state.flash_typed_phrase[0] = '\0';
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Width floor prevents the AlwaysAutoResize+TextWrapped shrink
    // loop (same bug fixed in render_read_rom_modal). Flash modal
    // has TextWrapped at the REFUSED branches.
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 240.0f),
                                        ImVec2(560.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("\xEE\xA5\x85  Flash...##flash_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto const pending = build_pending_flash(state);

    if (!state.project.has_value()) {
        ImGui::TextUnformatted("No project loaded.");
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    if (!pending.has_value()) {
        ImGui::TextUnformatted("Working ROM matches source — nothing to flash.");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    auto const &d = pending->decision;
    auto const profile = state.project->policy_profile();
    auto const pname = std::string{st::policy::profile_name(profile)};
    using A = st::policy::Action;

    // Header: plan stats.
    ImGui::Text("Sectors: %zu   Bytes: %zu   Profile: %s", pending->plan.writes.size(),
                pending->total_bytes, pname.c_str());
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // Engine-safety is a hard refusal across every profile.
    if (!d.engine_safety_tables.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("REFUSED: engine-safety-critical tables in plan");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        for (auto const &id : d.engine_safety_tables) {
            ImGui::BulletText("%s", id.c_str());
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        text_subtle("Engine-safety violations block in every profile (docs/06).");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // Emissions-flagged list (informational; the action below decides what
    // the user has to do about it).
    if (!d.emissions_tables.empty()) {
        ImGui::TextUnformatted("Emissions-relevant tables in plan:");
        ImGui::Indent();
        for (auto const &id : d.emissions_tables) {
            ImGui::BulletText("%s", id.c_str());
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    } else {
        text_subtle("No emissions-flagged tables touched.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    }

    // Action-specific UI: silent / confirm / confirm+reason.
    bool ready_to_send = true;
    switch (d.overall_action) {
    case A::Silent:
    case A::Badge:
        // The profile demands no user interaction — just go.
        text_subtle("Profile '%s' raises no gate for this plan.", pname.c_str());
        break;
    case A::Warn:
        text_subtle("Profile '%s' would flag this on save; no flash-time gate.", pname.c_str());
        break;
    case A::Confirm:
        ImGui::Checkbox("I confirm flashing these emissions edits", &state.flash_confirm_checked);
        ready_to_send = state.flash_confirm_checked;
        break;
    case A::ConfirmWithReason:
        ImGui::Checkbox("I confirm flashing these emissions edits", &state.flash_confirm_checked);
        ImGui::TextUnformatted("Reason (required):");
        ImGui::InputTextMultiline("##flash_reason", state.flash_reason, sizeof state.flash_reason,
                                  ImVec2(-FLT_MIN, 60.0f));
        ready_to_send = state.flash_confirm_checked && state.flash_reason[0] != '\0';
        break;
    case A::Block:
        // Distinct from the engine-safety branch above: profile-level
        // Block, e.g. a hypothetical future strict profile.
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("REFUSED by policy.");
        ImGui::PopStyleColor();
        ready_to_send = false;
        break;
    }

    // Typed-phrase confirmation gate (analyst Issue #14). Layered on
    // top of the policy-driven checkbox/reason gate above — the user
    // must ALSO type "YES FLASH" before the Verify button enables.
    // Stops click-through fatigue from making the existing checkbox
    // a no-op. Suppressed when the profile already refuses (Block).
    if (d.overall_action != A::Block) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        bool const phrase_ok = typed_phrase_gate(state.flash_typed_phrase,
                                                 sizeof state.flash_typed_phrase,
                                                 "YES FLASH", "##flash_phrase");
        ready_to_send = ready_to_send && phrase_ok;
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceL));

    // Buttons. "Send to ECU" is intentionally never enabled in this build:
    // the GUI doesn't have a transport binding yet. The dry-run "Verify"
    // button completes the gate workflow without contacting hardware.
    //
    // Verify gets accent treatment + 160 wide to match the primary-action
    // convention across every other modal. No Enter shortcut on purpose:
    // this modal sits adjacent to the "actually flash bytes" workflow
    // (CLI today, in-app later) so accidental Enter-on-focus would feel
    // unsafe even though Verify itself sends nothing.
    bool verify_clicked = false;
    {
        push_primary_button_colors();
        ImGui::BeginDisabled(!ready_to_send);
        verify_clicked = ImGui::Button("Verify policy", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        pop_primary_button_colors();
    }
    if (verify_clicked) {
        state.status_msg = "Flash plan cleared policy gate (" + pname + ").";
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (ready_to_send) {
            ImGui::SetTooltip("Acknowledge that the plan cleared the policy gate.\n"
                              "No bytes are sent to any ECU.");
        } else {
            ImGui::SetTooltip("Tick the confirm box (and fill the reason) first.");
        }
    }
    // "Send to ECU" stays hidden until a real transport binding lands
    // (Phase 3). Greying out a permanently-disabled CTA with the
    // explanation buried in a hover tooltip reads as "the app is
    // broken." Replacing it with an explicit inline note (below)
    // tells the user the same fact without the dead-button surface.
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("ECU send is CLI-only in this build (no in-app transport "
                        "binding yet). After Verify, drive the flash from:");
    ImGui::Indent();
    ImGui::TextDisabled("subuwutuner-cli project-flash <dir> --trace …");
    ImGui::Unindent();

    ImGui::EndPopup();
}

} // namespace st::ui
