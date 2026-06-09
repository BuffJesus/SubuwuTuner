// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// TGV + EGR delete workflow modal.
//
// Encodes the pack-declared `tgv_egr_delete` workflow per analyst's
// Task A-E delivery (2026-06-09 — findings/tgv-egr-delete/
// TASK_E_WORKFLOW_SPEC.md). Three-step guided flow:
//   1. Jurisdiction confirm — explicit off-road/track-only attestation
//   2. Hardware confirm — physical TGV + EGR removal acknowledged
//   3. Review — show what will change, Apply commits the edits
//
// Apply path produces 6 edits (all tagged "tgv_egr_delete" so the
// status-bar badge's Revert All peels them as a batch):
//   - Zero EGR Airflow table (16×12 uint16_be, canon 0x34DEA)
//   - Zero EGR Absolute Pressure Main (16×16 uint8, canon 0x34F9A)
//   - Zero TGV Closed Activation (16×22 uint16_be, canon 0x3A922)
//   - Zero Ignition Timing EGR Adder A (8×11 uint8, canon 0x38DD0)
//   - Zero Ignition Timing EGR Adder C (8×11 uint8, canon 0x38F30)
//   - Disable P0400 DTC (clear enable bit in primary_dtc_enable bitmap)
//
// Adders A and C were added per analyst's Task F (gating ambiguous;
// conservative zero). Adders B and D were byte-verified stock-neutral
// and stay untouched.
//
// Checksum_reseat is NOT a workflow step — `Flasher::execute` handles
// post-edit checksum recompute automatically via apply_checksum_repair
// (per pack's `checksum_type`), so users get the right slot-7 fix as
// part of the normal flash flow.
//
// Bench-test recommendation per analyst's Task B: if actuator-circuit
// DTCs fire after install (e.g. P0488, P2122), open Task B v2 to
// trace the PWM-driver chain and add NOP patches. Empirical-first
// approach saves analyst time when L1 + DTC bit-clear is sufficient.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "panels/panels.hpp" // enqueue_toast
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/edit.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace st::ui {

namespace {

constexpr char const *kWorkflowId = "tgv_egr_delete";
constexpr int kStepCount = 3;
constexpr char const *kStepNames[kStepCount] = {
    "Jurisdiction",
    "Hardware",
    "Review",
};

// Tables the workflow zeros. Order matters — apply iterates this
// verbatim; the status bar's Revert All undoes in reverse. All three
// edits land contiguously so undo_while_tag peels them as a batch.
// Categories per the analyst's WORKFLOW_SPEC step 1-3:
//   1. zero EGR Airflow
//   2. zero EGR Absolute Pressure Main
//   3. zero TGV Closed Activation
struct WorkflowTable {
    char const *table_id;
    char const *display_name;
    char const *label;     // edit-description string
    char const *rationale; // shown in review screen
};

constexpr std::array<WorkflowTable, 5> kWorkflowTables = {{
    {"airflow_manifold_flow_model_egr_flow_model_egr_airflow",
     "EGR Airflow",
     "TGV+EGR delete: zero EGR Airflow",
     "Sets the commanded EGR mass-airflow target to 0 across every operating "
     "cell. After zero, FUN_001095D8 / FUN_00109DA4 unconditionally write 0 "
     "to the EGR flow target (analyst-traced)."},
    {"airflow_manifold_flow_model_egr_flow_model_egr_absolute_pressure_main",
     "EGR Absolute Pressure Main",
     "TGV+EGR delete: zero EGR Absolute Pressure Main",
     "Sets the EGR-pressure setpoint to 0 across every operating cell. After "
     "zero, FUN_00109CDC outputs 0 too (the secondary input table at 0x68F28 "
     "is already factory-zero — code-side byte-verified)."},
    {"ignition_compensation_coolant_tgv_closed_activation",
     "TGV Closed Activation",
     "TGV+EGR delete: zero TGV Closed Activation",
     "Sets the TGV-closed blend weight to 0 across every cell. FUN_000F1C14 "
     "blends Open and Closed targets weighted by the activation values; with "
     "Closed = 0, the dispatcher returns the Open target only (TGV "
     "effectively always-open at runtime)."},
    {"ignition_compensation_egr_tgv_closed_ignition_timing_egr_adder_a",
     "Ignition Timing EGR Adder A",
     "TGV+EGR delete: zero EGR Ignition Adder A",
     "Per analyst's Task F — gating ambiguous. The Adder is consumed by "
     "FUN_EGRADDERS unconditionally and applies a weighted blend; the weight's "
     "runtime semantics weren't fully traced, so the conservative path zeros "
     "the Adder. Removes timing-modification risk under the delete."},
    {"ignition_compensation_egr_tgv_closed_ignition_timing_egr_adder_c",
     "Ignition Timing EGR Adder C",
     "TGV+EGR delete: zero EGR Ignition Adder C",
     "Same Task F treatment as Adder A. Adders B + D are stock-neutral "
     "(byte-verified 0x80 = 0° everywhere in stock and WRK3), so they stay "
     "untouched."},
}};

// Code targeted by the DTC bit-clear step. P0400 = "Exhaust Gas
// Recirculation Flow Malfunction". Analyst confirmed this is the only
// EGR-flow DTC in the 360-record code table for LF79103P; TGV-specific
// codes aren't in this bitmap, so this single edit covers the delete.
constexpr char const *kDtcCode = "P0400";

struct ModalState {
    int step{0};
    // Step 1 checkboxes
    bool jurisdiction_off_road{false};
    bool jurisdiction_local_permits{false};
    // Step 2 checkbox
    bool hardware_removed_confirmed{false};
};
static ModalState g_state;

void reset_state() {
    g_state.step = 0;
    g_state.jurisdiction_off_road = false;
    g_state.jurisdiction_local_permits = false;
    g_state.hardware_removed_confirmed = false;
}

bool jurisdiction_ok() {
    // Either checkbox is sufficient — the user only needs ONE valid
    // legal basis to proceed (off-road usage OR explicit jurisdiction
    // permission). Both checked is also fine.
    return g_state.jurisdiction_off_road || g_state.jurisdiction_local_permits;
}

} // namespace

bool pack_supports_tgv_egr_delete(AppState const &state) {
    if (!state.project.has_value()) {
        return false;
    }
    return state.project->definition().supports_workflow("tgv_egr_delete");
}

bool tgv_egr_delete_active(AppState const &state) {
    if (!state.project.has_value()) {
        return false;
    }
    auto const &records = state.project->active_history().records();
    auto const cursor = state.project->active_history().cursor();
    if (cursor == 0) {
        return false;
    }
    // Head of applied range = records[cursor-1]. We treat the badge
    // as "active" iff the head edit's tag is the workflow id — i.e.,
    // the user hasn't appended any subsequent edits.
    auto const &head = records[cursor - 1];
    return head.tag == kWorkflowId;
}

void revert_tgv_egr_delete(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto const reverted =
        state.project->active_history().undo_while_tag(kWorkflowId);
    if (reverted.empty()) {
        if (tgv_egr_delete_active(state)) {
            enqueue_toast(state, ToastKind::Warn,
                          "Revert All blocked: a non-workflow edit sits on top "
                          "of the TGV+EGR-delete batch. Undo manually until "
                          "those are removed, then Revert All will peel the "
                          "workflow off.");
        } else {
            enqueue_toast(state, ToastKind::Warn,
                          "TGV+EGR delete: no edits to revert at history head.");
        }
        return;
    }
    enqueue_toast(state, ToastKind::Success,
                  "Reverted " + std::to_string(reverted.size()) +
                      " TGV+EGR delete edits.");
}

namespace {

void draw_jurisdiction_step(AppState &state) {
    (void)state;
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("This workflow disables EGR and TGV subsystems. EGR "
                       "(Exhaust Gas Recirculation) and TGV (Tumble Generator "
                       "Valves) are emissions-control hardware. In most "
                       "jurisdictions, deleting them on a vehicle operated on "
                       "public roads is illegal.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("By proceeding, you affirm at least ONE of the following:");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

    ImGui::Checkbox("My vehicle will not be operated on public roads "
                    "(off-road, track-only, dyno mule, etc.)##tgv_egr_offroad",
                    &g_state.jurisdiction_off_road);
    ImGui::Indent();
    text_subtle("Track-day cars, dyno mules, off-road-only rigs, and competition "
                "vehicles typically qualify. Daily-drivers do not.");
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

    ImGui::Checkbox("My jurisdiction explicitly permits EGR/TGV deletion##tgv_egr_permits",
                    &g_state.jurisdiction_local_permits);
    ImGui::Indent();
    text_subtle("A small number of jurisdictions (some Canadian provinces, "
                "selected non-OBD regions) have no equivalent of CARB/EPA "
                "tampering rules. SubuwuTuner does not maintain a list — "
                "you're responsible for verifying.");
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    if (!jurisdiction_ok()) {
        ImGui::TextColored(chip_fg_caution(),
                           "\xE2\x9A\xA0  Check at least one box above to "
                           "proceed.");
    }
}

void draw_hardware_step(AppState &state) {
    (void)state;
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("This is a SOFTWARE-side delete only. It tells the ECU "
                       "to command \"no EGR\" and \"TGV always open\" at every "
                       "operating point, and disables the P0400 EGR Flow "
                       "Malfunction code. The actual hardware needs to be "
                       "removed or disconnected separately.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("Recommended hardware steps (do these BEFORE flashing this delete):");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    ImGui::BulletText("Remove or block the EGR valve. Plug the EGR vacuum / "
                      "coolant / exhaust connections per your delete kit.");
    ImGui::BulletText("Remove the TGV motor assembly OR unplug its electrical "
                      "connector. (The motor will read open-circuit; the L1 "
                      "patch zeros the closed-mode command, so the motor "
                      "driver shouldn't try to drive it.)");
    ImGui::BulletText("Inspect the intake manifold passages where TGV blades "
                      "lived for any loose hardware before reinstalling.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    ImGui::Checkbox("Hardware removal is done (or will be done before flashing "
                    "this delete)##tgv_egr_hwconfirm",
                    &g_state.hardware_removed_confirmed);
    ImGui::Indent();
    text_subtle("If you flash this delete with hardware still in place, the "
                "ECU will command no EGR + no TGV but the actuators are still "
                "physically present. Unlikely to cause damage but the engine "
                "won't get the intended fueling/timing improvements until "
                "hardware is also addressed.");
    ImGui::Unindent();
}

void draw_review_step(AppState &state) {
    (void)state;
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextWrapped("Apply will change 6 things in your working ROM:");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    auto change_row = [](char const *name, char const *transition, char const *why) {
        ImGui::BulletText("%s", name);
        ImGui::Indent();
        ImGui::TextUnformatted(transition);
        text_subtle("%s", why);
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    };

    for (auto const &wt : kWorkflowTables) {
        change_row(wt.display_name, "every cell \xE2\x86\x92 0", wt.rationale);
    }
    change_row("P0400 DTC (EGR Flow Malfunction)",
               "enable bit \xE2\x86\x92 0 (suppressed)",
               "Prevents the ECU from logging P0400 when the EGR target = 0 "
               "and actual flow = 0. Routed through the existing "
               "set_dtc_enabled byte-edit so it's revertible via the "
               "status-bar badge.");

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("Post-flash expectations (per analyst's Task D doc):");
    ImGui::Indent();
    ImGui::BulletText("AF Correction drifts +3 to +8%% in cruise cells for "
                      "the first ~10 minutes of driving.");
    ImGui::BulletText("AF Learning absorbs the drift by the 30-min mark.");
    ImGui::BulletText("If actuator-circuit DTCs fire (P0488, P2122, etc.), "
                      "open the bench rig — empirical-first per Task B.");
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("Revertible: use the status-bar TGV+EGR-delete badge \xE2\x86\x92 "
                "Revert All to undo all 4 edits atomically.");
}

void apply_tgv_egr_delete(AppState &state) {
    auto const cursor_before = state.project->active_history().cursor();

    // Step 1-3: zero each L1 cal table via set_cells(0.0) over the
    // whole table. apply_op_table records each as its own tagged
    // Edit so the status-bar badge's Revert All can peel them as a
    // batch. Same plumbing fa24_swap uses.
    for (auto const &wt : kWorkflowTables) {
        apply_op_table(state, wt.label, kWorkflowId, wt.table_id,
                       [](st::Definition::TableData &td, st::edit::Rect r) {
                           return st::edit::set_cells(td, r, 0.0);
                       });
    }

    // Step 4: disable P0400 via the existing set_dtc_enabled
    // infrastructure. The change lands as a ByteEdit::Change inside
    // edit::History tagged with kWorkflowId, so it joins the workflow's
    // revert batch. Routing through set_dtc_enabled (instead of a
    // raw byte write) keeps the bit-arithmetic in one place and
    // surfaces the same audit-log entry the project-disable-dtc CLI
    // emits.
    auto const &def = state.project->definition();
    auto const *dtc = def.find_dtc(kDtcCode);
    if (dtc == nullptr) {
        enqueue_toast(state, ToastKind::Warn,
                      std::string{"TGV+EGR delete: pack doesn't declare DTC "} +
                          kDtcCode + " — skipping bitmap edit. L1 zeroing "
                                     "alone may still suffice; check for "
                                     "P0400 after flashing.");
    } else {
        auto const *bm = def.find_dtc_bitmap(dtc->bitmap_id);
        if (bm == nullptr) {
            enqueue_toast(state, ToastKind::Warn,
                          std::string{"TGV+EGR delete: bitmap '"} +
                              dtc->bitmap_id + "' referenced by " + kDtcCode +
                              " not found in pack — skipping bitmap edit.");
        } else {
            // Read current state, then flip if currently enabled.
            // st::set_dtc_enabled writes to the ROM directly; we wrap
            // it in apply_op so the change shows up in History with
            // the workflow tag.
            auto &working = state.project->working_rom();
            auto const change = st::set_dtc_enabled(working, *bm, *dtc, false);
            if (!change.has_value()) {
                enqueue_toast(state, ToastKind::Warn,
                              "TGV+EGR delete: couldn't disable " +
                                  std::string{kDtcCode} + ": " +
                                  change.error().to_string());
            } else {
                // Record the change in History so Revert All sees it.
                std::vector<st::edit::ByteEdit::Change> changes;
                changes.push_back({change->address, change->before, change->after});
                auto edit = st::edit::Edit::bytes(std::move(changes),
                                                   "TGV+EGR delete: disable " +
                                                       std::string{kDtcCode})
                                .with_tag(kWorkflowId);
                state.project->active_history().record(std::move(edit));
            }
        }
    }

    auto const edits_recorded =
        state.project->active_history().cursor() - cursor_before;
    if (edits_recorded == 0) {
        enqueue_toast(state, ToastKind::Danger,
                      "TGV+EGR delete: no edits applied — your pack is missing "
                      "the table IDs this workflow expects. See bottom-bar "
                      "status for the first failure.");
        return;
    }
    enqueue_toast(state, ToastKind::Success,
                  "TGV+EGR delete applied (" + std::to_string(edits_recorded) +
                      " edits). Status-bar badge \xE2\x86\x92 Revert All to "
                      "undo as a unit. Flash the working ROM via Tools \xE2\x86\x92 "
                      "Flash to commit to the ECU.");
}

void draw_step_indicator(int current_step) {
    for (int i = 0; i < kStepCount; ++i) {
        if (i > 0) {
            ImGui::SameLine();
            ImGui::TextUnformatted(" \xE2\x80\xA2 ");
            ImGui::SameLine();
        }
        if (i == current_step) {
            ImGui::TextColored(chip_fg_accent(), "%d. %s", i + 1, kStepNames[i]);
        } else if (i < current_step) {
            ImGui::TextColored(chip_fg_ok(), "\xE2\x9C\x93 %s", kStepNames[i]);
        } else {
            text_subtle("%d. %s", i + 1, kStepNames[i]);
        }
    }
}

} // namespace

void render_tgv_egr_delete_modal(AppState &state) {
    if (!state.show_tgv_egr_delete_modal) {
        return;
    }

    // Centered standalone window with manual dim overlay — per memory
    // note `project_imgui_popupmodal_invisible.md`, OpenPopup + BeginPopupModal
    // doesn't reliably render for startup-fired modals; the plain Begin
    // pattern is the established workaround in this codebase.
    ImGuiViewport const *viewport = ImGui::GetMainViewport();
    ImVec2 const screen = viewport->WorkSize;
    ImVec2 const screen_pos = viewport->WorkPos;
    ImVec2 const window_size{800.0f, 580.0f};
    ImVec2 const window_pos{screen_pos.x + (screen.x - window_size.x) * 0.5f,
                            screen_pos.y + (screen.y - window_size.y) * 0.5f};
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);

    // Dim overlay (covers the main window so the modal looks modal).
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(screen_pos, ImVec2(screen_pos.x + screen.x,
                                          screen_pos.y + screen.y),
                      IM_COL32(0, 0, 0, 128));

    ImGuiWindowFlags const flags = ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin("TGV + EGR Delete (off-road only)##tgv_egr_delete",
                       &state.show_tgv_egr_delete_modal, flags)) {
        ImGui::End();
        return;
    }
    if (!state.show_tgv_egr_delete_modal) {
        reset_state();
        ImGui::End();
        return;
    }

    draw_step_indicator(g_state.step);
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::Separator();

    switch (g_state.step) {
    case 0: draw_jurisdiction_step(state); break;
    case 1: draw_hardware_step(state); break;
    case 2: draw_review_step(state); break;
    default:
        ImGui::TextUnformatted("(invalid step — please restart the workflow)");
        break;
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceL));
    ImGui::Separator();

    bool const at_first = g_state.step == 0;
    bool const at_last = g_state.step == kStepCount - 1;

    if (!at_first) {
        if (ImGui::Button("\xE2\x86\x90 Back", ImVec2(120.0f, 0.0f))) {
            --g_state.step;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
        state.show_tgv_egr_delete_modal = false;
        reset_state();
    }
    ImGui::SameLine();

    push_primary_button_colors();
    if (!at_last) {
        // Block forward navigation if the step's gate isn't satisfied.
        bool const step_blocked =
            (g_state.step == 0 && !jurisdiction_ok()) ||
            (g_state.step == 1 && !g_state.hardware_removed_confirmed);
        ImGui::BeginDisabled(step_blocked);
        if (ImGui::Button("Next \xE2\x86\x92", ImVec2(120.0f, 0.0f))) {
            ++g_state.step;
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("\xEE\x9C\xBE  Apply delete", ImVec2(160.0f, 0.0f))) {
            apply_tgv_egr_delete(state);
            state.show_tgv_egr_delete_modal = false;
            reset_state();
        }
    }
    pop_primary_button_colors();

    ImGui::End();
}

} // namespace st::ui
