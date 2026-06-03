// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Knock-pull auto-tune modal — GUI parity with `subuwutuner-cli
// project-autotune-knock-pull`: pick a target 2D table, pick a knock
// CSV, configure trigger / step / min-samples (+ optional add-back),
// Preview to see the proposed delta grid + lints, Apply to commit as a
// single edit::History entry. The run_/apply_ helpers are file-local.

#include "modals/modals.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/autotune.hpp"
#include "st/edit.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
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

// Runs the knock-pull pipeline against the staged table + log + tuning
// options and stashes the result + lints in AppState for the modal
// preview. Mirrors cmd_project_autotune_knock_pull's preview branch.
std::optional<std::string> run_knock_pull_preview(AppState &state) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    std::string const target_table_id{state.kp_at_table_id};
    if (target_table_id.empty()) {
        return std::string{"Pick a target table."};
    }
    std::filesystem::path const log_path{state.kp_at_log_path};
    if (log_path.empty()) {
        return std::string{"Pick a CSV log."};
    }

    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    if (table->dimensions != 2) {
        return "Table '" + target_table_id +
               "' has dimensions=" + std::to_string(table->dimensions) +
               "; knock-pull needs a 2D timing table.";
    }
    // Preview reads from the active slot (Issue #10 phase 3).
    auto const *read_rom = state.view_rom();
    if (read_rom == nullptr) {
        return std::string{"No active ROM to read."};
    }
    auto td = state.project->definition().read_table_values(*read_rom, *table);
    if (!td.has_value()) {
        return "read table: " + td.error().to_string();
    }

    // Map (axis_x, axis_y) onto (rpm_axis, load_axis). Combo: 0=y, 1=x.
    bool const rpm_is_y = (state.kp_at_rpm_axis_kind == 0);
    auto const &rpm_axis = rpm_is_y ? td->axis_y : td->axis_x;
    auto const &load_axis = rpm_is_y ? td->axis_x : td->axis_y;
    if (rpm_axis.empty() || load_axis.empty()) {
        return std::string{"Table axes are empty."};
    }
    std::vector<double> current_timing;
    current_timing.reserve(load_axis.size() * rpm_axis.size());
    for (std::size_t li = 0; li < load_axis.size(); ++li) {
        for (std::size_t ri = 0; ri < rpm_axis.size(); ++ri) {
            std::size_t const td_r = rpm_is_y ? ri : li;
            std::size_t const td_c = rpm_is_y ? li : ri;
            current_timing.push_back(td->values[td_r][td_c]);
        }
    }

    std::ifstream in{log_path, std::ios::binary};
    if (!in) {
        return "cannot open log: " + log_path.string();
    }
    std::stringstream buf;
    buf << in.rdbuf();
    auto samples = st::autotune::read_knock_samples_csv(buf.str());
    if (!samples.has_value()) {
        return "log parse: " + samples.error().to_string();
    }

    st::autotune::KnockPullOptions opts;
    opts.trigger_degrees = static_cast<double>(state.kp_at_trigger_degrees);
    opts.pull_step_degrees = static_cast<double>(state.kp_at_pull_step_degrees);
    opts.min_samples_per_cell = static_cast<std::size_t>(std::max(0, state.kp_at_min_samples));

    auto result =
        st::autotune::tune_knock_pull(rpm_axis, load_axis, current_timing, *samples, opts);
    if (!result.has_value()) {
        return "tune_knock_pull: " + result.error().to_string();
    }
    if (state.kp_at_enable_add_back) {
        st::autotune::KnockAddBackOptions abo;
        abo.enabled = true;
        abo.add_step_degrees = static_cast<double>(state.kp_at_add_step_degrees);
        abo.min_clean_samples_per_cell =
            static_cast<std::size_t>(std::max(0, state.kp_at_add_back_min_clean));
        abo.clean_threshold_degrees = static_cast<double>(state.kp_at_clean_threshold);
        *result = st::autotune::apply_knock_add_back(*result, abo);
    }
    auto lints = st::autotune::lint_knock_proposal(rpm_axis, load_axis, *result);

    state.kp_at_result = std::move(*result);
    state.kp_at_lints = std::move(lints);
    state.kp_at_table_data = std::move(*td);
    state.kp_at_rpm_axis_values.assign(rpm_axis.begin(), rpm_axis.end());
    state.kp_at_load_axis_values.assign(load_axis.begin(), load_axis.end());
    return std::nullopt;
}

// Commits the cached knock-pull proposal as a single edit::History
// entry on the target table. Mirrors the CLI's --apply branch — the
// proposal grid is (load × rpm) in kernel-row-major; td->values is the
// pack's native (axis_y × axis_x) layout, un-transpose accordingly.
std::optional<std::string> apply_knock_pull_proposal(AppState &state) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    if (!state.kp_at_result.has_value() || !state.kp_at_table_data.has_value()) {
        return std::string{"Run Preview first."};
    }
    std::string const target_table_id{state.kp_at_table_id};
    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    auto td = *state.kp_at_table_data;
    auto const &result = *state.kp_at_result;
    if (result.cells.empty()) {
        return std::string{"Empty proposal — nothing to apply."};
    }

    bool const rpm_is_y = (state.kp_at_rpm_axis_kind == 0);
    std::size_t const grid_rows = td.values.size();
    std::size_t const grid_cols = grid_rows > 0 ? td.values[0].size() : 0;
    if (grid_rows == 0 || grid_cols == 0) {
        return std::string{"Table is empty."};
    }
    st::edit::Rect const rect{0, grid_rows - 1, 0, grid_cols - 1};
    auto before = st::edit::snapshot(td, rect);
    if (!before.has_value()) {
        return "snapshot before: " + before.error().to_string();
    }
    std::size_t pulled = 0;
    std::size_t added = 0;
    for (std::size_t li = 0; li < result.rows; ++li) {
        for (std::size_t ri = 0; ri < result.cols; ++ri) {
            std::size_t const td_r = rpm_is_y ? ri : li;
            std::size_t const td_c = rpm_is_y ? li : ri;
            auto const &cell = result.cells[li * result.cols + ri];
            td.values[td_r][td_c] = cell.proposed_value;
            if (cell.pulled)
                ++pulled;
            else if (cell.proposed_value > cell.current_value)
                ++added;
        }
    }
    auto after = st::edit::snapshot(td, rect);
    if (!after.has_value()) {
        return "snapshot after: " + after.error().to_string();
    }
    st::Rom *target_rom = state.project->active_rom_mut();
    if (target_rom == nullptr) {
        return std::string{"Active ROM is read-only — switch View → Active ROM "
                           "to an editable slot."};
    }
    if (auto wb = state.project->definition().write_table_values(*target_rom, *table, td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[80];
    std::snprintf(descbuf, sizeof descbuf, "autotune knock-pull (%zu pulled%s%s)", pulled,
                  added > 0 ? ", " : "",
                  added > 0 ? (std::to_string(added) + " added-back").c_str() : "");
    state.project->active_history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                                 std::move(*after),
                                                                 std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(td);
    }
    state.dirty = true;
    state.status_msg = "Autotune knock-pull applied: " + std::to_string(pulled) + " pulled" +
                       (added > 0 ? (", " + std::to_string(added) + " added-back") : "") + " on " +
                       table->id + ".";
    if (state.audit_log.has_value()) {
        std::vector<std::pair<std::string, std::string>> fields{
            {"table", table->id},
            {"pulled", std::to_string(pulled)},
            {"added_back", std::to_string(added)},
            {"log_path", state.kp_at_log_path}};
        // Active-rom-aware audit (Issue #10 sweep): autotune commits
        // against the active editable slot; capture which one so the
        // audit timeline doesn't blur multi-ROM workflows.
        if (!state.active_rom_id.empty() && state.active_rom_id != "working") {
            fields.emplace_back("rom", state.active_rom_id);
        }
        (void)state.audit_log->log(
            st::audit::EntryKind::AutotuneCommitted, "autotune.knock",
            std::string{descbuf}, std::move(fields));
    }
    return std::nullopt;
}

} // namespace

void render_kp_autotune_modal(AppState &state) {
    if (state.show_kp_autotune_modal) {
        ImGui::OpenPopup("\xEE\xA5\x90  Autotune knock pull##kp_autotune_modal");
        state.show_kp_autotune_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(680.0f, 420.0f), ImVec2(1200.0f, 900.0f));
    if (!ImGui::BeginPopupModal("\xEE\xA5\x90  Autotune knock pull##kp_autotune_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // ---- Inputs ------------------------------------------------------
    ImGui::TextUnformatted("Target table");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##kp_at_table", "snake_case id of the 2D ignition timing table",
                             state.kp_at_table_id, sizeof state.kp_at_table_id);

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextUnformatted("Log CSV");
    {
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w - 8.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText("##kp_at_log", state.kp_at_log_path, sizeof state.kp_at_log_path,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Browse…##kp_at_log")) {
            nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
            if (r == NFD_OKAY) {
                std::snprintf(state.kp_at_log_path, sizeof state.kp_at_log_path, "%s", out.get());
                state.kp_at_result.reset();
                state.kp_at_lints.clear();
                state.kp_at_table_data.reset();
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextUnformatted("RPM axis");
    ImGui::SetNextItemWidth(180.0f);
    char const *const rpm_items[] = {"y (default Subaru)", "x"};
    ImGui::Combo("##kp_at_rpm_axis", &state.kp_at_rpm_axis_kind, rpm_items, 2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Which of the pack's two axes is engine speed.\n"
                          "Default 'y' matches the common Subaru convention\n"
                          "(axis_x = load, axis_y = RPM). Flip when the pack\n"
                          "uses the opposite orientation.");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::SeparatorText("Tuning");

    ImGui::PushItemWidth(180.0f);
    ImGui::SliderFloat("trigger degrees", &state.kp_at_trigger_degrees, 0.1f, 5.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A cell fires a pull when its mean Feedback Knock\n"
                          "Correction is below -trigger (more negative = ECU\n"
                          "pulling harder). Default 1.5° per docs/12.");
    }
    ImGui::SliderFloat("pull step degrees", &state.kp_at_pull_step_degrees, 0.10f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Degrees subtracted from a triggered cell per pass.\n"
                          "Default 0.75°. Monotonic-subtract — this pass only\n"
                          "ever lowers timing.");
    }
    ImGui::DragInt("min samples / cell", &state.kp_at_min_samples, 1.0f, 1, 10000);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cells with fewer than this many gated samples\n"
                          "stay at their current value. Default 30.");
    }
    ImGui::PopItemWidth();

    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    ImGui::Checkbox("enable add-back pass (opt-in)", &state.kp_at_enable_add_back);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("docs/12 §Knock-based ignition pull add-back: raise\n"
                          "timing on cells whose mean knock stayed clean over\n"
                          "enough samples. Adding timing is the more dangerous\n"
                          "direction — off by default.");
    }
    if (state.kp_at_enable_add_back) {
        ImGui::Indent();
        ImGui::PushItemWidth(180.0f);
        ImGui::SliderFloat("add step degrees", &state.kp_at_add_step_degrees, 0.05f, 2.0f, "%.2f");
        ImGui::DragInt("min clean samples", &state.kp_at_add_back_min_clean, 1.0f, 1, 10000);
        ImGui::SliderFloat("clean threshold (°)", &state.kp_at_clean_threshold, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mean knock must be greater than -threshold for a\n"
                              "cell to count as clean. Default 0.05°.");
        }
        ImGui::PopItemWidth();
        ImGui::Unindent();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    ImGui::Spacing();

    bool const have_inputs = state.kp_at_table_id[0] != '\0' && state.kp_at_log_path[0] != '\0';
    bool run_clicked = false;
    {
        ImGui::BeginDisabled(!have_inputs);
        run_clicked = ImGui::Button("Run preview", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!have_inputs) {
                ImGui::SetTooltip("Pick a target table id and a log CSV first.");
            } else {
                ImGui::SetTooltip("Parse the log + run tune_knock_pull "
                                  "(+ add-back if enabled) + lint. Nothing "
                                  "written yet.");
            }
        }
    }
    if (run_clicked) {
        if (auto err = run_knock_pull_preview(state); err.has_value()) {
            state.kp_at_status_msg = *err;
            state.kp_at_result.reset();
            state.kp_at_lints.clear();
            state.kp_at_table_data.reset();
        } else {
            state.kp_at_status_msg.clear();
        }
    }

    if (!state.kp_at_status_msg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "Preview failed: %s",
                           state.kp_at_status_msg.c_str());
    }

    // ---- Result -----------------------------------------------------
    if (state.kp_at_result.has_value()) {
        auto const &result = *state.kp_at_result;
        ImGui::Spacing();
        ImGui::SeparatorText("Preview");
        ImGui::Text("Samples: %zu (after gates: %zu)", result.total_samples,
                    result.samples_after_gates);

        std::size_t pulled = 0;
        std::size_t added = 0;
        for (auto const &c : result.cells) {
            if (c.pulled)
                ++pulled;
            else if (c.proposed_value > c.current_value)
                ++added;
        }
        ImGui::Text("Grid: %zu rows × %zu cols (load × RPM) — pulled: %zu%s", result.rows,
                    result.cols, pulled,
                    state.kp_at_enable_add_back ? (", added-back: " + std::to_string(added)).c_str()
                                                : "");

        // 2D delta ledger. Columns: row-load label + one per RPM
        // breakpoint. Cells show the proposed - current delta; zero =
        // disabled-text dot, negative (pull) = red, positive (add) =
        // green.
        std::size_t const cols = result.cols;
        std::size_t const n_cols_total = cols + 1; // +1 for the label
        // ImGui::BeginTable caps at 64 columns; bail gracefully if the
        // RPM axis is somehow huge. Subaru factory tables max ~16.
        if (n_cols_total > 64) {
            text_subtle("(RPM axis has %zu cells; ledger requires "
                        "≤63. Use the CLI for this table.)",
                        cols);
        } else if (ImGui::BeginTable("##kp_at_ledger", static_cast<int>(n_cols_total),
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                         ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_ScrollX,
                                     ImVec2(0.0f, 260.0f))) {
            ImGui::TableSetupColumn("load\\rpm", ImGuiTableColumnFlags_WidthFixed, 88.0f);
            for (std::size_t c = 0; c < cols; ++c) {
                char buf[24];
                std::snprintf(
                    buf, sizeof buf, "%.0f",
                    c < state.kp_at_rpm_axis_values.size() ? state.kp_at_rpm_axis_values[c] : 0.0);
                ImGui::TableSetupColumn(buf, ImGuiTableColumnFlags_WidthFixed, 56.0f);
            }
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableHeadersRow();

            for (std::size_t r = 0; r < result.rows; ++r) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (r < state.kp_at_load_axis_values.size()) {
                    ImGui::Text("%.2f", state.kp_at_load_axis_values[r]);
                } else {
                    ImGui::TextDisabled("-");
                }
                for (std::size_t c = 0; c < cols; ++c) {
                    ImGui::TableSetColumnIndex(static_cast<int>(c + 1));
                    auto const &cell = result.cells[r * cols + c];
                    double const delta = cell.proposed_value - cell.current_value;
                    if (std::abs(delta) < 0.0001) {
                        ImGui::TextDisabled(".");
                    } else if (delta < 0.0) {
                        ImGui::TextColored(chip_fg_danger(), "%+.2f", delta);
                    } else {
                        ImGui::TextColored(chip_fg_ok(), "%+.2f", delta);
                    }
                }
            }
            ImGui::EndTable();
        }

        // Lint findings, if any.
        if (!state.kp_at_lints.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(chip_fg_warn(),
                               "Lint findings (%zu):", state.kp_at_lints.size());
            for (auto const &v : state.kp_at_lints) {
                ImGui::BulletText("cell %zu — %s\n    %s", v.cell_index, pretty_lint_kind(v.kind),
                                  v.message.c_str());
            }
        }
    }

    // ---- Footer ------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool const have_preview = state.kp_at_result.has_value();
    bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    // Enter triggers Apply only when a preview exists — matches the
    // visual disabled-button state so the keyboard shortcut doesn't
    // bypass the "preview first" guard.
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
        state.kp_at_result.reset();
        state.kp_at_lints.clear();
        state.kp_at_table_data.reset();
        state.kp_at_rpm_axis_values.clear();
        state.kp_at_load_axis_values.clear();
        state.kp_at_status_msg.clear();
    };

    if ((apply_clicked && have_preview) || want_apply) {
        if (auto err = apply_knock_pull_proposal(state); err.has_value()) {
            state.kp_at_status_msg = *err;
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
