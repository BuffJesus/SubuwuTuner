// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Flash modal — policy gate over the FlashPlan computed from the
// project's source vs working ROM delta. PendingFlash + build_pending_flash
// stay file-local: no cross-file callers. ECU send is CLI-only in this
// build (no in-app transport binding yet); the modal completes the
// "Verify policy" leg of the workflow.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/diff.hpp"
#include "st/flash.hpp"
#include "st/policy.hpp"
#include "st/policy/flash_preflight.hpp"
#include "st/profile.hpp"

#include <imgui.h>

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {
namespace {

// Build the FlashPlan + PolicyDecision for the currently-loaded project.
// Returns nullopt if there's no project, no delta, or a size mismatch.
struct PendingFlash {
    st::flash::FlashPlan plan;
    st::flash::PolicyDecision decision;
    std::size_t total_bytes;
    std::vector<st::diff::TableDelta> changed_tables;
    std::size_t tables_skipped{0};
    std::optional<std::string> matched_cid{};
    std::size_t approved_calibration_regions{0};
    std::size_t candidate_calibration_regions{0};
    st::DiagnosticReport preflight;
};

std::string_view blocker_action(std::string_view category) {
    using namespace st::policy;
    if (category == kCatEcuIdentityKnown || category == kCatEcuIdMatch) {
        return "Connect through the supported transport and read the live ECU CID again.";
    }
    if (category == kCatDefinitionMatch) {
        return "Select and lint the definition matching that exact observed CID.";
    }
    if (category == kCatSourceImage) {
        return "Preserve the original read, hash it, and confirm it matches this project source.";
    }
    if (category == kCatRecoveryImage || category == kCatBackupPresent) {
        return "Attach and attest a readable exact-CID recovery image before writing.";
    }
    if (category == kCatJournalSafety) {
        return "Resolve or explicitly resume the interrupted flash journal.";
    }
    if (category == kCatWriteRegions || category == kCatWriteExtent) {
        return "Restrict the plan to approved exact-CID calibration regions.";
    }
    if (category == kCatBatteryVoltage) {
        return "Stabilize and re-measure bench/vehicle voltage within the required range.";
    }
    if (category == kCatIgnitionOn) {
        return "Confirm ignition state through the live preflight probe.";
    }
    if (category == kCatChecksumKnown) {
        return "Configure and validate the checksum strategy for this exact ROM family.";
    }
    if (category == kCatEcuCommunication) {
        return "Stop OBD retries and follow the documented external recovery path.";
    }
    return "Resolve this proof requirement and rerun preflight.";
}

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

    // Add a semantic preview alongside the transport-level sector count.
    // The flash plan knows which bytes will be written, but the user needs
    // to know which named calibrations those bytes belong to before they
    // approve a destructive operation. Aggregate-only diffing keeps this
    // preview light; the full cell list remains in Compare.
    st::diff::Options diff_options;
    diff_options.include_cell_list = false;
    diff_options.include_identical = false;
    if (auto diff = st::diff::compare(proj.source_rom(), proj.working_rom(),
                                      proj.definition(), diff_options);
        diff.has_value()) {
        pf.tables_skipped = diff->skipped.size();
        pf.changed_tables = std::move(diff->tables);
    }

    // The GUI has no live ECU binding yet, so this deliberately constructs a
    // conservative offline preflight report. Local project facts populate
    // only what is actually known; identity, backup, recovery image,
    // journal, and approved-region evidence remain unverified blockers.
    st::policy::PreflightContext preflight_ctx;
    preflight_ctx.profile = proj.policy_profile();
    preflight_ctx.checksum_strategy_known = !proj.definition().pack().checksum_type.empty();
    preflight_ctx.source_rom_size = proj.source_rom().size();
    preflight_ctx.bytes_to_write = pf.total_bytes;

    // A source-ROM match is useful local evidence, but it is not proof that
    // the connected ECU is the same unit. Use it to select only approved
    // exact-CID calibration ranges; the canonical identity validator still
    // blocks until a live observed CID is supplied by a transport binding.
    if (auto const match = proj.definition().match_info(proj.source_rom());
        match.has_value()) {
        pf.matched_cid = match->cid;
        preflight_ctx.expected_ecu_id = match->cid;
        preflight_ctx.definition_match_verified = true;
        for (auto const &region : proj.definition().calibration_regions()) {
            if (region.cid != match->cid)
                continue;
            if (region.status == "approved") {
                ++pf.approved_calibration_regions;
            } else {
                ++pf.candidate_calibration_regions;
            }
        }
        for (auto const *region : proj.definition().approved_calibration_regions(match->cid)) {
            preflight_ctx.approved_write_regions.push_back(
                {static_cast<std::uint64_t>(region->address),
                 static_cast<std::uint64_t>(region->length)});
        }
    }
    preflight_ctx.planned_write_ranges.reserve(pf.plan.writes.size());
    for (auto const &write : pf.plan.writes) {
        preflight_ctx.planned_write_ranges.push_back(
            {static_cast<std::uint64_t>(write.sector.address),
             static_cast<std::uint64_t>(write.sector.length)});
    }
    pf.preflight = st::policy::default_pipeline().run(preflight_ctx);
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
        state.focus_pending_flash = true;
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
    state.help_context = AppState::HelpContext::FlashModal;

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

    // Header: plan stats — human-readable summary instead of a
    // raw-byte breakdown. The tuner cares about "this will write to
    // the ECU" first, with the byte / sector count as backup.
    ImGui::Text("This flash will write %zu sector%s (%zu bytes) to the ECU.",
                pending->plan.writes.size(),
                pending->plan.writes.size() == 1 ? "" : "s",
                pending->total_bytes);
    text_subtle("Policy profile: %s", pname.c_str());
    glossary_tooltip_for(state, "Flash");

    bool const local_plan_ready = pending->matched_cid.has_value() &&
                                  pending->approved_calibration_regions != 0 &&
                                  !state.project->definition().pack().checksum_type.empty() &&
                                  d.engine_safety_tables.empty();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    ImGui::TextUnformatted("Truth state");
    ImGui::TextDisabled("Local plan");
    ImGui::SameLine(130.0f);
    chip(local_plan_ready ? "Coherent for offline review" : "Needs local work",
         local_plan_ready ? chip_fg_ok() : chip_fg_caution(),
         local_plan_ready ? chip_bg_ok() : chip_bg_caution());
    ImGui::TextDisabled("Live ECU");
    ImGui::SameLine(130.0f);
    chip("Unverified / write blocked", chip_fg_danger(), chip_bg_danger());

    if (ImGui::CollapsingHeader("Plain-language review", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(chip_fg_accent(), "What will change");
        ImGui::TextWrapped("%zu named calibration table%s and %zu flash sector%s are in the "
                           "current source-to-working-ROM delta.",
                           pending->changed_tables.size(),
                           pending->changed_tables.size() == 1 ? "" : "s",
                           pending->plan.writes.size(),
                           pending->plan.writes.size() == 1 ? "" : "s");
        ImGui::TextColored(chip_fg_accent(), "Why it changed");
        ImGui::TextWrapped("The project records %zu applied edit operation%s. Review their "
                           "descriptions in History; this screen does not invent intent that "
                           "was not recorded.",
                           state.project->active_history().cursor(),
                           state.project->active_history().cursor() == 1 ? "" : "s");
        ImGui::TextColored(chip_fg_accent(), "What evidence supports it");
        ImGui::TextWrapped(
            "%s. The definition contributes %zu approved exact-CID calibration region%s. "
            "A local match is not a live ECU identity check.",
            pending->matched_cid.has_value()
                ? ("Source ROM matches CID " + *pending->matched_cid).c_str()
                : "The source ROM has no exact CID match",
            pending->approved_calibration_regions,
            pending->approved_calibration_regions == 1 ? "" : "s");
        ImGui::TextColored(chip_fg_accent(), "What is risky");
        ImGui::TextWrapped("%zu engine-safety and %zu emissions-relevant table flag%s are in "
                           "the policy decision. Unknown live identity, backup, recovery, and "
                           "electrical state remain separate blockers.",
                           d.engine_safety_tables.size(), d.emissions_tables.size(),
                           d.engine_safety_tables.size() + d.emissions_tables.size() == 1 ? ""
                                                                                         : "s");
        ImGui::TextColored(chip_fg_accent(), "How to undo it");
        ImGui::TextWrapped("Use History to undo recorded edits, reset individual table cells "
                           "to Source, or restore a verified project checkpoint. None of these "
                           "actions substitutes for an ECU recovery image.");
        ImGui::TextColored(chip_fg_accent(), "What must be verified after power-cycle");
        ImGui::TextWrapped("Re-identify the ECU, confirm communication, read back every written "
                           "sector, compare it with this plan, verify checksums, and record the "
                           "result before declaring the flash successful.");
    }

    // Semantic delta preview. This is intentionally above the policy
    // details: users should first see the human meaning of the write, then
    // the jurisdiction and safety decision that governs it. Rows are
    // actionable links rather than decorative text — clicking one closes
    // the modal and opens that table in the Tune workspace for inspection.
    if (!pending->changed_tables.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        char header[96];
        std::snprintf(header, sizeof header, "Changed tables (%zu)",
                      pending->changed_tables.size());
        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            for (auto const &table : pending->changed_tables) {
                ImGui::PushID(table.table_id.c_str());
                char label[256];
                char const *const safety = table.engine_safety_critical ? "  ·  engine safety" :
                    (table.emissions_relevant ? "  ·  emissions" : "");
                std::snprintf(label, sizeof label, "%s  ·  %zu cell%s  ·  max |Δ| %.4g%s",
                              table.table_name.empty() ? table.table_id.c_str()
                                                        : table.table_name.c_str(),
                              table.cells_changed,
                              table.cells_changed == 1 ? "" : "s",
                              table.max_abs_delta, safety);
                if (ImGui::Selectable(label, false)) {
                    ImGui::CloseCurrentPopup();
                    jump_to_table(state, table.table_id);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nClick to inspect this table before flashing.",
                                     table.table_id.c_str());
                }
                ImGui::PopID();
            }
            if (pending->tables_skipped > 0) {
                ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
                text_subtle("%zu table%s could not be summarized by the definition pack; "
                            "the byte-level plan still remains subject to policy.",
                            pending->tables_skipped,
                            pending->tables_skipped == 1 ? "" : "s");
            }
            ImGui::Unindent();
        }
    }

    // Expandable summary of the flagged tables in the plan. Without
    // this the user knows the byte count but not WHICH calibration
    // tables are flagged for safety or emissions. The policy
    // decision already tracks both sets; surfacing them here in a
    // collapsed tree gives the user a sanity check before they
    // commit. (The full "every changed table" list would require
    // re-walking the plan against the definition — out of scope for
    // this UX polish; flagged tables are the ones that matter for
    // the decision.)
    {
        std::size_t const n_flagged =
            d.engine_safety_tables.size() + d.emissions_tables.size();
        if (n_flagged > 0) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
            char header[64];
            std::snprintf(header, sizeof header,
                          "Flagged tables in this plan (%zu)", n_flagged);
            if (ImGui::CollapsingHeader(header)) {
                ImGui::Indent();
                if (!d.engine_safety_tables.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_warn());
                    ImGui::TextUnformatted("Engine-safety:");
                    ImGui::PopStyleColor();
                    for (auto const &id : d.engine_safety_tables) {
                        ImGui::BulletText("%s", id.c_str());
                    }
                }
                if (!d.emissions_tables.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
                    ImGui::TextUnformatted("Emissions:");
                    ImGui::PopStyleColor();
                    for (auto const &id : d.emissions_tables) {
                        ImGui::BulletText("%s", id.c_str());
                    }
                }
                ImGui::Unindent();
            }
        }
    }

    // Hardware-independent truth boundary. This is intentionally shown in
    // the flash review itself, not only in Project Readiness: a user opening
    // the destructive-action surface must see why local policy approval is
    // not the same thing as ECU eligibility.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    ImGui::TextUnformatted("Hardware preflight");
    if (pending->matched_cid.has_value()) {
        text_subtle("Source ROM CID: %s  |  approved calibration regions: %zu  |  candidates not used: %zu",
                    pending->matched_cid->c_str(), pending->approved_calibration_regions,
                    pending->candidate_calibration_regions);
    } else {
        ImGui::TextColored(chip_fg_danger(),
                           "Source ROM did not match an exact definition CID; no calibration allow-list was selected.");
    }
    auto const blockers = pending->preflight.blockers();
    auto const warnings = pending->preflight.warnings();
    if (blockers.empty()) {
        ImGui::TextColored(chip_fg_ok(), "No preflight blockers in the available evidence.");
    } else {
        ImGui::TextColored(chip_fg_danger(), "%zu preflight blocker%s — hardware write refused.",
                           blockers.size(), blockers.size() == 1 ? "" : "s");
    }
    if (!warnings.empty()) {
        text_subtle("%zu preflight warning%s also need review.", warnings.size(),
                    warnings.size() == 1 ? "" : "s");
    }
    if (ImGui::CollapsingHeader("Show preflight details", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        for (auto const &diagnostic : pending->preflight.items()) {
            ImVec4 const color = diagnostic.is_blocker() ? chip_fg_danger()
                : (diagnostic.is_warning() ? chip_fg_caution() : chip_fg_info());
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::BulletText("%s: %s", diagnostic.category().data(),
                             diagnostic.message().data());
            ImGui::PopStyleColor();
            if (diagnostic.is_blocker()) {
                ImGui::Indent();
                text_subtle("Action: %s", blocker_action(diagnostic.category()).data());
                ImGui::Unindent();
            }
        }
        ImGui::Unindent();
    }

    // Issue #10 sweep: when the user is viewing a non-working ROM
    // (additional or source), the Flash modal still operates on the
    // working slot — that's the only ROM the project allows edits to
    // and therefore the only one with a sensible flash plan. Surface
    // this so the user is not confused into thinking the modal will
    // write the additional ROM they were just inspecting.
    if (!state.viewing_working_rom()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
        ImGui::TextWrapped(
            "Note: flashing the WORKING ROM. You are currently viewing '%s' "
            "(read-only); switch via View → Active ROM to confirm what will "
            "land on the ECU.",
            state.active_rom_id.empty() ? "working" : state.active_rom_id.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    }

    // Active vehicle profile context (analyst Issue #7). Looked up
    // fresh per render — cheap (TOML load of one file) and avoids a
    // stale-cache footgun where the user edits the profile via CLI
    // mid-session and the modal still shows old data. Three states:
    //   - none set       → subtle hint pointing at Settings
    //   - set + missing  → amber "(missing on disk)" so the user knows
    //                      the linked profile got deleted/moved
    //   - set + loaded   → "Vehicle: <name>  ·  <year make model>" plus
    //                      a transport-hint subline when present
    if (state.settings.active_vehicle_profile_id.empty()) {
        text_subtle("No active vehicle profile. Tools → Settings to pick one.");
    } else {
        auto const profile_path = st::profile::default_profile_dir() /
                                  (state.settings.active_vehicle_profile_id + ".stprofile");
        auto const loaded = st::profile::load(profile_path);
        if (!loaded.has_value()) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_warn());
            ImGui::Text("Vehicle profile '%s' missing on disk.",
                        state.settings.active_vehicle_profile_id.c_str());
            ImGui::PopStyleColor();
        } else {
            auto const &vp = *loaded;
            char const *display = vp.display_name.empty() ? vp.id.c_str()
                                                          : vp.display_name.c_str();
            ImGui::Text("Vehicle: %s  ·  %s %s %s%s%s", display,
                        vp.year.c_str(), vp.make.c_str(), vp.model.c_str(),
                        vp.transmission.empty() ? "" : "  ·  ",
                        vp.transmission.c_str());
            if (!vp.transport_hint.empty()) {
                text_subtle("Transport hint: %s", vp.transport_hint.c_str());
            }
        }
    }
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // Engine-safety is a hard refusal across every profile. Phrased
    // calmly: "this can't be flashed, here's why, and here's what to
    // do" rather than a red REFUSED slab. The amber/caution band
    // reads as "needs your attention" — danger-red is reserved for
    // the actual destructive-action confirmation gate.
    if (!d.engine_safety_tables.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_warn());
        ImGui::TextUnformatted("\xE2\x9A\xA0  Can't flash: engine-safety tables modified");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::TextWrapped(
            "These tables control engine safety and are blocked from "
            "flashing in every jurisdiction profile. Revert your edits "
            "in the table editor (Right-click \xE2\x86\x92 Reset to "
            "Source) or open them under View \xE2\x86\x92 Active ROM to "
            "review:");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::Indent();
        for (auto const &id : d.engine_safety_tables) {
            ImGui::BulletText("%s", id.c_str());
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        text_subtle("Why: these tables can damage the engine if mis-set. "
                    "The block is hardcoded \xE2\x80\x94 see docs/06.");
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
    bool ready_to_send = pending->preflight.ok();
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
        if (state.focus_pending_flash) {
            // Land focus on the safety checkbox — Space toggles, Tab
            // moves to Verify. Per the no-Enter-on-Verify policy
            // upthread, deliberately NOT focusing the primary button.
            ImGui::SetKeyboardFocusHere();
            state.focus_pending_flash = false;
        }
        ImGui::Checkbox("I confirm flashing these emissions edits", &state.flash_confirm_checked);
        // Compose with the preflight seed above -- the checkbox is an
        // ADDITIONAL gate, never a substitute for the hardware
        // blockers. Assigning here would discard them.
        ready_to_send = ready_to_send && state.flash_confirm_checked;
        break;
    case A::ConfirmWithReason:
        if (state.focus_pending_flash) {
            ImGui::SetKeyboardFocusHere();
            state.focus_pending_flash = false;
        }
        ImGui::Checkbox("I confirm flashing these emissions edits", &state.flash_confirm_checked);
        ImGui::TextUnformatted("Reason (required):");
        ImGui::InputTextMultiline("##flash_reason", state.flash_reason, sizeof state.flash_reason,
                                  ImVec2(-FLT_MIN, 60.0f));
        ready_to_send =
            ready_to_send && state.flash_confirm_checked && state.flash_reason[0] != '\0';
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
    // Clear any leftover focus-pending flag — the Silent / Badge /
    // Warn / Block branches above have no widget to focus, but we
    // don't want the flag carrying over to the next modal open under
    // a different profile.
    state.focus_pending_flash = false;

    // Typed-phrase confirmation gate (analyst Issue #14). Layered on
    // top of the policy-driven checkbox/reason gate above — the user
    // must ALSO type "YES FLASH" before the Verify button enables.
    // Stops click-through fatigue from making the existing checkbox
    // a no-op. Suppressed when the profile already refuses (Block).
    if (d.overall_action != A::Block) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        // Explain WHY the typed-phrase gate exists. Without context
        // it reads as gatekeeping ritual; with context, the user
        // understands it's a guard against accidental ECU writes.
        text_subtle("Final guard against an accidental flash. "
                    "Mistypes are harmless \xE2\x80\x94 just retype.");
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
            ImGui::SetTooltip("Resolve the hardware preflight blockers first.\n"
                              "No bytes are sent by this GUI.");
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
                        "binding yet). After Verify:");
    ImGui::Indent();
    ImGui::TextDisabled("Preview again, hardware-free:");
    ImGui::TextDisabled("  subuwutuner-cli project-flash <dir> [--trace <FILE.uds>]");
    ImGui::TextDisabled("Write to a live ECU (needs a plan, not a project dir):");
    ImGui::TextDisabled("  subuwutuner-cli flash-delta <source.bin> <target.bin> -o p.toml");
    ImGui::TextDisabled("  subuwutuner-cli flash-apply --plan p.toml --transport obdx \\");
    ImGui::TextDisabled("      --device COM5 --confirm --reason \"…\"");
    ImGui::Unindent();

    ImGui::EndPopup();
}

} // namespace st::ui
