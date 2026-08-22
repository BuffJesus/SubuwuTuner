// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Project Readiness — the offline-first orientation surface. This panel is
// deliberately honest about the boundary between a coherent local project,
// a valid flash plan, and a live ECU that has actually been identified and
// verified. A loaded ROM is useful; it is not proof that an ECU is ready.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "modals/modals.hpp"
#include "persistence.hpp"
#include "project_io.hpp" // save_project
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/edit.hpp"

#include "st/defs.hpp"
#include "st/library/guided_tasks.hpp"
#include "st/policy.hpp"
#include "st/tune_export.hpp"

#include <imgui.h>

#include <span>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace st::ui {
namespace {

struct LocalSummary {
    std::size_t changed_bytes{0};
    std::size_t changed_sectors{0};
    std::uint32_t source_crc{0};
    std::uint32_t working_crc{0};
};

struct GuidedTaskState {
    std::filesystem::path project_dir;
    std::array<bool, 7> complete{};
    std::array<std::array<char, 256>, 7> notes{};
};

static constexpr std::array<std::string_view, 7> kGuidedTaskIds{
    "identity-baseline", "fueling-maf", "boost-response", "ignition-knock",
    "cold-start", "fa24-preparation", "final-review"};

GuidedTaskState &guided_state() {
    static GuidedTaskState value;
    return value;
}

std::filesystem::path guided_state_read_path(st::Project const &project) {
    return state_sidecar::read_path(project.dir(), "guided-tasks.toml");
}

std::filesystem::path guided_state_write_path(st::Project const &project) {
    return state_sidecar::write_path(project.dir(), "guided-tasks.toml");
}

void save_guided_state(st::Project const &project);

void load_guided_state(st::Project const &project) {
    auto &state = guided_state();
    if (state.project_dir == project.dir()) {
        return;
    }
    state = {};
    state.project_dir = project.dir();
    std::ifstream in{guided_state_read_path(project), std::ios::binary};
    if (in) {
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto decoded = st::library::guided_tasks::parse(buffer.str());
        if (decoded.has_value()) {
            for (auto const &record : decoded->tasks) {
                auto const it = std::find(kGuidedTaskIds.begin(), kGuidedTaskIds.end(), record.id);
                if (it == kGuidedTaskIds.end()) {
                    continue;
                }
                auto const index = static_cast<std::size_t>(
                    std::distance(kGuidedTaskIds.begin(), it));
                state.complete[index] = record.complete;
                std::snprintf(state.notes[index].data(), state.notes[index].size(), "%s",
                              record.note.c_str());
            }
            return;
        }
    }

    // One-time migration from the initial text sidecar. A successful legacy
    // read is immediately rewritten through the versioned library serializer.
    std::ifstream legacy{state_sidecar::read_path(project.dir(), "guided-tasks.txt")};
    std::string schema;
    if (!(legacy >> schema) || schema != "subuwutuner-guided-tasks-v1") {
        return;
    }
    std::size_t index = 0;
    int complete = 0;
    std::string note;
    while (legacy >> index >> complete >> std::quoted(note)) {
        if (index >= state.complete.size()) {
            continue;
        }
        state.complete[index] = complete != 0;
        std::snprintf(state.notes[index].data(), state.notes[index].size(), "%s", note.c_str());
    }
    save_guided_state(project);
}

void save_guided_state(st::Project const &project) {
    auto const &state = guided_state();
    st::library::guided_tasks::State serializable;
    for (std::size_t i = 0; i < state.complete.size(); ++i) {
        serializable.tasks.push_back(
            {std::string{kGuidedTaskIds[i]}, state.complete[i], state.notes[i].data()});
    }
    auto encoded = st::library::guided_tasks::serialize(serializable);
    if (!encoded.has_value()) {
        return;
    }
    std::ofstream out{guided_state_write_path(project), std::ios::binary | std::ios::trunc};
    if (out) {
        out << *encoded;
        if (out) {
            state_sidecar::remove_legacy(project.dir(), "guided-tasks.toml");
        }
    }
}

void open_table_search(AppState &state, char const *query) {
    apply_workspace_mode(state, WorkspaceMode::Tune);
    std::snprintf(state.table_filter, sizeof state.table_filter, "%s", query);
    state.focus_table_filter = true;
}

LocalSummary summarize_local(st::Project const &project) {
    LocalSummary out;
    auto const source = project.source_rom().data();
    auto const working = project.working_rom().data();
    out.source_crc = project.source_rom().crc32();
    out.working_crc = project.working_rom().crc32();

    constexpr std::size_t kSectorSize = 0x1000;
    std::size_t const n = source.size() < working.size() ? source.size() : working.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (source[i] != working[i]) {
            ++out.changed_bytes;
        }
    }
    if (out.changed_bytes != 0) {
        for (std::size_t base = 0; base < n; base += kSectorSize) {
            std::size_t const end = (base + kSectorSize < n) ? base + kSectorSize : n;
            bool changed = false;
            for (std::size_t i = base; i < end; ++i) {
                if (source[i] != working[i]) {
                    changed = true;
                    break;
                }
            }
            if (changed) {
                ++out.changed_sectors;
            }
        }
    }
    return out;
}

void readiness_row(char const *label, char const *value, ImVec4 fg, ImVec4 bg) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(190.0f);
    chip(value, fg, bg);
}

void action_button(AppState &state, char const *label, char const *hint,
                   bool enabled, bool &clicked) {
    ImGui::BeginDisabled(!enabled);
    clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", hint);
    }
    (void)state;
}

void render_guided_tasks(AppState &state, st::Project &project, bool has_checkpoint,
                         bool has_changes) {
    struct Task {
        char const *title;
        char const *inputs;
        char const *evidence;
        char const *action;
    };
    static constexpr std::array<Task, 7> tasks{{
        {"1. Identity and baseline", "Exact source ROM, matching definition, stock checkpoint",
         "CID match, pack lint, immutable baseline", "Open identity maps"},
        {"2. Fueling and MAF", "MAF scaling, injector data, fueling log",
         "Measured trims/lambda and reversible map delta", "Browse fueling maps"},
        {"3. Boost response", "Target boost, wastegate tables, boost-control log",
         "Target-versus-actual response without unsafe overshoot", "Open boost analysis"},
        {"4. Ignition and knock", "Timing maps, DAM/FBKC/FLKC log, fuel context",
         "Clean repeatable pulls and reviewed safety pairs", "Open knock analysis"},
        {"5. Cold start and drivability", "ECT/IAT/RPM/lambda cold-start log",
         "Stable start, warm-up curve, and repeatable idle", "Open cold-start analysis"},
        {"6. FA24 preparation", "Supported pack, displacement/HPFP/AVCS/injector facts",
         "Atomic reversible plan; hardware remains unverified", "Open FA24 workflow"},
        {"7. Final review and recovery", "Semantic diff, checkpoint, recovery image and plan",
         "Every blocker has an action; post-power-cycle checks remain pending", "Review changes"},
    }};

    load_guided_state(project);
    auto &progress = guided_state();
    if (!ImGui::CollapsingHeader("Guided tuning tasks", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    text_subtle("Completion records local work only. Hardware verification is always separate.");
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool open = ImGui::TreeNodeEx(tasks[i].title, ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::SameLine();
        chip("Not verified on hardware", chip_fg_caution(), chip_bg_caution());
        if (open) {
            ImGui::TextDisabled("Inputs");
            ImGui::SameLine(110.0f);
            ImGui::TextWrapped("%s", tasks[i].inputs);
            ImGui::TextDisabled("Evidence");
            ImGui::SameLine(110.0f);
            ImGui::TextWrapped("%s", tasks[i].evidence);
            if (i == 0 && !has_checkpoint) {
                ImGui::TextColored(chip_fg_caution(),
                                   "Baseline evidence is incomplete: create a checkpoint below.");
            }

            bool enabled = true;
            if (i == 5) {
                enabled = pack_supports_fa24_swap(state);
            } else if (i == 6) {
                enabled = has_changes;
            }
            ImGui::BeginDisabled(!enabled);
            if (ImGui::Button(tasks[i].action)) {
                switch (i) {
                case 0:
                    open_table_search(state, "identity");
                    break;
                case 1:
                    open_table_search(state, "fuel");
                    break;
                case 2:
                    state.show_ebcs_panel = true;
                    state.show_log_explorer_panel = true;
                    break;
                case 3:
                    state.show_knock_dashboard_panel = true;
                    state.show_log_explorer_panel = true;
                    break;
                case 4:
                    state.show_coldstart_panel = true;
                    state.show_log_explorer_panel = true;
                    break;
                case 5:
                    state.show_fa24_swap_modal = true;
                    break;
                case 6:
                    state.show_compare_panel = true;
                    break;
                default:
                    break;
                }
            }
            ImGui::EndDisabled();
            if (!enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(i == 5 ? "The loaded pack does not declare FA24 support."
                                         : "Make or import a change before final review.");
            }
            bool complete = progress.complete[i];
            if (ImGui::Checkbox("Local task complete", &complete)) {
                progress.complete[i] = complete;
                save_guided_state(project);
            }
            ImGui::InputTextWithHint("##guided_note", "Completion note / evidence reference",
                                     progress.notes[i].data(), progress.notes[i].size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                save_guided_state(project);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

} // namespace

void render_readiness_panel(AppState &state) {
    if (!state.show_readiness_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Project Readiness", &state.show_readiness_panel)) {
        ImGui::End();
        return;
    }

    if (!state.project.has_value()) {
        render_empty_state("No project", "Open a ROM project to see its readiness state.");
        ImGui::End();
        return;
    }

    auto &project = *state.project;
    auto const summary = summarize_local(project);
    auto const &pack = project.definition().pack();
    auto const source_match = project.definition().match_info(project.source_rom());
    std::size_t approved_calibration_regions = 0;
    std::size_t candidate_calibration_regions = 0;
    if (source_match.has_value()) {
        for (auto const &region : project.definition().calibration_regions()) {
            if (region.cid != source_match->cid)
                continue;
            if (region.status == "approved") {
                ++approved_calibration_regions;
            } else {
                ++candidate_calibration_regions;
            }
        }
    }
    bool const source_shape_ok = !project.source_rom().empty() &&
                                 project.source_rom().size() == pack.rom_size_bytes;
    bool const working_shape_ok = project.working_rom().size() == project.source_rom().size();
    bool const definition_ok = project.definition().validate().has_value();
    bool const has_changes = summary.changed_bytes != 0;
    bool const has_checkpoint = !project.checkpoints().empty();
    bool const has_checkpoint_warning = !project.checkpoint_warnings().empty();
    bool const has_before_flash = [&project] {
        for (auto const &checkpoint : project.checkpoints()) {
            if (checkpoint.id == "before-flash") {
                return true;
            }
        }
        return false;
    }();

    // Flash-ready checksum state, computed once and reused by the summary
    // tally and the dedicated row below.
    auto const &working_bytes = project.working_rom().data();
    auto const checksum = st::tune_export::checksum_state(
        std::span<std::uint8_t const>{working_bytes.data(), working_bytes.size()});
    bool const saved_state_current = !state.dirty && state.persisted_state_verified;

    ImGui::TextUnformatted("Project Readiness");
    text_subtle("A local project can be coherent before an ECU is eligible.");

    // Verification summary — an at-a-glance tally across the individual checks
    // below, so the panel reads as a report rather than a single badge. Each
    // non-passing check keeps its own row + remediation action underneath.
    {
        int verified = 0;
        int attention = 0;
        auto const tally = [&](bool ok) { ok ? ++verified : ++attention; };
        tally(source_shape_ok && working_shape_ok);
        tally(definition_ok);
        tally(source_match.has_value());
        tally(saved_state_current);
        tally(checksum != st::tune_export::ChecksumState::NeedsRepair);
        tally(has_checkpoint && !has_checkpoint_warning);
        if (attention == 0) {
            readiness_row("Verification", "All local checks pass", chip_fg_ok(), chip_bg_ok());
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d need attention", attention);
            readiness_row("Verification", buf, chip_fg_warn(), chip_bg_warn());
        }
        text_subtle("%d of %d local checks verified. Each item below carries its "
                    "own status and, where fixable in-app, an action.",
                    verified, verified + attention);
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    ImGui::Text("%s", project.display_name().c_str());
    text_subtle("Pack: %s  |  Policy: %s", pack.id.c_str(),
                std::string{st::policy::profile_name(project.policy_profile())}.c_str());
    ImGui::Separator();

    ImGui::TextUnformatted("Project truth");
    readiness_row("ROM shape", source_shape_ok && working_shape_ok ? "Verified" : "Needs attention",
                  source_shape_ok && working_shape_ok ? chip_fg_ok() : chip_fg_warn(),
                  source_shape_ok && working_shape_ok ? chip_bg_ok() : chip_bg_warn());
    text_subtle("Source: %zu bytes  |  Working: %zu bytes  |  Pack expects: %zu bytes",
                project.source_rom().size(), project.working_rom().size(), pack.rom_size_bytes);
    readiness_row("Definition", definition_ok ? "Lint passed" : "Needs attention",
                  definition_ok ? chip_fg_ok() : chip_fg_warn(),
                  definition_ok ? chip_bg_ok() : chip_bg_warn());
    readiness_row("Source CID", source_match.has_value() ? "Matched" : "No exact match",
                  source_match.has_value() ? chip_fg_ok() : chip_fg_warn(),
                  source_match.has_value() ? chip_bg_ok() : chip_bg_warn());
    if (source_match.has_value()) {
        text_subtle("%s  |  approved calibration regions: %zu  |  candidates excluded: %zu",
                    source_match->cid.c_str(), approved_calibration_regions,
                    candidate_calibration_regions);
    } else {
        text_subtle("No exact source-ROM CID match means no calibration allow-list can be selected.");
    }
    readiness_row("Checksum strategy", pack.checksum_type.empty() ? "Not declared" : "Declared",
                  pack.checksum_type.empty() ? chip_fg_warn() : chip_fg_info(),
                  pack.checksum_type.empty() ? chip_bg_warn() : chip_bg_info());
    if (!pack.checksum_type.empty()) {
        text_subtle("Declared family strategy: %s. Byte validation remains a separate gate.",
                    pack.checksum_type.c_str());
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::TextUnformatted("Plan truth");
    readiness_row("Changes", has_changes ? "Review required" : "No changes",
                  has_changes ? chip_fg_caution() : chip_fg_ok(),
                  has_changes ? chip_bg_caution() : chip_bg_ok());
    text_subtle("Source CRC32: %08X  |  Working CRC32: %08X", summary.source_crc,
                summary.working_crc);
    if (has_changes) {
        text_subtle("%zu changed bytes across %zu sectors.", summary.changed_bytes,
                    summary.changed_sectors);
    }
    readiness_row("Saved-state verification",
                  state.dirty ? "Pending save"
                              : (saved_state_current ? "Reopened and matched" : "Not verified"),
                  saved_state_current ? chip_fg_ok() : chip_fg_warn(),
                  saved_state_current ? chip_bg_ok() : chip_bg_warn());
    text_subtle(saved_state_current
                    ? "ROM bytes, edit histories, metadata, and additional slots match after reopen."
                    : "Save the project to run a full read-after-write verification.");
    if (state.dirty) {
        if (ImGui::SmallButton("Save project##readiness_save")) {
            save_project(state);
        }
    }

    // Flash-ready checksum — deliberately distinct from the project-integrity
    // row above. Project integrity asks "is the saved project self-consistent?";
    // this asks "does the working ROM satisfy the ECU's brick-protection sum so
    // a flash won't be rejected (NRC 0x22) or boot into a corrupt app?". A
    // project can be perfectly coherent and still not be flash-ready.
    {
        auto const cksum = checksum;
        switch (cksum) {
        case st::tune_export::ChecksumState::FlashReady:
            readiness_row("Flash-ready checksum", "Balanced (0x5AA5)", chip_fg_ok(),
                          chip_bg_ok());
            text_subtle("The 2 MB app window sums to the ECU's brick-protection "
                        "target. This is separate from project integrity above.");
            break;
        case st::tune_export::ChecksumState::NeedsRepair:
            readiness_row("Flash-ready checksum", "Needs repair", chip_fg_warn(),
                          chip_bg_warn());
            text_subtle("A calibration edit moved the sum off 0x5AA5. The balance "
                        "cell must be re-tuned before flashing — the project can "
                        "still be internally consistent.");
            if (ImGui::SmallButton("Repair balance##readiness_repair")) {
                // Re-tune the 2-byte balance word as one undoable edit. Copy
                // the working bytes, repair the copy, then write only the two
                // changed bytes back through the ROM so the History stays the
                // source of truth for undo.
                std::vector<std::uint8_t> buf(working_bytes.begin(), working_bytes.end());
                if (st::tune_export::repair_balance(buf).has_value()) {
                    constexpr std::size_t b = st::tune_export::kBalanceOffset;
                    std::uint8_t const b0_before = working_bytes[b];
                    std::uint8_t const b1_before = working_bytes[b + 1];
                    if (project.working_rom().write_u8(b, buf[b]).has_value() &&
                        project.working_rom().write_u8(b + 1, buf[b + 1]).has_value()) {
                        // Record on the working-slot history to match the ROM
                        // just mutated (working_rom()); active_history() could
                        // point at an additional slot and desync undo.
                        project.history().record(st::edit::Edit::bytes(
                            {{b, b0_before, buf[b]}, {b + 1, b1_before, buf[b + 1]}},
                            "Repair checksum balance"));
                        state.dirty = true;
                        state.persisted_state_verified = false;
                    }
                }
            }
            break;
        case st::tune_export::ChecksumState::NotApplicable:
            readiness_row("Flash-ready checksum", "Not a 2 MB SH-2A image", chip_fg_info(),
                          chip_bg_info());
            text_subtle("The 0x5AA5 balance check applies to 2 MB LF79xxxP ROMs. "
                        "This image uses a different (or no) checksum contract.");
            break;
        }
    }

    readiness_row("Offline checkpoint", has_checkpoint ? "Present" : "Missing",
                  has_checkpoint ? chip_fg_ok() : chip_fg_warn(),
                  has_checkpoint ? chip_bg_ok() : chip_bg_warn());
    if (has_checkpoint) {
        text_subtle("%zu immutable snapshot%s available for offline comparison.",
                    project.checkpoints().size(), project.checkpoints().size() == 1 ? "" : "s");
    } else {
        text_subtle("Create a named snapshot before making the next group of edits.");
    }
    readiness_row("Recovery evidence", "Confirm before flash", chip_fg_info(), chip_bg_info());
    text_subtle("A project checkpoint is not an ECU backup. Attest an exact-CID recovery image before any write.");
    if (has_checkpoint_warning) {
        readiness_row("Checkpoint metadata", "Needs review", chip_fg_warn(), chip_bg_warn());
        text_subtle("One or more checkpoint files did not match their recorded metadata.");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::TextUnformatted("ECU truth");
    readiness_row("Live identity", "Not available in GUI", chip_fg_info(), chip_bg_info());
    text_subtle("The GUI has no live transport binding in this build; a loaded ROM does not identify the ECU.");
    readiness_row("Flash state", "Hardware proof required", chip_fg_warn(), chip_bg_warn());
    text_subtle("Silent CAN/UDS failure requires exact external recovery; repeated blind OBD writes are blocked.");

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    render_guided_tasks(state, project, has_checkpoint, has_changes);

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::Separator();
    ImGui::TextUnformatted("Recommended next action");
    bool review_clicked = false;
    action_button(state, has_changes ? "Review changes" : "Open Map Explorer",
                  has_changes ? "Open the Compare workspace to inspect the semantic delta."
                              : "Use the Tables sidebar to browse the pack by category and purpose.",
                  true, review_clicked);
    if (review_clicked && has_changes) {
        state.show_compare_panel = true;
    }
    ImGui::SameLine();
    bool inspect_clicked = false;
    action_button(state, "Open Flash Review",
                  "Review the current flash plan. No bytes are sent by the GUI.",
                  has_changes, inspect_clicked);
    if (inspect_clicked) {
        state.show_flash_modal = true;
    }
    ImGui::SameLine();
    bool checkpoint_clicked = false;
    action_button(state, "Create Before-Flash checkpoint",
                  "Save the current working ROM as an immutable offline review point.",
                  !has_before_flash, checkpoint_clicked);
    if (checkpoint_clicked) {
        auto result = project.create_checkpoint("before-flash", "Before Flash",
                                                "Created from Project Readiness before a flash review.");
        if (result.has_value()) {
            state.status_msg = "Before-Flash checkpoint created.";
        } else {
            state.status_msg = "Checkpoint failed: " + result.error().to_string();
        }
    }

    ImGui::End();
}

} // namespace st::ui
