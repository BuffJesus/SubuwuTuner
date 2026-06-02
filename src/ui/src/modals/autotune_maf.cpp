// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// MAF auto-tune modal — GUI parity with `subuwutuner-cli
// project-autotune-maf`: pick a target 1D table + log CSV + tuning
// params, Preview to see the proposed per-cell deltas + lint findings,
// Apply to commit as a single edit::History entry. The run_/apply_
// helpers are file-local — no cross-file callers.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/autotune.hpp"
#include "st/edit.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace st::ui {
namespace {

// Runs the MAF auto-tune pipeline against the staged log path + tuning
// options and stashes the result + lints in AppState for the modal
// preview. Returns nullopt on success or an error message.
std::optional<std::string> run_maf_autotune_preview(AppState &state) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    std::string const target_table_id{state.maf_at_table_id};
    if (target_table_id.empty()) {
        return std::string{"Pick a target table."};
    }
    std::filesystem::path const log_path{state.maf_at_log_path};
    if (log_path.empty()) {
        return std::string{"Pick a CSV log."};
    }

    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    if (table->dimensions != 1) {
        return "Table '" + target_table_id +
               "' has dimensions=" + std::to_string(table->dimensions) +
               "; MAF scaling must be 1D.";
    }
    if (!table->axis_x.has_value() || table->axis_x->empty()) {
        return "Table '" + target_table_id + "' has no axis_x.";
    }
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!td.has_value()) {
        return "read table: " + td.error().to_string();
    }
    if (td->values.empty() || td->values[0].empty()) {
        return "Table is empty.";
    }
    auto const &axis_values = td->axis_x;
    std::vector<double> current(td->values[0].begin(), td->values[0].end());
    if (axis_values.size() != current.size()) {
        return "Axis length doesn't match current row length.";
    }

    // Slurp the log.
    std::ifstream in{log_path, std::ios::binary};
    if (!in) {
        return "cannot open log: " + log_path.string();
    }
    std::stringstream buf;
    buf << in.rdbuf();
    auto samples = st::autotune::read_maf_samples_csv(buf.str());
    if (!samples.has_value()) {
        return "log parse: " + samples.error().to_string();
    }

    st::autotune::MafTuneOptions opts;
    opts.gain = static_cast<double>(state.maf_at_gain);
    opts.max_delta_pct = static_cast<double>(state.maf_at_max_delta_pct);
    opts.min_samples_per_cell = static_cast<std::size_t>(std::max(0, state.maf_at_min_samples));
    opts.require_open_loop = state.maf_at_require_open_loop;

    auto result = st::autotune::tune_maf(axis_values, current, *samples, opts);
    if (!result.has_value()) {
        return "tune_maf: " + result.error().to_string();
    }
    if (state.maf_at_apply_smooth) {
        *result = st::autotune::smooth_proposals(*result, opts.max_delta_pct);
    }
    auto lints = st::autotune::lint_maf_proposal(axis_values, current, *result);

    // Stash for the modal preview + Apply.
    state.maf_at_result = std::move(*result);
    state.maf_at_lints = std::move(lints);
    state.maf_at_table_data = std::move(*td);
    return std::nullopt;
}

// Commits the cached MAF auto-tune proposal as a single edit::History
// entry on the target table. Mirrors the CLI's --apply branch.
std::optional<std::string> apply_maf_autotune_proposal(AppState &state) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    if (!state.maf_at_result.has_value() || !state.maf_at_table_data.has_value()) {
        return std::string{"Run Preview first."};
    }
    std::string const target_table_id{state.maf_at_table_id};
    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    auto td = *state.maf_at_table_data;
    auto const &result = *state.maf_at_result;

    if (result.cells.empty()) {
        // Defensive: an empty proposal would underflow `size() - 1` on
        // the rect below. Upstream guards prevent this in practice; a
        // future refactor of `tune_maf` might not.
        return std::string{"Empty proposal — nothing to apply."};
    }
    st::edit::Rect const rect{0, 0, 0, result.cells.size() - 1};
    auto before = st::edit::snapshot(td, rect);
    if (!before.has_value()) {
        return "snapshot before: " + before.error().to_string();
    }
    std::size_t modified = 0;
    for (auto const &c : result.cells) {
        if (c.proposed_value != c.current_value)
            ++modified;
        td.values[0][c.cell_index] = c.proposed_value;
    }
    auto after = st::edit::snapshot(td, rect);
    if (!after.has_value()) {
        return "snapshot after: " + after.error().to_string();
    }
    if (auto wb = state.project->definition().write_table_values(state.project->working_rom(),
                                                                 *table, td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[64];
    std::snprintf(descbuf, sizeof descbuf, "autotune maf (%zu cell%s)", modified,
                  modified == 1 ? "" : "s");
    state.project->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                          std::move(*after), std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(td);
    }
    state.dirty = true;
    state.status_msg = "Autotune MAF applied: " + std::to_string(modified) + " cell" +
                       (modified == 1 ? "" : "s") + " changed on " + table->id + ".";
    if (state.audit_log.has_value()) {
        (void)state.audit_log->log(
            st::audit::EntryKind::AutotuneCommitted, "autotune.maf",
            std::string{descbuf},
            {{"table", table->id},
             {"cells_modified", std::to_string(modified)},
             {"log_path", state.maf_at_log_path}});
    }
    return std::nullopt;
}

} // namespace

void render_maf_autotune_modal(AppState &state) {
    if (state.show_maf_autotune_modal) {
        ImGui::OpenPopup("\xEE\xA5\x90  Autotune MAF##maf_autotune_modal");
        state.show_maf_autotune_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 380.0f), ImVec2(1100.0f, 800.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA5\x90  Autotune MAF##maf_autotune_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // ---- Inputs ------------------------------------------------------
    ImGui::TextUnformatted("Target table");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##maf_at_table", "snake_case id of the 1D MAF scaling table",
                             state.maf_at_table_id, sizeof state.maf_at_table_id);

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextUnformatted("Log CSV");
    {
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w - 8.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText("##maf_at_log", state.maf_at_log_path, sizeof state.maf_at_log_path,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Browse…##maf_at_log")) {
            nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
            if (r == NFD_OKAY) {
                std::snprintf(state.maf_at_log_path, sizeof state.maf_at_log_path, "%s", out.get());
                // Path changed — drop any stale preview.
                state.maf_at_result.reset();
                state.maf_at_lints.clear();
                state.maf_at_table_data.reset();
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::SeparatorText("Tuning");

    ImGui::PushItemWidth(180.0f);
    ImGui::SliderFloat("gain", &state.maf_at_gain, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fraction of the observed error to apply per\n"
                          "pass. 0.5 is a safe default; 1.0 commits the\n"
                          "full observed delta. Lower = more passes,\n"
                          "less risk per pass.");
    }
    ImGui::SliderFloat("max delta", &state.maf_at_max_delta_pct, 0.0f, 0.50f, "±%.0f%%",
                       ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Per-cell clamp on the proposed change. Caps\n"
                          "runaway moves on noisy or outlier-rich logs.");
    }
    ImGui::DragInt("min samples / cell", &state.maf_at_min_samples, 1.0f, 1, 10000);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cells with fewer than this many samples after\n"
                          "data-quality gates stay at their current value\n"
                          "and report with confidence = 0.");
    }
    ImGui::PopItemWidth();

    ImGui::Checkbox("smooth pass (neighbor weight 0.25)", &state.maf_at_apply_smooth);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Confidence-weighted neighbor smoothing per\n"
                          "docs/12 §MAF auto-tune. Disabled = raw per-cell\n"
                          "proposals; smooth re-clamps to ±max-delta.");
    }
    ImGui::Checkbox("require open-loop fueling", &state.maf_at_require_open_loop);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drop samples taken in closed-loop. Useful if\n"
                          "your log straddles both modes and you want to\n"
                          "tune from WOT pulls only.");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    ImGui::Spacing();

    bool const have_inputs = state.maf_at_table_id[0] != '\0' && state.maf_at_log_path[0] != '\0';
    bool run_clicked = false;
    {
        ImGui::BeginDisabled(!have_inputs);
        run_clicked = ImGui::Button("Run preview", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!have_inputs) {
                ImGui::SetTooltip("Pick a target table id and a log CSV first.");
            } else {
                ImGui::SetTooltip("Parse the log + run tune_maf + smooth + "
                                  "lint.  Nothing written yet.");
            }
        }
    }
    if (run_clicked) {
        if (auto err = run_maf_autotune_preview(state); err.has_value()) {
            state.maf_at_status_msg = *err;
            state.maf_at_result.reset();
            state.maf_at_lints.clear();
            state.maf_at_table_data.reset();
        } else {
            state.maf_at_status_msg.clear();
        }
    }

    if (!state.maf_at_status_msg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "Preview failed: %s",
                           state.maf_at_status_msg.c_str());
    }

    // ---- Result -----------------------------------------------------
    if (state.maf_at_result.has_value()) {
        auto const &result = *state.maf_at_result;
        ImGui::Spacing();
        ImGui::SeparatorText("Preview");
        ImGui::Text("Samples: %zu (after gates: %zu)", result.total_samples,
                    result.samples_after_gates);

        std::size_t modified = 0;
        std::size_t underpowered = 0;
        for (auto const &c : result.cells) {
            if (c.confidence == 0.0)
                ++underpowered;
            else if (c.proposed_value != c.current_value)
                ++modified;
        }
        ImGui::Text("Modified: %zu / %zu cells, underpowered: %zu", modified, result.cells.size(),
                    underpowered);

        // Per-cell ledger.
        if (ImGui::BeginTable("##maf_at_ledger", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 240.0f))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
            ImGui::TableSetupColumn("axis", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("current", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("proposed", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("samples", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("conf", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            auto const &axis = state.maf_at_table_data->axis_x;
            for (auto const &c : result.cells) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", c.cell_index);
                ImGui::TableSetColumnIndex(1);
                if (c.cell_index < axis.size()) {
                    ImGui::Text("%.3f", axis[c.cell_index]);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.4f", c.current_value);

                ImGui::TableSetColumnIndex(3);
                ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                if (c.confidence == 0.0) {
                    color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                } else if (c.proposed_value > c.current_value) {
                    color = chip_fg_ok();
                } else if (c.proposed_value < c.current_value) {
                    color = chip_fg_danger();
                }
                ImGui::TextColored(color, "%.4f", c.proposed_value);

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%zu", c.samples_used);
                ImGui::TableSetColumnIndex(5);
                if (c.confidence == 0.0) {
                    ImGui::TextDisabled("-");
                } else {
                    ImGui::Text("%.2f", c.confidence);
                }
            }
            ImGui::EndTable();
        }

        // Lint findings, if any.
        if (!state.maf_at_lints.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(chip_fg_warn(),
                               "Lint findings (%zu):", state.maf_at_lints.size());
            for (auto const &v : state.maf_at_lints) {
                ImGui::BulletText("cells %zu..%zu — %s\n    %s", v.cell_index, v.cell_index + 1,
                                  pretty_lint_kind(v.kind), v.message.c_str());
            }
        }
    }

    // ---- Footer ------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool const have_preview = state.maf_at_result.has_value();
    bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    bool const want_apply = have_preview &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false));
    bool apply_clicked = false;
    {
        push_primary_button_colors();
        ImGui::BeginDisabled(!have_preview);
        apply_clicked = ImGui::Button("Apply proposal", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        pop_primary_button_colors();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!have_preview) {
                ImGui::SetTooltip("Run Preview first.");
            } else {
                ImGui::SetTooltip("Commit the proposal as a single undoable\n"
                                  "edit on the target table.  (Enter)");
            }
        }
    }
    ImGui::SameLine();
    bool const cancel_clicked = ImGui::Button("Close", ImVec2(110.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Close without applying.  (Esc)");
    }

    auto const reset_modal_state = [&]() {
        state.maf_at_result.reset();
        state.maf_at_lints.clear();
        state.maf_at_table_data.reset();
        state.maf_at_status_msg.clear();
    };

    if ((apply_clicked && have_preview) || want_apply) {
        if (auto err = apply_maf_autotune_proposal(state); err.has_value()) {
            state.maf_at_status_msg = *err;
        } else {
            reset_modal_state();
            ImGui::CloseCurrentPopup();
        }
    } else if (cancel_clicked || want_cancel) {
        reset_modal_state();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace st::ui
