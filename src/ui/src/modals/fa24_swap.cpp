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
// The tables this writes (when "use defaults" branch is taken),
// calibrated against the NTMotorsports FA24-swap basemap (cipher-decrypted
// 2026-06-09; see findings/ntm-fa24-basemap-2026-06-08/WORKFLOW_VALIDATION.md):
//   1. engine_displacement                         → 2.4 L
//   2. fuel_timing_hpfp_lobe_phase_descriptor      → 0x3D37322C (NTM bytes)
//      The 4-byte cluster at canon 0x49BA0; the named Base Offset at
//      0x49BA8 stays stock (NTM doesn't tune that uint16). Per-byte
//      semantics aren't RE'd yet — defaults branch writes the exact
//      NTM pattern; basemap branch copies whatever the user's basemap
//      contains.
//   3. avcs_intake_*_intake_cam_target_tgv_closed  → load from basemap (default)
//      NTM does a structured 2D retune, not a uniform delta — the
//      +1.45° cell-mean-delta defaults exist as a low-magnitude
//      fallback. Step 2 defaults to "Yes, load basemap" so users
//      get NTM's exact per-cell curves. Baro Low == Baro High in
//      both stock and NTM, so they share the same default.
//   4. fuel_injectors_pulse_injector_mult_table    → ×1.43 per cell
//      NTM scales uniformly by ~1.43, not 1.18 (the pre-NTM-data guess).
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

#include "st/rom.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <array>
#include <filesystem>
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
// starting values. "LoadBasemap" reads the user-picked .bin and
// copies the workflow tables' values cell-by-cell into the working
// ROM, recording each table as its own tagged Edit.
enum class BasemapSource : std::uint8_t {
    UseDefaults,
    LoadBasemap,
};

// Per-modal state — survives across frames while the modal is open.
// Reset on each open via reset_state(). The show-modal flag lives on
// AppState (state.show_fa24_swap_modal) so welcome-card / menubar
// callers can toggle it from their own translation units; this state
// holds only the step + decision selections, which are private to
// the modal's UI flow.
// Severity of the basemap status line — colors the text under the
// "Yes" radio after a file is picked. Warn highlights things like
// CID mismatch or unrecognized firmware so the user notices before
// committing to Apply.
enum class BasemapStatusSeverity : std::uint8_t { Ok, Warn, Error };

struct ModalState {
    int step{0};
    CamStrategy cam_strategy{CamStrategy::KeepFA24Cams};
    // Default to LoadBasemap. NTM's per-cell AVCS retune can't be
    // honestly expressed as a uniform delta, and the HPFP lobe-phase
    // descriptor's per-byte semantics aren't RE'd — both are best
    // served by copying the user's NTM basemap. UseDefaults remains
    // a fallback for users who don't have a basemap in hand. Step 2's
    // "No" radio still works; it's just not the default any more.
    BasemapSource basemap_source{BasemapSource::LoadBasemap};
    std::filesystem::path basemap_path;
    std::string basemap_status; // shown under the "Yes" radio when set
    BasemapStatusSeverity basemap_status_severity{BasemapStatusSeverity::Ok};
};
static ModalState g_state;

void reset_state() {
    g_state.step = 0;
    g_state.cam_strategy = CamStrategy::KeepFA24Cams;
    g_state.basemap_source = BasemapSource::LoadBasemap;
    g_state.basemap_path.clear();
    g_state.basemap_status.clear();
    g_state.basemap_status_severity = BasemapStatusSeverity::Ok;
}

} // namespace

bool pack_supports_fa24_swap(AppState const &state) {
    if (!state.project.has_value()) {
        return false;
    }
    // Pack must declare the workflow via a [[workflow]] block.
    // Definition::supports_workflow validates each required_tables id
    // resolves to a present table. This is the only gate — there was
    // a transitional hardcoded table-presence fallback until 2026-06-07
    // that masked packs with the required IDs but unverified data
    // (lf75404h's HPFP cluster failed analyst validation while still
    // matching by name, for example). Dropping the fallback aligns
    // eligibility with explicit pack-author opt-in.
    return state.project->definition().supports_workflow("fa24_swap");
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
    text_subtle("Recommended: pick 'Yes' and load your tuner's FA24 basemap (e.g. NTM's). "
                "The AVCS cam targets are per-cell, not uniform — defaults approximate them "
                "with a mean delta, basemap gives you the exact curve. Either way, bench / "
                "dyno time is still required to refine.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    bool yes = g_state.basemap_source == BasemapSource::LoadBasemap;
    bool no_ = g_state.basemap_source == BasemapSource::UseDefaults;
    if (ImGui::RadioButton("Yes — load it from a .bin file", yes)) {
        g_state.basemap_source = BasemapSource::LoadBasemap;
    }
    ImGui::Indent();
    text_subtle("Reads the workflow tables (HPFP timing, AVCS targets, injector mult, "
                "displacement) from your basemap and copies them into the working ROM. "
                "Basemap must be the same firmware family as the loaded project pack.");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    // Browse button always available — clicking it auto-selects the
    // Yes radio so the user doesn't have to make two clicks for the
    // common "find the file then proceed" flow.
    if (ImGui::Button("Browse\xE2\x80\xA6##fa24_basemap")) {
        nfdu8filteritem_t const filters[] = {{"ROM image", "bin"}};
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
        if (r == NFD_OKAY) {
            g_state.basemap_path = std::filesystem::path{out.get()};
            g_state.basemap_source = BasemapSource::LoadBasemap;
            // Sanity-check size against the loaded project's source
            // ROM size while we have the path in hand — surfaces an
            // obvious mismatch (wrong firmware family / partial dump)
            // before the user gets to Step 3 Apply.
            std::error_code ec;
            auto const sz = std::filesystem::file_size(g_state.basemap_path, ec);
            auto const expected = state.project->source_rom().data().size();
            if (ec) {
                g_state.basemap_status = "Couldn't stat the picked file: " + ec.message();
                g_state.basemap_status_severity = BasemapStatusSeverity::Error;
            } else if (sz != expected) {
                g_state.basemap_status =
                    "Size mismatch: basemap is " + std::to_string(sz) +
                    " bytes, project expects " + std::to_string(expected) +
                    " bytes. Likely wrong firmware family.";
                g_state.basemap_status_severity = BasemapStatusSeverity::Error;
            } else {
                // Size matches — go one step further and run the pack's
                // identification check against the basemap's bytes. If the
                // basemap is the wrong CID (e.g. an LF79103P stock cal
                // dropped into an LF79101P project, or a CID this pack
                // doesn't recognize at all), the workflow would copy
                // values whose row/col layout matches but whose meaning
                // is for a different firmware. Warning here is cheap
                // (~one 2 MB read on file pick) and prevents a confusing
                // mid-apply discovery.
                auto basemap_rom = st::Rom::from_file(g_state.basemap_path);
                if (!basemap_rom.has_value()) {
                    g_state.basemap_status =
                        "Couldn't load file as a ROM: " + basemap_rom.error().to_string();
                    g_state.basemap_status_severity = BasemapStatusSeverity::Error;
                } else {
                    auto const &def = state.project->definition();
                    auto const basemap_cid = def.matches(*basemap_rom);
                    auto const project_cid = def.matches(state.project->source_rom());
                    std::string const fname = g_state.basemap_path.filename().string();
                    if (basemap_cid.has_value() && project_cid.has_value() &&
                        *basemap_cid == *project_cid) {
                        g_state.basemap_status =
                            "Selected: " + fname + "  \xC2\xB7  CID " + *basemap_cid + " (match)";
                        g_state.basemap_status_severity = BasemapStatusSeverity::Ok;
                    } else if (basemap_cid.has_value() && project_cid.has_value()) {
                        g_state.basemap_status =
                            "Selected: " + fname + "  \xC2\xB7  WARNING basemap CID is " +
                            *basemap_cid + ", project CID is " + *project_cid +
                            ". Workflow may apply values for a different firmware.";
                        g_state.basemap_status_severity = BasemapStatusSeverity::Warn;
                    } else if (basemap_cid.has_value()) {
                        // Project's source didn't identify against this
                        // pack — shouldn't happen in normal flow but
                        // surface what we found in the basemap so the
                        // user has something to compare to manually.
                        g_state.basemap_status =
                            "Selected: " + fname + "  \xC2\xB7  CID " + *basemap_cid +
                            " (project CID unrecognized — verify manually)";
                        g_state.basemap_status_severity = BasemapStatusSeverity::Warn;
                    } else {
                        // Basemap doesn't match any CID this pack
                        // declares. Could still apply if the table
                        // layout happens to line up, but the user
                        // should know.
                        g_state.basemap_status =
                            "Selected: " + fname +
                            "  \xC2\xB7  WARNING basemap CID unrecognized by this pack. "
                            "Verify it's the same firmware family before applying.";
                        g_state.basemap_status_severity = BasemapStatusSeverity::Warn;
                    }
                }
            }
        } else if (r == NFD_ERROR) {
            g_state.basemap_status = std::string{"File dialog error: "} + NFD::GetError();
            g_state.basemap_status_severity = BasemapStatusSeverity::Error;
        }
    }
    ImGui::SameLine();
    if (g_state.basemap_path.empty()) {
        text_subtle("No file picked yet.");
    } else {
        ImVec4 const color = g_state.basemap_status_severity == BasemapStatusSeverity::Error
                                 ? chip_fg_danger()
                                 : (g_state.basemap_status_severity == BasemapStatusSeverity::Warn
                                        ? chip_fg_caution()
                                        : chip_fg_ok());
        ImGui::TextColored(color, "%s", g_state.basemap_status.c_str());
    }
    // Mirror the review-screen warning here: when the user is on the
    // LoadBasemap default but hasn't picked a file yet, Apply will
    // silently fall through to SubuwuTuner defaults. Surfacing it on
    // Step 2 (where the choice happens) is more honest than waiting
    // for Step 3 to point out the gap.
    if (g_state.basemap_source == BasemapSource::LoadBasemap &&
        g_state.basemap_path.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::TextColored(chip_fg_caution(),
                           "\xE2\x9A\xA0  Apply will fall back to defaults until you "
                           "pick a basemap file.");
    }
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

    if (ImGui::RadioButton("No \xE2\x80\x94 fall back to SubuwuTuner's default values", no_)) {
        g_state.basemap_source = BasemapSource::UseDefaults;
    }
    ImGui::Indent();
    text_subtle("Fallback only. AVCS targets get cell-mean approximations of NTM's per-cell "
                "retune; HPFP lobe-phase descriptor gets NTM's exact byte pattern. Safe to "
                "start from but log + verify HPFP rail pressure before sustained driving.");
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

    bool const from_basemap = g_state.basemap_source == BasemapSource::LoadBasemap &&
                              !g_state.basemap_path.empty();

    // Surface the case where the user left Step 2 on "Yes" (the
    // default) but never picked a file. Apply still falls through to
    // defaults, but staying silent would let the user think they're
    // getting NTM-exact curves when they're not.
    if (g_state.basemap_source == BasemapSource::LoadBasemap &&
        g_state.basemap_path.empty()) {
        ImVec4 const color = chip_fg_caution();
        ImGui::TextColored(color,
                           "No basemap picked yet \xE2\x80\x94 these edits will use "
                           "SubuwuTuner's fallback defaults. Click Back to pick a basemap "
                           "for NTM-exact values.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    }

    // Software-fix path (KeepFA24Cams) — 5 edits
    ImGui::TextWrapped("This will change 5 calibration tables in your pack:");
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));

    // Each row uses the same shape: name, transition, why-line.
    auto change_row = [](char const *name, char const *transition, char const *why) {
        ImGui::BulletText("%s", name);
        ImGui::Indent();
        ImGui::TextUnformatted(transition);
        text_subtle("%s", why);
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    };

    if (from_basemap) {
        // Basemap path: every workflow table gets the basemap's values
        // copied wholesale into the working ROM. We don't enumerate
        // per-cell deltas here — the basemap may differ from stock on
        // dozens of cells per table, which would make the Review screen
        // unreadable. Instead surface the source file as the source of
        // truth, with a one-line per-table description.
        change_row("Engine - Displacement", "copied from basemap",
                   "FA24 cars run 2.4 L; the basemap captures the exact stored value.");
        change_row("Fuel - Timing - HPFP - Lobe Phase Descriptor",
                   "copied from basemap",
                   "4-byte cluster at canon 0x49BA0 that NTM-style basemaps rewrite "
                   "for the FA24 cam lobe-phase fix.");
        change_row("AVCS - Intake - Cam Target (Baro Low, TGV Closed)",
                   "copied from basemap",
                   "Tuned AVCS reference for the FA24 cam sensor plate (Baro Low).");
        change_row("AVCS - Intake - Cam Target (Baro High, TGV Closed)",
                   "copied from basemap",
                   "Tuned AVCS reference for the FA24 cam sensor plate (Baro High).");
        change_row("Fuel - Injectors - Pulse - Injector Mult Table",
                   "copied from basemap",
                   "Tuned injector scaling for the FA24's injector flow rate.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        text_subtle("Source: %s", g_state.basemap_path.filename().string().c_str());
    } else {
        change_row("Engine - Displacement", "2.0 L → 2.4 L",
                   "Required for the MAF / load-calc math to track the larger engine.");
        change_row("Fuel - Timing - HPFP - Lobe Phase Descriptor",
                   "0x372C2C2C → 0x3D37322C (NTM bytes)",
                   "4-byte cluster at canon 0x49BA0 that NTM's basemap rewrites for the "
                   "FA24 cam lobe-phase fix. Per-byte semantics aren't RE'd yet; the "
                   "default writes NTM's exact byte pattern atomically. Safety-critical: "
                   "wrong values risk HPFP pressure faults.");
        change_row("AVCS - Intake - Cam Target (Baro Low, TGV Closed)",
                   "+1.45° offset applied to every cell (NTM cell-mean delta)",
                   "NTM does a structured 2D retune (some cells +30°, some -70°); "
                   "the +1.45° default is the cell-mean delta — a small nudge, not "
                   "a substitute. Load NTM basemap for the per-cell curve.");
        change_row("AVCS - Intake - Cam Target (Baro High, TGV Closed)",
                   "+1.45° offset applied to every cell (NTM cell-mean delta)",
                   "Same shape and mean delta as Baro Low — NTM keeps the two "
                   "tables identical. Load NTM basemap for the exact curve.");
        change_row("Fuel - Injectors - Pulse - Injector Mult Table",
                   "per-cell stock × 1.43",
                   "Scales injector pulse-width for the FA24's higher flow rate. "
                   "NTM's uniform ×1.43 multiplier; calibrated against their basemap.");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    text_subtle("This is a calibration change only. No firmware patch. Revert "
                "any edit individually under Tools → History after applying, or "
                "use the FA24-swap-mode status badge → Revert All.");
}

// Read the named table from `basemap_rom` via the project's
// definition (so axis sizing + scaling line up), then write each
// cell into the working ROM's same table. Recorded as a single
// tagged Edit (revert_fa24_swap peels them off as a batch). 1D and
// 2D tables are handled via TableData.values; 3D tables (which the
// FA24 workflow doesn't touch today) would need an extra branch on
// TableData.slices.
void copy_table_from_basemap(AppState &state, std::string const &label,
                             std::string const &table_id, st::Rom const &basemap_rom) {
    auto const &def = state.project->definition();
    auto const *tbl = def.find_table(table_id);
    if (tbl == nullptr) {
        state.status_msg = label + ": table '" + table_id + "' not in pack";
        return;
    }
    auto basemap_td_r = def.read_table_values(basemap_rom, *tbl);
    if (!basemap_td_r.has_value()) {
        state.status_msg = label + ": basemap read: " + basemap_td_r.error().to_string();
        return;
    }
    auto const basemap_td = std::move(*basemap_td_r);

    apply_op_table(state, label, "fa24_swap", table_id,
                   [&basemap_td](st::Definition::TableData &td,
                                 st::edit::Rect r) -> st::Status {
                       // Refuse to write back if the basemap's grid
                       // shape doesn't line up with the working ROM's —
                       // that's a sign the pack changed shape between
                       // the basemap's vintage and now (or the basemap
                       // is the wrong firmware).
                       if (basemap_td.values.size() != td.values.size()) {
                           return st::failure(st::ErrorCode::InvalidArgument,
                                              "basemap row count mismatch");
                       }
                       for (std::size_t row = r.r_start; row <= r.r_end; ++row) {
                           if (basemap_td.values[row].size() != td.values[row].size()) {
                               return st::failure(st::ErrorCode::InvalidArgument,
                                                  "basemap col count mismatch");
                           }
                           for (std::size_t col = r.c_start; col <= r.c_end; ++col) {
                               td.values[row][col] = basemap_td.values[row][col];
                           }
                       }
                       return st::ok();
                   });
}

// Workflow table descriptor — what to edit + how, parameterised so
// the apply loop is a single pass over kWorkflowTables instead of an
// open-coded sequence. Adding a 5th workflow table (or a 6th when the
// AVCS TGV-open variants land) is a one-line addition here, no apply-
// path changes. The default op signature is (TableData&, Rect, double)
// — all of set_cells / add_cells / multiply_cells fit.
using EditOp = st::Status (*)(st::Definition::TableData &, st::edit::Rect, double);

struct WorkflowTable {
    char const *table_id;
    char const *label_basemap; // edit-description string when copying from basemap
    char const *label_default; // edit-description string for the defaults branch
    EditOp default_op;         // op the defaults branch invokes
    double default_arg;        // single double argument for set_cells / add_cells / multiply_cells
    bool needs_keep_fa24_cams; // false = always apply; true = skipped on hardware-only cam paths
};

// Order matters — apply_fa24_swap iterates this verbatim and the
// status bar's "Revert All" undoes in reverse. The 4 keep-FA24-cams
// edits should land contiguously so undo_while_tag peels them as one
// batch even when the user adds an unrelated edit on top later.
// Defaults calibrated against NTMotorsports' FA24-swap basemap
// (cipher-decrypted 2026-06-09; see findings/ntm-fa24-basemap-2026-06-08/
// WORKFLOW_VALIDATION.md for the per-table numerical comparison).
constexpr std::array<WorkflowTable, 5> kWorkflowTables = {{
    {"engine_displacement",
     "FA24 swap: Engine Displacement (basemap)",
     "FA24 swap: Engine Displacement \xE2\x86\x92 2.4 L",
     &st::edit::set_cells, 2.4, /*needs_keep_fa24_cams=*/false},
    // HPFP lobe-phase descriptor at canon 0x49BA0 — the 4-byte cluster
    // NTM's FA24-swap basemap actually rewrites (stock 0x372C2C2C →
    // NTM 0x3D37322C). Treated here as a single uint32 because per-byte
    // semantics aren't RE'd; the default writes the exact NTM byte
    // pattern atomically via set_cells. Once the bytes' per-cylinder /
    // phase-pair role is confirmed, the table will split into per-byte
    // cells and this entry will switch to a structured op.
    {"fuel_timing_hpfp_lobe_phase_descriptor",
     "FA24 swap: HPFP Lobe Phase Descriptor (basemap)",
     "FA24 swap: HPFP Lobe Phase Descriptor \xE2\x86\x92 NTM 0x3D37322C",
     &st::edit::set_cells, static_cast<double>(0x3D37322Cu), true},
    // NTM does a structured 2D retune (not uniform delta, not constant).
    // Stock and NTM both keep Baro Low == Baro High at every cell, and
    // the cell-mean delta is +1.45 deg for both tables (verified
    // 2026-06-09 against the correct 10x16 shape — earlier +10.5/+8
    // figures were computed against an inflated 22x24 table size and
    // were wrong). For an exact NTM match, load the basemap; the
    // defaults below are a low-magnitude fallback that nudges the
    // table in the right direction without lying about precision.
    {"avcs_intake_barometric_multiplier_low_intake_cam_target_tgv_closed",
     "FA24 swap: AVCS Intake Cam Target (Baro Low, TGV Closed) (basemap)",
     "FA24 swap: AVCS Intake Cam Target (Baro Low, TGV Closed) +1.45\xC2\xB0 mean",
     &st::edit::add_cells, 1.45, true},
    {"avcs_intake_barometric_multiplier_high_intake_cam_target_tgv_closed",
     "FA24 swap: AVCS Intake Cam Target (Baro High, TGV Closed) (basemap)",
     "FA24 swap: AVCS Intake Cam Target (Baro High, TGV Closed) +1.45\xC2\xB0 mean",
     &st::edit::add_cells, 1.45, true},
    {"fuel_injectors_pulse_injector_mult_table",
     "FA24 swap: Injector Mult Table (basemap)",
     "FA24 swap: Injector Mult Table \xC3\x97""1.43",
     &st::edit::multiply_cells, 1.43, true},
}};

void apply_fa24_swap(AppState &state) {
    // All edits route through apply_op_table() so each is recorded as
    // its own undoable history entry, surfaces in the audit log, and
    // flips the dirty flag. The shared transaction_tag (kWorkflowId)
    // is what powers the status-bar badge's Revert All affordance —
    // History::undo_while_tag walks back contiguous tag-matching edits.

    bool const from_basemap = g_state.basemap_source == BasemapSource::LoadBasemap &&
                              !g_state.basemap_path.empty();

    // Load the basemap up-front (one shared Rom) so we don't re-read
    // 2 MB N times. Fall back to defaults if the load fails so the
    // user gets something rather than nothing — surfaces the failure
    // via a warn toast.
    std::optional<st::Rom> basemap_rom;
    if (from_basemap) {
        auto r = st::Rom::from_file(g_state.basemap_path);
        if (!r.has_value()) {
            enqueue_toast(state, ToastKind::Warn,
                          std::string{"Couldn't load basemap ("} + r.error().to_string() +
                              "). Falling back to SubuwuTuner defaults.");
        } else {
            basemap_rom = std::move(*r);
        }
    }

    auto const cursor_before = state.project->active_history().cursor();

    // Single pass over the descriptor table. needs_keep_fa24_cams skips
    // HPFP/AVCS/Injector when the user picked a hardware-only cam path
    // (Displacement is the only descriptor with needs_keep_fa24_cams=false
    // so it lands for every strategy).
    for (auto const &wt : kWorkflowTables) {
        if (wt.needs_keep_fa24_cams && g_state.cam_strategy != CamStrategy::KeepFA24Cams) {
            continue;
        }
        if (basemap_rom.has_value()) {
            copy_table_from_basemap(state, wt.label_basemap, wt.table_id, *basemap_rom);
        } else {
            apply_op_table(state, wt.label_default, kWorkflowId, wt.table_id,
                           [op = wt.default_op, arg = wt.default_arg](
                               st::Definition::TableData &td, st::edit::Rect r) {
                               return op(td, r, arg);
                           });
        }
    }

    auto const edits_recorded =
        state.project->active_history().cursor() - cursor_before;
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
    std::string const source_note = basemap_rom.has_value()
                                        ? std::string{" (from "} +
                                              g_state.basemap_path.filename().string() + ")"
                                        : std::string{" (defaults)"};
    enqueue_toast(state, ToastKind::Success,
                  "FA24 swap applied (" + std::to_string(edits_recorded) + " edits)" +
                      source_note + ". Status-bar badge → Revert All to undo as a unit.");
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
