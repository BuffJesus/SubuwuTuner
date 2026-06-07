// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// FA24-swap modal — guided 3-step recipe for the FA20→FA24 engine
// swap into a VA WRX. Opt-in entry point from the Welcome panel's
// "Common workflows" card (per pack-side [[workflow]] declaration).
//
// Why a dedicated modal: the FA24 swap touches 4 calibration tables
// out of ~200 in the LF79101P/LF79103P/LF9L000E packs. A user opening
// the table editor cold has no way to know which 4 matter or what to
// set them to. The modal collapses the decision down to two picks
// (cam strategy, basemap yes/no) and applies the 4 edits atomically
// with an in-app preview. See HANDOFF-from-analyst-2026-06-07-fa24-swap-uiux-plan.md
// for the full design rationale.
//
// The 4 tables this writes (when "use defaults" branch is taken):
//   1. engine_displacement                         → 2.4 L
//   2. fuel_timing_hpfp_base_offset                → +180 deg from stock
//   3. avcs_intake_*_intake_cam_target_*           → +2 deg per cell
//   4. fuel_injectors_pulse_injector_mult_table    → ×1.18 per cell
//
// Each edit routes through the standard apply_op() pipeline so the
// existing undo/redo and audit log surface them — "Revert all" is
// implemented by N sequential Undo invocations rather than as a
// dedicated transaction.
//
// SCAFFOLD STATUS (2026-06-07): three-step navigation + the
// pack-readiness check are wired; the actual apply_op calls are
// stubbed pending two upstream decisions (see TODO comments below):
//   - Per-cell math for AVCS offset + Injector Mult scaling needs
//     the apply_op interface extended to N-table batched edits or
//     simply iterated table-by-table — see TODO at apply_fa24_swap().
//   - NTM basemap byte-diff handling waits on the basemap arrival
//     (~week of 2026-06-09 per the user). Step 2's "Yes" branch is
//     currently a stub that prints a "not yet implemented" toast.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "panels/panels.hpp" // enqueue_toast
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

#include <array>
#include <string>
#include <string_view>

namespace st::ui {

namespace {

constexpr int kStepCount = 3;
constexpr char const *kStepNames[kStepCount] = {"Cam strategy", "Basemap", "Review"};

// Pack-side workflow identifier (matches the [[workflow]] entry in
// the pack TOML). Used to look up which pack declares FA24 swap
// support and to drive the disabled-state gating on the welcome card.
constexpr char const *kWorkflowId = "fa24_swap";

// Cam-strategy enum. The chosen strategy determines whether the
// software-side fix runs (Roberto + RS Motors paths skip it entirely
// because their hardware already aligns the FA24 cam signal with
// what the FA20 ECU expects).
enum class CamStrategy : std::uint8_t {
    KeepFA24Cams,   // NTM path — Atlas applies HPFP phase + AVCS offset
    SwapFA20Cams,   // Roberto path — mechanical fix, no software work
    RsMotorsKit,    // RS Motors trigger-wheels — no software work
};

// Basemap source. "UseDefaults" applies SubuwuTuner's best-guess
// starting values (placeholder until the basemap byte-diff is wired);
// "LoadBasemap" prompts for a .bin / .stune file and writes only the
// diff cells.
enum class BasemapSource : std::uint8_t {
    UseDefaults,
    LoadBasemap, // not yet implemented — stubbed in Step 2 branch
};

// Per-modal state — survives across frames while the modal is open.
// Reset on each open via reset_state(). The show-modal flag lives on
// AppState (state.show_fa24_swap_modal) so welcome-card / menubar
// callers can toggle it from their own translation units; this state
// holds only the step + decision selections, which are private to
// the modal's UI flow.
struct ModalState {
    int step{0};
    CamStrategy cam_strategy{CamStrategy::KeepFA24Cams};
    BasemapSource basemap_source{BasemapSource::UseDefaults};
};
static ModalState g_state;

void reset_state() {
    g_state.step = 0;
    g_state.cam_strategy = CamStrategy::KeepFA24Cams;
    g_state.basemap_source = BasemapSource::UseDefaults;
}

} // namespace

bool pack_supports_fa24_swap(AppState const &state) {
    if (!state.project.has_value()) {
        return false;
    }
    auto const &def = state.project->definition();
    // Required-table IDs are present in the in-tree packs as of
    // 2026-06-07 (lf79103p, lf9l000e, lf9d012h native; lf79101p
    // inherits via `extends = "lf79103p"`). The TOML-driven workflow
    // registry that would move this list into pack metadata is task
    // #2 — deferred until there's a second workflow shape worth
    // generalising for.
    constexpr std::array<char const *, 5> required = {
        "engine_displacement",
        "fuel_timing_hpfp_base_offset",
        "avcs_intake_barometric_multiplier_low_intake_cam_target_tgv_closed",
        "avcs_intake_barometric_multiplier_high_intake_cam_target_tgv_closed",
        "fuel_injectors_pulse_injector_mult_table",
    };
    for (auto const *id : required) {
        if (def.find_table(id) == nullptr) {
            return false;
        }
    }
    return true;
}

bool fa24_swap_active(AppState const &state) {
    if (!state.project.has_value()) {
        return false;
    }
    auto const &records = state.project->active_history().records();
    auto const cursor = state.project->active_history().cursor();
    if (cursor == 0) {
        return false;
    }
    // Head of applied range = records[cursor-1]. We treat the badge as
    // "lit" if ANY edit in the currently-applied range carries the tag,
    // not just the very last one — the user may have made an unrelated
    // manual edit on top of the workflow without reverting it.
    for (std::size_t i = 0; i < cursor; ++i) {
        if (records[i].tag == "fa24_swap") {
            return true;
        }
    }
    return false;
}

void revert_fa24_swap(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto &h = state.project->active_history();
    auto const reverted = h.undo_while_tag("fa24_swap");
    if (reverted.empty()) {
        // Either no tagged edits, or the head edit is untagged (user
        // added something on top). Tell them so they understand why
        // the button did nothing.
        if (fa24_swap_active(state)) {
            enqueue_toast(state, ToastKind::Warn,
                          "Revert All blocked: a non-workflow edit sits on top of the "
                          "FA24-swap batch. Undo manually until those are removed, then "
                          "Revert All will peel the workflow off.");
        } else {
            enqueue_toast(state, ToastKind::Info,
                          "Nothing to revert — no FA24-swap edits in this project's "
                          "history.");
        }
        return;
    }
    // Apply each restore in undo order (newest first). Same shape as
    // apply_history_step but the path-back is per-edit since this is
    // a batch and we're past the standard Ctrl+Z plumbing.
    st::Rom *target_rom = state.project->active_rom_mut();
    if (target_rom == nullptr) {
        enqueue_toast(state, ToastKind::Danger,
                      "Revert All failed: active ROM is read-only. Switch View → Active "
                      "ROM to an editable slot and retry.");
        return;
    }
    auto const &def = state.project->definition();
    std::size_t restored = 0;
    for (auto const *e : reverted) {
        if (auto const *t = e->as_table(); t != nullptr) {
            auto const *tbl = def.find_table(t->table_id);
            if (tbl == nullptr) {
                continue; // edit references a table the pack no longer has — skip
            }
            // Read the current grid, restore the "before" cells, write back.
            auto td_r = def.read_table_values(*target_rom, *tbl);
            if (!td_r.has_value()) {
                continue;
            }
            auto td = std::move(*td_r);
            if (!st::edit::restore(td, t->before).has_value()) {
                continue;
            }
            if (!def.write_table_values(*target_rom, *tbl, td).has_value()) {
                continue;
            }
            ++restored;
        } else if (auto const *b = e->as_byte(); b != nullptr) {
            // ByteEdit reverts are project-side — we'd need the
            // project-layer revert helper. Workflow modals don't write
            // ByteEdits today; if we hit one, skip and warn.
            (void)b;
        }
    }
    state.dirty = true;
    enqueue_toast(state, ToastKind::Success,
                  "Reverted FA24 swap (" + std::to_string(restored) + " edits undone).");
}

namespace {

void draw_step_indicator(int current_step) {
    ImVec4 const accent = accent_for(Theme::Dark).base;
    ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_muted());
    for (int i = 0; i < kStepCount; ++i) {
        if (i > 0)
            ImGui::SameLine();
        if (i == current_step) {
            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted("\xE2\x97\x8F"); // ●
            ImGui::PopStyleColor();
        } else if (i < current_step) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
            ImGui::TextUnformatted("\xE2\x9C\x93"); // ✓
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted("\xE2\x97\x8B"); // ○
        }
        if (i + 1 < kStepCount) {
            ImGui::SameLine();
            ImGui::TextUnformatted("\xE2\x80\x94"); // —
        }
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextUnformatted(kStepNames[current_step]);
    ImGui::Separator();
}

void draw_cam_strategy(AppState &state) {
    (void)state; // step body reads only g_state; AppState is for future per-pack copy
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // Lead with what the user is deciding, not "Step 1." The three
    // paths are valid choices for different builds — the modal's job
    // is to make the trade-offs visible, not to push one.
    ImGui::TextWrapped("Which cams are in your FA24 block? This determines whether "
                       "SubuwuTuner needs to shift HPFP timing and AVCS reference "
                       "in software, or whether your hardware already aligns the "
                       "FA24 signals with what the FA20 ECU expects.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    struct Choice {
        CamStrategy strategy;
        char const *name;
        char const *blurb;
    };
    constexpr std::array<Choice, 3> choices = {{
        {CamStrategy::KeepFA24Cams, "Keep the FA24 cams (recommended)",
         "Less labor. SubuwuTuner applies the HPFP phase shift and AVCS "
         "reference offset. Requires this software (no other tuning platform "
         "exposes the HPFP timing calibration)."},
        {CamStrategy::SwapFA20Cams, "Swap FA20 cams into the FA24 block",
         "More labor (transfer cam sensor plates via the C-clip swap onto the "
         "FA24 cam gears). No software phase fix needed — runs on any tuning "
         "platform."},
        {CamStrategy::RsMotorsKit, "Use the RS Motors swap kit",
         "FA20-pattern trigger wheels pre-installed on FA24 cams. ~$700 hardware. "
         "No software phase fix needed."},
    }};
    bool first = true;
    for (auto const &c : choices) {
        bool const selected = g_state.cam_strategy == c.strategy;
        if (first) {
            // Set focus on first radio so keyboard nav works
            ImGui::SetItemDefaultFocus();
        }
        first = false;
        if (ImGui::RadioButton(c.name, selected)) {
            g_state.cam_strategy = c.strategy;
        }
        ImGui::Indent();
        text_subtle("%s", c.blurb);
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    }
}

void draw_basemap(AppState &state) {
    (void)state;
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    // If the user picked a no-software-fix cam strategy in Step 1,
    // this step is effectively informational — there's nothing to
    // import because there's nothing to change.
    if (g_state.cam_strategy != CamStrategy::KeepFA24Cams) {
        ImGui::TextWrapped("You picked a hardware-only path. SubuwuTuner doesn't "
                           "need to apply software fixes for HPFP timing or AVCS "
                           "reference — your cam install handles the alignment. "
                           "This step has nothing to import.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        text_subtle("Step 3 will still confirm the Engine Displacement change "
                    "(2.0 L → 2.4 L), which applies regardless of cam strategy.");
        return;
    }

    ImGui::TextWrapped("Do you have a basemap to import?");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Not sure? Pick 'No' — SubuwuTuner's defaults are a safe starting "
                "point. You'll likely need bench or dyno time to refine either way.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    bool yes = g_state.basemap_source == BasemapSource::LoadBasemap;
    bool no_ = g_state.basemap_source == BasemapSource::UseDefaults;
    if (ImGui::RadioButton("Yes — load it from a .bin or .stune file", yes)) {
        g_state.basemap_source = BasemapSource::LoadBasemap;
    }
    ImGui::Indent();
    text_subtle("Recommended path. The basemap captures the exact values for HPFP "
                "timing, AVCS reference, and injector scaling that match your FA24 "
                "hardware. SubuwuTuner will diff it against stock and apply only "
                "the cells that differ.");
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

    if (ImGui::RadioButton("No — use SubuwuTuner's default starting values", no_)) {
        g_state.basemap_source = BasemapSource::UseDefaults;
    }
    ImGui::Indent();
    text_subtle("Best-guess starting point. Safe to start from but you'll want to "
                "log + verify HPFP rail pressure before sustained driving.");
    ImGui::Unindent();
}

void draw_review(AppState &state) {
    (void)state;
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    if (g_state.cam_strategy != CamStrategy::KeepFA24Cams) {
        // Hardware-only paths only flip Displacement; everything else
        // is left to the user's stock-FA20-aware calibration.
        ImGui::TextWrapped("This will change 1 calibration table in your pack:");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::BulletText("Engine - Displacement: 2.0 L → 2.4 L");
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        text_subtle("This is a calibration change only. No firmware patch. Revert "
                    "any edit individually under Tools → History after applying.");
        return;
    }

    // Software-fix path (KeepFA24Cams) — 4 edits
    ImGui::TextWrapped("This will change 4 calibration tables in your pack:");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    // Each row uses the same shape: name, before/after, why-line.
    auto change_row = [](char const *name, char const *transition, char const *why) {
        ImGui::BulletText("%s", name);
        ImGui::Indent();
        ImGui::TextUnformatted(transition);
        text_subtle("%s", why);
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    };

    change_row("Engine - Displacement", "2.0 L → 2.4 L",
               "Required for the MAF / load-calc math to track the larger engine.");
    change_row("Fuel - Timing - HPFP - Base Offset", "80.0° → 260.0°",
               "Shifts the pump command past the flipped triangle lobe on the FA24 "
               "cam. Default of +180° is a geometric estimate; the basemap byte-diff "
               "(when received) will refine this.");
    change_row("AVCS - Intake - Cam Target (TGV Open / Closed, Baro Low / High)",
               "+2° offset applied to every cell of all 4 tables",
               "Absorbs the FA24 cam sensor plate's ~2° tooth-angle difference "
               "from the FA20 plate (per the documented swap recipes).");
    change_row("Fuel - Injectors - Pulse - Injector Mult Table",
               "per-cell stock × 1.18",
               "Scales injector pulse-width for the FA24's higher injector flow rate.");

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("This is a calibration change only. No firmware patch. Revert "
                "any edit individually under Tools → History after applying, or "
                "use the FA24-swap-mode status badge → Revert All.");
}

void apply_fa24_swap(AppState &state) {
    // All edits route through apply_op_table() so each is recorded as
    // its own undoable history entry, surfaces in the audit log, and
    // flips the dirty flag. The shared transaction_tag ("fa24_swap")
    // is what powers the status-bar badge's Revert All affordance —
    // History::undo_while_tag walks back contiguous tag-matching edits.
    //
    // Basemap import path is still stubbed pending NTM basemap arrival
    // (~week of 2026-06-09); for now any "Yes" pick falls through to
    // the defaults branch with a warn toast so the user knows.
    if (g_state.basemap_source == BasemapSource::LoadBasemap) {
        // TODO(2026-06-09+): file-picker + byte-diff against stock.
        enqueue_toast(state, ToastKind::Warn,
                      "Basemap import isn't wired yet — applying SubuwuTuner defaults. "
                      "Refine via direct table edits once your basemap arrives.");
    }

    constexpr char const *kTag = "fa24_swap";
    std::size_t edits_recorded = 0;
    auto const cursor_before = state.project->active_history().cursor();

    // 1. Displacement — applies for every cam strategy. Single-cell
    //    table; "set whole table to 2.4" is correct regardless of its
    //    actual row/col layout.
    apply_op_table(state, "FA24 swap: Engine Displacement → 2.4 L", kTag,
                   "engine_displacement",
                   [](st::Definition::TableData &td, st::edit::Rect r) {
                       return st::edit::set_cells(td, r, 2.4);
                   });

    if (g_state.cam_strategy == CamStrategy::KeepFA24Cams) {
        // 2. HPFP base offset — flipped triangle lobe shift. 260° is
        //    the geometric default; the basemap byte-diff will refine
        //    once it arrives.
        apply_op_table(state, "FA24 swap: HPFP Base Offset → 260°", kTag,
                       "fuel_timing_hpfp_base_offset",
                       [](st::Definition::TableData &td, st::edit::Rect r) {
                           return st::edit::set_cells(td, r, 260.0);
                       });

        // 3a. AVCS Intake Cam Target — Baro Low × TGV Closed. +2°
        //     absorbs the FA24 cam sensor plate tooth-angle delta.
        apply_op_table(state, "FA24 swap: AVCS Intake Cam Target (Baro Low, TGV Closed) +2°",
                       kTag,
                       "avcs_intake_barometric_multiplier_low_intake_cam_target_tgv_closed",
                       [](st::Definition::TableData &td, st::edit::Rect r) {
                           return st::edit::add_cells(td, r, 2.0);
                       });

        // 3b. Same offset on the Baro High variant.
        apply_op_table(state, "FA24 swap: AVCS Intake Cam Target (Baro High, TGV Closed) +2°",
                       kTag,
                       "avcs_intake_barometric_multiplier_high_intake_cam_target_tgv_closed",
                       [](st::Definition::TableData &td, st::edit::Rect r) {
                           return st::edit::add_cells(td, r, 2.0);
                       });

        // 4. Injector multiplier — scale per-cell by FA24/FA20 flow
        //    ratio (1.18). Whole-table multiply.
        apply_op_table(state, "FA24 swap: Injector Mult Table ×1.18", kTag,
                       "fuel_injectors_pulse_injector_mult_table",
                       [](st::Definition::TableData &td, st::edit::Rect r) {
                           return st::edit::multiply_cells(td, r, 1.18);
                       });
    }

    edits_recorded = state.project->active_history().cursor() - cursor_before;
    if (edits_recorded == 0) {
        // Every apply_op_table call wrote a status_msg on its way out
        // (typically "table 'X' not in pack" — the workflow declared
        // pack support but the pack actually lacks the IDs the modal
        // expects). Surface a single toast so the user knows nothing
        // landed rather than relying on the bottom-bar status_msg
        // which might be obscured.
        enqueue_toast(state, ToastKind::Danger,
                      "FA24 swap: no edits applied — your pack is missing the table IDs "
                      "this workflow expects. See bottom-bar status for the first failure.");
        return;
    }
    enqueue_toast(state, ToastKind::Success,
                  "FA24 swap applied (" + std::to_string(edits_recorded) +
                      " edits). Status-bar badge → Revert All to undo as a unit.");
}

} // namespace

void render_fa24_swap_modal(AppState &state) {
    // Modal is opt-in only — opened from the Welcome panel's "Common
    // workflows" card or a future Tools menu entry by flipping
    // state.show_fa24_swap_modal to true.
    if (!state.show_fa24_swap_modal) {
        return;
    }

    // Re-validate pack support each frame in case the user swapped
    // packs between open and the next frame.
    if (!pack_supports_fa24_swap(state)) {
        state.show_fa24_swap_modal = false;
        enqueue_toast(state, ToastKind::Warn,
                      "FA24 swap workflow requires a pack with HPFP-Timing + AVCS + "
                      "Injector-Pulse coverage (LF79101P / LF79103P / LF9L000E). "
                      "Loaded pack doesn't qualify.");
        return;
    }

    ImGuiViewport const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    bool open = true;
    if (!ImGui::Begin("\xEE\x9D\xA8  FA24 Swap##fa24_swap", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (!open) {
        state.show_fa24_swap_modal = false;
        reset_state();
        ImGui::End();
        return;
    }

    draw_step_indicator(g_state.step);

    switch (g_state.step) {
    case 0:
        draw_cam_strategy(state);
        break;
    case 1:
        draw_basemap(state);
        break;
    case 2:
        draw_review(state);
        break;
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
        state.show_fa24_swap_modal = false;
        reset_state();
    }
    ImGui::SameLine();

    push_primary_button_colors();
    if (!at_last) {
        if (ImGui::Button("Next \xE2\x86\x92", ImVec2(120.0f, 0.0f))) {
            ++g_state.step;
        }
    } else {
        if (ImGui::Button("\xEE\x9C\xBE  Apply changes", ImVec2(160.0f, 0.0f))) {
            apply_fa24_swap(state);
            state.show_fa24_swap_modal = false;
            reset_state();
        }
    }
    pop_primary_button_colors();

    ImGui::End();
}

} // namespace st::ui
