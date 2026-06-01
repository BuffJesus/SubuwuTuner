// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// CSV-import preview modal — GUI equivalent of `project-edit-csv --dry-run`.
// Renders the parsed cell list as a before→after diff, color-coded by
// direction, and only commits the edit on explicit user confirmation.

#include "modals/modals.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace st::ui {

void render_csv_import_modal(AppState &state) {
    if (state.show_csv_import_modal) {
        ImGui::OpenPopup("\xEE\x84\x9B  Import CSV##csv_import_modal");
        state.show_csv_import_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 320.0f), ImVec2(900.0f, 700.0f));
    if (!ImGui::BeginPopupModal("\xEE\x84\x9B  Import CSV##csv_import_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto const &parsed = state.csv_import_parsed;
    auto const &before_td = state.csv_import_before_values;

    ImGui::Text("Importing: %s", state.csv_import_source_path.filename().string().c_str());
    ImGui::Text("Target:    %s", state.csv_import_table_id.c_str());
    ImGui::Text("Cells:     %zu", parsed.cells.size());

    if (!parsed.cells.empty()) {
        std::size_t r_min = parsed.cells[0].row, r_max = parsed.cells[0].row;
        std::size_t c_min = parsed.cells[0].col, c_max = parsed.cells[0].col;
        for (auto const &e : parsed.cells) {
            r_min = std::min(r_min, e.row);
            r_max = std::max(r_max, e.row);
            c_min = std::min(c_min, e.col);
            c_max = std::max(c_max, e.col);
        }
        ImGui::Text("Bounds:    rows %zu..%zu, cols %zu..%zu", r_min, r_max, c_min, c_max);
    }

    // Warnings (yellow chip). pack_id mismatch is the common one.
    if (!parsed.warnings.empty()) {
        ImGui::Spacing();
        for (auto const &w : parsed.warnings) {
            ImGui::TextColored(chip_fg_warn(), "warning: %s", w.message.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Before/after preview. Pull precision from the table's scaling so
    // the diff reads consistently with the grid view.
    auto const *table = state.project.has_value()
                            ? state.project->definition().find_table(state.csv_import_table_id)
                            : nullptr;
    auto const *scaling = (table != nullptr && state.project.has_value())
                              ? state.project->definition().find_scaling(table->scaling)
                              : nullptr;
    int const prec = scaling != nullptr ? scaling->precision : 4;

    constexpr std::size_t kPreviewLimit = 12;
    std::size_t const shown = std::min(kPreviewLimit, parsed.cells.size());
    text_subtle("Preview (first %zu of %zu):", shown, parsed.cells.size());
    if (ImGui::BeginTable("##csv_preview", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("row", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("col", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("before", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("after", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < shown; ++i) {
            auto const &e = parsed.cells[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", e.row);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", e.col);

            ImGui::TableSetColumnIndex(2);
            if (before_td.has_value() && e.row < before_td->values.size() &&
                e.col < before_td->values[e.row].size()) {
                double const b = before_td->values[e.row][e.col];
                ImGui::Text("%.*f", prec, b);
            } else {
                ImGui::TextDisabled("?");
            }

            ImGui::TableSetColumnIndex(3);
            // Color-code: green when value increases, red when decreases.
            // Tuner shorthand — "raising" vs "pulling" — keep neutral
            // when unchanged or the before value is unknown.
            // Tolerance = half a display-precision step so a value that
            // round-trips through the CSV (`%.*f` write → strtod read)
            // doesn't get flagged as a "decrease" by 1e-15 of FP noise.
            ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if (before_td.has_value() && e.row < before_td->values.size() &&
                e.col < before_td->values[e.row].size()) {
                double const b = before_td->values[e.row][e.col];
                double const eps = 0.5 * std::pow(10.0, -prec);
                if (e.value > b + eps)
                    color = chip_fg_ok();
                else if (e.value < b - eps)
                    color = chip_fg_danger();
            }
            ImGui::TextColored(color, "%.*f", prec, e.value);
        }
        ImGui::EndTable();
    }
    if (parsed.cells.size() > shown) {
        text_subtle("... %zu more not shown", parsed.cells.size() - shown);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool const want_apply = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
    bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);

    constexpr float kBtnW = 160.0f;
    push_primary_button_colors();
    bool const apply_clicked = ImGui::Button("Apply edits", ImVec2(kBtnW, 0.0f));
    pop_primary_button_colors();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Apply the CSV as a single bulk edit. "
                          "Undoable via Ctrl+Z.  (Enter)");
    }
    ImGui::SameLine();
    bool const cancel_clicked = ImGui::Button("Cancel", ImVec2(kBtnW * 0.7f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Discard the preview. Nothing is written. "
                          "(Esc)");
    }

    // Inline error from a prior failed apply. Surfaces between the
    // preview and the buttons so the user sees the cause and can fix
    // + retry without losing the parsed edits. We do NOT close the
    // popup on apply failure — only on success or explicit cancel.
    if (!state.csv_import_apply_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "Apply failed:  %s",
                           state.csv_import_apply_error.c_str());
        text_subtle("The preview is preserved — fix the underlying issue and "
                    "click Apply again, or Cancel to discard.");
    }

    if (apply_clicked || want_apply) {
        if (auto err =
                apply_parsed_csv_edits(state, state.csv_import_table_id, state.csv_import_parsed);
            err.has_value()) {
            // Stay in the modal; show the error inline. Don't touch
            // status_msg — failure feedback belongs in the modal.
            state.csv_import_apply_error = *err;
        } else {
            state.csv_import_parsed = {};
            state.csv_import_before_values.reset();
            state.csv_import_table_id.clear();
            state.csv_import_source_path.clear();
            state.csv_import_apply_error.clear();
            ImGui::CloseCurrentPopup();
        }
    } else if (cancel_clicked || want_cancel) {
        state.csv_import_parsed = {};
        state.csv_import_before_values.reset();
        state.csv_import_table_id.clear();
        state.csv_import_source_path.clear();
        state.csv_import_apply_error.clear();
        state.status_msg = "Import cancelled.";
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace st::ui
