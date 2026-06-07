// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Table view — the main editor surface. Owns the heatmap legend, the
// scrollable grid, the per-cell editor, the +5%/-5%/smooth/interpolate
// toolbar, the right-click context menu, and the keyboard navigation
// for selection. Companion helpers (GridStats, compute_stats,
// heatmap_color, text_right_aligned, render_table_heatmap,
// render_table_grid) stay file-local — only this panel calls them.

#include "panels/panels.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "project_io.hpp"
#include "theme.hpp"
#include "widgets/widgets.hpp"

#include "st/defs.hpp"
#include "st/edit.hpp"
#include "st/policy.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {
namespace {

struct GridStats {
    double min{0.0};
    double max{0.0};
    double mean{0.0};
    std::size_t count{0};
};

GridStats compute_stats(st::Definition::TableData const &td) {
    GridStats s;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    std::size_t n = 0;
    for (auto const &row : td.values) {
        for (auto const v : row) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
            ++n;
        }
    }
    if (n > 0) {
        s.min = lo;
        s.max = hi;
        s.mean = sum / static_cast<double>(n);
        s.count = n;
    }
    return s;
}

// Heatmap overlay: blue (cool / low) → transparent (mid) → orange (high).
// Tuned to be readable with bright text on the dark row backgrounds.
//
// Asymmetric alpha caps: orange-end at ~140/255 (warm, reads as
// shading), blue-end at ~70/255 (half that). The cold-end alpha was
// halved after user feedback that a lone min-value cell read as a
// selection highlight rather than as a heat-coded extreme — the
// selection color is also blue at ~55% alpha, so saturated blue on a
// single cell was confusable with "this cell is selected." Quieter
// blue keeps the min-end informative without the lookalike.
ImU32 heatmap_color(double v, double min_v, double max_v) {
    if (max_v <= min_v) {
        return 0; // flat table: skip shading
    }
    double t = (v - min_v) / (max_v - min_v);
    t = std::clamp(t, 0.0, 1.0);

    auto const lerp = [](double a, double b, double f) { return a + (b - a) * f; };
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
    if (t < 0.5) {
        double const s = t * 2.0;
        r = lerp(45.0, 20.0, s);
        g = lerp(80.0, 22.0, s);
        b = lerp(140.0, 26.0, s);
        a = lerp(70.0, 0.0, s); // cold end max ~28%, halved from prior 55%
    } else {
        double const s = (t - 0.5) * 2.0;
        r = lerp(20.0, 180.0, s);
        g = lerp(22.0, 90.0, s);
        b = lerp(26.0, 50.0, s);
        a = lerp(0.0, 140.0, s); // warm end unchanged
    }
    return IM_COL32(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b),
                    static_cast<int>(a));
}

// ImGui table cells default to left-aligned text. For numerical grids,
// right-alignment so decimal points line up across rows is more legible.
void text_right_aligned(char const *text) {
    float const w = ImGui::CalcTextSize(text).x;
    float const avail = ImGui::GetContentRegionAvail().x;
    if (w < avail) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    }
    ImGui::TextUnformatted(text);
}

// Renders the table's `td.values` grid as a heatmap. For 3D tables the
// caller is expected to pass a TableData whose `values` is the currently
// selected slice (built upstream in render_table_view); this function does
// not look at `td.slices`.
void render_table_heatmap(st::Definition::TableData const &td, st::Table const *tbl,
                          st::Scaling const *scal, GridStats const &stats) {
    if (td.values.empty() || td.values.front().empty()) {
        ImGui::TextDisabled("(no values)");
        return;
    }

    auto const rows = td.values.size();
    auto const cols = td.values.front().size();

    // Scalar (1×1) — an ImPlot heatmap of one cell with no axes carries
    // no information; render the value plainly and point at Grid view
    // for editing. Same fallback the grid path takes for dim=0 tables,
    // so toggling View modes on a scalar doesn't make the panel blank.
    // Inline centering rather than using text_centered_* — those helpers
    // are defined further down the file (forward-reference would fail).
    if (rows == 1 && cols == 1) {
        int const precision = scal != nullptr ? scal->precision : 0;
        std::string const unit = (scal != nullptr) ? scal->unit : std::string{};
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f%s%s", precision, td.values[0][0],
                      unit.empty() ? "" : " ", unit.c_str());
        ImVec2 const avail = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(0.0f, avail.y * 0.20f));
        ImGui::SetWindowFontScale(1.6f);
        {
            float const avail_x = ImGui::GetContentRegionAvail().x;
            float const w = ImGui::CalcTextSize(buf).x;
            if (w < avail_x) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_x - w) * 0.5f);
            }
            ImGui::TextUnformatted(buf);
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        {
            char const *hint = "Scalar table — switch to Grid view to edit.";
            float const avail_x = ImGui::GetContentRegionAvail().x;
            float const w = ImGui::CalcTextSize(hint).x;
            if (w < avail_x) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_x - w) * 0.5f);
            }
            ImGui::TextDisabled("%s", hint);
        }
        return;
    }

    // Flatten row-major into a contiguous buffer; ImPlot's heatmap reads
    // row-major and renders values[0] at the bottom-left by default — we
    // invert the Y axis below so row 0 lines up with the grid view (top).
    std::vector<double> flat;
    flat.reserve(rows * cols);
    for (auto const &row : td.values) {
        flat.insert(flat.end(), row.begin(), row.end());
    }

    // Build tick storage at function scope — ImPlot keeps pointers, so the
    // strings must outlive EndPlot.
    auto const build_ticks = [](std::vector<double> const &axis_vals,
                                std::vector<double> &positions, std::vector<std::string> &labels,
                                std::vector<char const *> &label_ptrs) {
        positions.reserve(axis_vals.size());
        labels.reserve(axis_vals.size());
        label_ptrs.reserve(axis_vals.size());
        for (std::size_t i = 0; i < axis_vals.size(); ++i) {
            positions.push_back(static_cast<double>(i) + 0.5);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", axis_vals[i]);
            labels.emplace_back(buf);
        }
        for (auto const &s : labels) {
            label_ptrs.push_back(s.c_str());
        }
    };
    std::vector<double> x_pos, y_pos;
    std::vector<std::string> x_lbl, y_lbl;
    std::vector<char const *> x_ptrs, y_ptrs;
    build_ticks(td.axis_x, x_pos, x_lbl, x_ptrs);
    build_ticks(td.axis_y, y_pos, y_lbl, y_ptrs);

    double const min_v = stats.min;
    double const max_v = (stats.max > stats.min) ? stats.max : stats.min + 1.0;
    int const n_cells = static_cast<int>(rows * cols);
    char const *fmt = (n_cells <= 256) ? "%.1f" : nullptr;
    int const prec = scal != nullptr ? scal->precision : 1;
    char fmt_buf[8];
    if (fmt != nullptr) {
        std::snprintf(fmt_buf, sizeof(fmt_buf), "%%.%df", std::clamp(prec, 0, 3));
        fmt = fmt_buf;
    }

    // Plasma reads well on the dark theme and matches the "cool-low /
    // warm-high" intuition of the in-grid cell shading without competing
    // with it visually.
    ImPlot::PushColormap(ImPlotColormap_Plasma);

    // Reserve room on the right for the colormap scale.
    constexpr float kScaleWidth = 64.0f;
    ImVec2 const avail = ImGui::GetContentRegionAvail();
    ImVec2 const plot_size = ImVec2(avail.x - kScaleWidth - 8.0f, avail.y);

    if (ImPlot::BeginPlot("##table_heatmap", plot_size,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoTitle)) {
        auto const x_flags = ImPlotAxisFlags_NoGridLines;
        auto const y_flags = ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Invert;
        ImPlot::SetupAxes(
            (tbl != nullptr && tbl->axis_x.has_value()) ? tbl->axis_x->c_str() : nullptr,
            (tbl != nullptr && tbl->axis_y.has_value()) ? tbl->axis_y->c_str() : nullptr, x_flags,
            y_flags);

        if (!x_pos.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_X1, x_pos.data(), static_cast<int>(x_pos.size()),
                                   x_ptrs.data(), false);
        }
        if (!y_pos.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_Y1, y_pos.data(), static_cast<int>(y_pos.size()),
                                   y_ptrs.data(), false);
        }

        ImPlot::PlotHeatmap("##h", flat.data(), static_cast<int>(rows), static_cast<int>(cols),
                            min_v, max_v, fmt, ImPlotPoint(0, 0),
                            ImPlotPoint(static_cast<double>(cols), static_cast<double>(rows)));
        ImPlot::EndPlot();
    }

    ImGui::SameLine();
    ImPlot::ColormapScale("##scale", min_v, max_v, ImVec2(kScaleWidth, plot_size.y));
    ImPlot::PopColormap();
}
void render_table_grid(st::Definition::TableData const &td, st::Scaling const *scal,
                       GridStats const &stats, Selection &selection, Fonts const &fonts,
                       AppState &state, std::vector<std::vector<bool>> const &edited_mask) {
    int const precision = scal != nullptr ? scal->precision : 0;
    auto const grid_rows = td.values.size();
    auto const grid_cols_count = grid_rows == 0 ? std::size_t{0} : td.values.front().size();

    // No values at all → genuinely nothing to render (read_table_values
    // would normally have errored before we got here, but be defensive).
    if (grid_cols_count == 0) {
        ImGui::TextDisabled("(no values)");
        return;
    }

    // Column count: one leftmost label column + one data column per cell.
    // We use max(axis_x.size(), grid_cols_count) so that:
    //  - Normal 2D/1D tables (axis_x.size() == grid_cols_count): unchanged.
    //  - Scalar tables (axis_x empty, grid_cols_count == 1): we still render
    //    one data column with a synthesized "value" header instead of bailing
    //    out with a disabled "(no X axis)" hint, which read to the user as
    //    "no data in the right hand panel" for every dim=0 cell-constant.
    //  - Pack-malformed cases where axis_x is shorter than the row's data:
    //    we expose the trailing data with placeholder [n] headers instead
    //    of silently dropping cells.
    int const cols =
        static_cast<int>(std::max(td.axis_x.size(), grid_cols_count)) + 1;

    // ---- Keyboard navigation ----
    // Arrow keys move the cursor (collapsing the selection to a single
    // cell); Shift+arrows extend by moving the cursor but leaving the
    // anchor. Movement is clamped at edges (no wrap). The "Table"
    // window must be the current focused window — gating prevents
    // arrows from competing with the sidebar's filter input or any
    // other input that has keyboard focus.
    bool scroll_to_cursor = false;
    // Arrow keys + F2 are app-level, not InputText keystrokes — gate on
    // not-editing AND Table-window-focused. While the cell editor is
    // active, InputText absorbs arrows for text cursor motion and F2
    // would re-trigger edit mode mid-edit.
    if (!state.editing_cell && selection.enabled && grid_rows > 0 && grid_cols_count > 0 &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        bool const shift = ImGui::GetIO().KeyShift;
        bool moved = false;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (selection.r_cursor > 0) {
                --selection.r_cursor;
                moved = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (selection.r_cursor + 1 < grid_rows) {
                ++selection.r_cursor;
                moved = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            if (selection.c_cursor > 0) {
                --selection.c_cursor;
                moved = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            if (selection.c_cursor + 1 < grid_cols_count) {
                ++selection.c_cursor;
                moved = true;
            }
        }
        if (moved) {
            if (!shift) {
                // Collapse to single cell so subsequent arrow taps
                // move the whole selection rather than re-extending
                // from the stale anchor.
                selection.r_anchor = selection.r_cursor;
                selection.c_anchor = selection.c_cursor;
            }
            scroll_to_cursor = true;
        }
        // F2 enters cell edit mode on the cursor cell. Excel's
        // canonical "edit this cell" shortcut. Suppressed when the
        // active slot is read-only (source); additional ROMs are
        // editable now via per-ROM history (Issue #10 phase 3).
        bool const slot_editable = state.project->active_rom_mut() != nullptr;
        if (slot_editable &&
            ImGui::IsKeyPressed(ImGuiKey_F2, /*repeat=*/false)) {
            std::snprintf(state.edit_buf, sizeof state.edit_buf, "%.*f", precision,
                          (selection.r_cursor < td.values.size() &&
                           selection.c_cursor < td.values[selection.r_cursor].size())
                              ? td.values[selection.r_cursor][selection.c_cursor]
                              : 0.0);
            state.editing_cell = true;
            state.editor_just_opened = true;
        }
        // Ctrl+C / Ctrl+V — TSV clipboard interop. Same gating as
        // arrow nav (Table window focused, not currently editing a
        // cell) so the cell editor's InputText sees Ctrl+C/V as text
        // operations when it's active. Paste also gated on the
        // active-ROM check; copy is read-only and always fine.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copy_rect_to_clipboard(td, selection.as_rect(), precision);
        }
        if (slot_editable &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
            paste_clipboard_at_cursor(state);
        }
    }

    auto const flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    // Grids are numerical — push monospace so column alignment is honest.
    // Right-align Selectable text so cells read like a calculator pad.
    // Selectable's selected background uses ImGuiCol_Header; override to
    // the accent at ~55% alpha so it overlays the heatmap instead of
    // hiding it.
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(1.0f, 0.5f));
    auto const a_hdr = accent_for(current_theme());
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(a_hdr.base.x, a_hdr.base.y, a_hdr.base.z, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImVec4(a_hdr.hover.x, a_hdr.hover.y, a_hdr.hover.z, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          ImVec4(a_hdr.active.x, a_hdr.active.y, a_hdr.active.z, 0.65f));

    auto const pop_style = [&]() {
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
    };

    if (!ImGui::BeginTable("grid", cols, flags)) {
        pop_style();
        return;
    }

    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    // Emit one header per max(axis_x.size(), grid_cols_count). Where axis
    // values exist they label the column; otherwise we synthesize:
    //   - "value" when the table is a 1×1 scalar (most common case here)
    //   - "[n]"   for the n-th unmapped data column on multi-col rows
    auto const header_cols = static_cast<std::size_t>(cols) - 1;
    for (std::size_t c = 0; c < header_cols; ++c) {
        char buf[32];
        if (c < td.axis_x.size()) {
            std::snprintf(buf, sizeof(buf), "%g", td.axis_x[c]);
        } else if (header_cols == 1) {
            std::snprintf(buf, sizeof(buf), "value");
        } else {
            std::snprintf(buf, sizeof(buf), "[%zu]", c);
        }
        ImGui::TableSetupColumn(buf, ImGuiTableColumnFlags_WidthFixed, 80.0f);
    }
    // Custom header row so the axis-X labels can right-align to match
    // the data cells beneath. TableHeadersRow() left-aligns its label
    // text, which looks broken against right-aligned numerical data.
    // Row-flag Headers gets us the TableHeaderBg fill for free.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int col = 0; col < cols; ++col) {
        ImGui::TableSetColumnIndex(col);
        char const *name = ImGui::TableGetColumnName(col);
        if (name == nullptr || name[0] == '\0') {
            continue;
        }
        text_right_aligned(name);
    }

    auto const grid_cols = td.values.empty() ? std::size_t{0} : td.values.front().size();
    char buf[32];
    for (std::size_t r = 0; r < td.values.size(); ++r) {
        ImGui::TableNextRow();
        // Leftmost axis-Y label column: right-aligned dimmed text on a
        // header-tinted background so it reads as table chrome (matching
        // the column header row) rather than another data cell.
        ImGui::TableNextColumn();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                               ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
        if (!td.axis_y.empty() && r < td.axis_y.size()) {
            std::snprintf(buf, sizeof(buf), "%.*f", precision, td.axis_y[r]);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            text_right_aligned(buf);
            ImGui::PopStyleColor();
        }
        for (std::size_t c = 0; c < td.values[r].size(); ++c) {
            double const v = td.values[r][c];
            ImGui::TableNextColumn();
            // Background: knock overlay wins when active (analyst Issue
            // #16) — it carries safety-critical signal (observed knock
            // on a cell) that should never be hidden under value heat.
            // Without it, fall back to the value heatmap.
            ImU32 bg = 0u;
            std::optional<st::autotune::KnockCellProposal> knock_for_cell;
            if (state.show_knock_overlay && state.kp_at_result.has_value() &&
                std::string_view{state.kp_at_table_id} == state.selected_table_id) {
                auto const &res = *state.kp_at_result;
                std::size_t const flat = r * res.cols + c;
                if (flat < res.cells.size()) {
                    auto const &cell = res.cells[flat];
                    knock_for_cell = cell;
                    if (cell.pulled) {
                        // Strong red: sustained knock, autotune pulled
                        // timing from this cell.
                        bg = IM_COL32(190, 50, 50, 160);
                    } else if (cell.samples_used > 0) {
                        // Faint green: cell saw samples, no sustained
                        // knock observed. Confidence affordance — the
                        // tune is validated here, not just untouched.
                        bg = IM_COL32(60, 130, 70, 75);
                    }
                    // else: zero samples → no override, value heatmap shows
                }
            }
            if (bg == 0u) {
                bg = heatmap_color(v, stats.min, stats.max);
            }
            if (bg != 0u) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, bg);
            }
            std::snprintf(buf, sizeof(buf), "%.*f", precision, v);

            ImGui::PushID(static_cast<int>(r * grid_cols + c));
            bool const is_sel = selection.contains(r, c);
            bool const is_cursor =
                selection.enabled && r == selection.r_cursor && c == selection.c_cursor;

            if (state.editing_cell && is_cursor) {
                // Render the InputText in place of the Selectable for
                // the cell being edited. Fill the cell width so the
                // visual swap feels in-place. AutoSelectAll makes "F2,
                // type 14.7, Enter" replace the value cleanly; the
                // user doesn't have to clear the buffer first.
                if (state.editor_just_opened) {
                    ImGui::SetKeyboardFocusHere();
                    state.editor_just_opened = false;
                }
                ImGui::SetNextItemWidth(-1.0f);
                bool const enter = ImGui::InputText(
                    "##cell_editor", state.edit_buf, sizeof state.edit_buf,
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                        ImGuiInputTextFlags_CharsScientific);
                bool const deactivated = ImGui::IsItemDeactivated();
                bool const escaped = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

                auto const exit_edit = [&] {
                    state.editing_cell = false;
                    state.editor_just_opened = false;
                };
                // `whole_selection = true` writes the value to every
                // selected cell (Excel's Ctrl+Enter convention). False
                // collapses to the cursor cell. Failure to parse
                // silently cancels — better than discarding a
                // half-typed number with no feedback.
                auto const commit = [&](bool whole_selection) {
                    double parsed = 0.0;
                    if (std::sscanf(state.edit_buf, "%lf", &parsed) == 1) {
                        if (!whole_selection) {
                            // Collapse selection to this cell so
                            // apply_op's rect is single-cell.
                            selection.r_anchor = selection.r_cursor;
                            selection.c_anchor = selection.c_cursor;
                        }
                        apply_op(state, whole_selection ? "fill" : "set",
                                 [parsed](auto &t, auto rect) {
                                     return st::edit::set_cells(t, rect, parsed);
                                 });
                    }
                    exit_edit();
                };

                if (escaped) {
                    // Esc cancels — don't commit; restore-by-not-applying.
                    exit_edit();
                } else if (enter) {
                    // Ctrl+Enter writes to every selected cell; plain
                    // Enter writes only to the cursor cell. KeyCtrl is
                    // the state at the moment InputText returned true.
                    commit(/*whole_selection=*/ImGui::GetIO().KeyCtrl);
                } else if (deactivated) {
                    // Lose-focus path: treat as plain Enter (single
                    // cell). Ctrl+Enter is an explicit gesture; we
                    // don't infer it from a stray click.
                    commit(/*whole_selection=*/false);
                }
            } else {
                if (ImGui::Selectable(buf, is_sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selection.click(r, c, ImGui::GetIO().KeyShift);
                    // Double-click promotes the click into edit mode,
                    // mirroring Excel/Sheets. AllowDoubleClick on the
                    // Selectable above is what lets us see the second
                    // click here.
                    if (state.project->active_rom_mut() != nullptr &&
                        ImGui::IsMouseDoubleClicked(0)) {
                        std::snprintf(state.edit_buf, sizeof state.edit_buf, "%.*f", precision, v);
                        state.editing_cell = true;
                        state.editor_just_opened = true;
                    }
                }
                // Knock overlay (Issue #16) — show per-cell metrics on
                // hover when active. Same precondition that drives the
                // cell background tint above; this just makes the
                // numbers reachable without leaving the Table panel.
                if (knock_for_cell.has_value() && ImGui::IsItemHovered()) {
                    auto const &k = *knock_for_cell;
                    if (k.pulled) {
                        ImGui::SetTooltip("Knock pulled — %zu samples, mean FBKC %.2f deg\n"
                                          "current %.2f  ->  proposed %.2f",
                                          k.samples_used,
                                          static_cast<double>(k.mean_feedback_knock),
                                          static_cast<double>(k.current_value),
                                          static_cast<double>(k.proposed_value));
                    } else if (k.samples_used > 0) {
                        ImGui::SetTooltip("Clean — %zu samples, mean FBKC %.2f deg",
                                          k.samples_used,
                                          static_cast<double>(k.mean_feedback_knock));
                    }
                }
                // Right-click selects the cell (if not already in the
                // selection) and opens a Copy/Paste context menu. The
                // implicit re-select matches Excel — right-clicking a
                // cell outside the current selection makes that cell
                // the new scope of the operation, rather than
                // silently using the previous selection.
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selection.contains(r, c)) {
                    selection.click(r, c, /*shift=*/false);
                }
                if (ImGui::BeginPopupContextItem("##cell_ctx")) {
                    bool const has_sel = selection.enabled;
                    bool const editing_allowed =
                        state.project->active_rom_mut() != nullptr;
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_sel)) {
                        copy_rect_to_clipboard(td, selection.as_rect(), precision);
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                                        has_sel && editing_allowed)) {
                        paste_clipboard_at_cursor(state);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset to Source", nullptr, false,
                                        has_sel && editing_allowed)) {
                        reset_selection_to_source(state);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("Set the selected cells back to the values "
                                          "in the source ROM\n(undoable; safer than "
                                          "Close → reopen).");
                    }
                    ImGui::EndPopup();
                }
            }

            // Edited-cell marker. A small accent dot in the top-left
            // corner of any cell whose working value differs from its
            // source value. Left corner (not right) so it doesn't
            // collide with the right-aligned cell value text. Drawn on
            // the window draw list so it overlays the heatmap shading
            // and the selection tint without competing with the value.
            bool const edited =
                r < edited_mask.size() && c < edited_mask[r].size() && edited_mask[r][c];
            if (edited) {
                ImVec2 const a = ImGui::GetItemRectMin();
                ImVec2 const b = ImGui::GetItemRectMax();
                if (b.x > a.x && b.y > a.y) {
                    auto *const dl = ImGui::GetWindowDrawList();
                    constexpr float pad = 3.0f;
                    constexpr float r_dot = 2.5f;
                    ImVec2 const center(a.x + pad + r_dot, a.y + pad + r_dot);
                    dl->AddCircleFilled(center, r_dot, IM_COL32(190, 215, 255, 235));
                }
            }

            // Cursor-cell outline. Within a multi-cell selection, this
            // is the active cell — where F2 opens the editor and where
            // arrow keys move from. Bright accent border mirrors the
            // Excel/Sheets convention so the focus is unambiguous when
            // the selection covers more than one cell.
            if (is_cursor && !state.editing_cell) {
                ImVec2 const a = ImGui::GetItemRectMin();
                ImVec2 const b = ImGui::GetItemRectMax();
                if (b.x > a.x && b.y > a.y) {
                    auto *const dl = ImGui::GetWindowDrawList();
                    dl->AddRect(a, b, IM_COL32(120, 180, 250, 255), 0.0f, 0, 2.0f);
                }
            }

            // After arrow-key movement, scroll the cursor cell into
            // the visible viewport so it never leaves the screen when
            // the user navigates with the keyboard.
            if (scroll_to_cursor && is_cursor) {
                ImGui::SetScrollHereY(0.5f);
                ImGui::SetScrollHereX(0.5f);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    pop_style();
}

} // namespace

void render_table_view(AppState &state, Fonts const &fonts) {
    if (!state.show_table_view_panel) {
        return;
    }
    // First-show fallback so toggling Table on outside the Tune
    // workspace (whose layout docks it as the central window)
    // doesn't drop a floating window at the default top-left.
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("Table");
    track_help_context(state,
                       state.project.has_value() ? AppState::HelpContext::TableEditor
                                                 : AppState::HelpContext::Welcome);

    if (!state.project.has_value()) {
        render_welcome_panel(state);
        ImGui::End();
        return;
    }
    if (state.selected_table_id.empty()) {
        // Project is loaded but no table picked — soft nudge at the sidebar.
        ImVec2 const avail = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(0.0f, avail.y * 0.30f));
        text_centered("Pick a table from the left panel to start.", 1.2f);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        text_centered_disabled("Hover any table name for its details.");
        ImGui::End();
        return;
    }
    if (!state.current_table_data.has_value()) {
        // Soft error: table exists in the pack but its bytes can't be
        // read from the working ROM. Usually a pack/ROM-version
        // mismatch or an address that's out of bounds for this ROM
        // size. Phrased as guidance, not a stack trace.
        char hint[256];
        std::snprintf(hint, sizeof hint,
                      "'%s' is declared in the pack but its bytes don't decode against the loaded ROM. "
                      "Check the pack matches this ROM's CID.",
                      state.selected_table_id.c_str());
        render_empty_state("Couldn't read this table", hint);
        ImGui::End();
        return;
    }

    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    auto const *scal =
        tbl != nullptr ? state.project->definition().find_scaling(tbl->scaling) : nullptr;
    int const precision = scal != nullptr ? scal->precision : 0;

    // Header: title row holds the human-readable name + unit chip + tiny
    // ●S / ●E dots for safety/emissions flags. Everything else (id, 0xaddr,
    // dimensions, category) moves into the title-hover tooltip. The
    // header used to stack 5-7 lines of meta before the grid; this trims
    // it to one line + the heatmap legend (kept — load-bearing).
    bool const have_name = (tbl != nullptr && !tbl->name.empty());
    char const *title = have_name ? tbl->name.c_str() : state.selected_table_id.c_str();

    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(title);
    bool const title_hovered = ImGui::IsItemHovered();
    ImGui::SetWindowFontScale(1.0f);
    if (title_hovered && tbl != nullptr) {
        ImGui::BeginTooltip();
        if (have_name) {
            ImGui::TextUnformatted(state.selected_table_id.c_str());
            ImGui::Separator();
        }
        ImGui::Text("%dD  \xC2\xB7  0x%08zX", tbl->dimensions, tbl->address);
        if (!tbl->category.empty()) {
            text_subtle("category: %s", tbl->category.c_str());
        }
        if (scal != nullptr && !scal->id.empty()) {
            text_subtle("scaling: %s", scal->id.c_str());
        }
        ImGui::EndTooltip();
    }

    // Unit chip — most signal-dense piece of metadata at a glance
    // ("g/min", "kPa", "%"). Stays a verbose chip; the safety/emissions
    // flags below collapse to single-character dots because their
    // verbose forms were visual noise once a user knew the meaning.
    auto place_chip = [](char const *text, ImVec4 fg, ImVec4 bg, char const *tooltip = nullptr) {
        constexpr float kChipPad = 16.0f; // FramePadding.x * 2 from chip()
        float const w = ImGui::CalcTextSize(text).x + kChipPad;
        ImGui::SameLine();
        float const cx = ImGui::GetCursorPosX();
        float const max_x = ImGui::GetWindowContentRegionMax().x;
        if (cx + w > max_x) {
            ImGui::NewLine();
        }
        chip(text, fg, bg);
        if (tooltip != nullptr && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
    };
    if (scal != nullptr && !scal->unit.empty()) {
        place_chip(scal->unit.c_str(), chip_fg_accent(), chip_bg_accent());
    }
    // Tiny safety / emissions dots. U+25CF ● drawn in the same warn /
    // caution palette the verbose chips used, so the color language
    // matches the sidebar S/E badges. Hover surfaces the full caveat.
    auto place_dot = [](ImVec4 fg, char const *letter, char const *tooltip) {
        ImGui::SameLine();
        ImGui::TextColored(fg, "\xE2\x97\x8F%s", letter);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
    };
    if (tbl != nullptr && tbl->engine_safety_critical) {
        place_dot(chip_fg_warn(), "S",
                  "Engine safety critical.\n"
                  "Cells in this table affect engine safety — wrong values\n"
                  "can damage the engine. Make small changes, verify, and\n"
                  "keep a stock backup of the working ROM before flashing.");
    }
    if (tbl != nullptr && tbl->emissions_relevant) {
        place_dot(chip_fg_caution(), "E",
                  "Emissions-relevant.\n"
                  "Cells in this table influence the vehicle's emissions\n"
                  "behavior. Jurisdiction profile (see docs/06-legal-ethics)\n"
                  "governs warnings; engine-safety refusals still apply.");
    }

    // Issue #15 inverse cross-reference: PIDs that name THIS table in
    // their `produces_table` field. The forward direction (gauge ->
    // table) ships via the gauge cluster's right-click menu; the
    // inverse direction (this table -> which channels log it) belongs
    // here so a tuner knows "this map drives the boost gauge". Chip
    // tooltip enumerates the pid ids — typical mapping is one pid per
    // table, but the inverse iteration is general so packs that wire
    // multiple sample paths (e.g. requested vs actual boost) surface
    // both.
    if (tbl != nullptr) {
        auto const producers = state.project->definition().find_pids_producing(tbl->id);
        if (!producers.empty()) {
            char chip_text[64];
            if (producers.size() == 1) {
                std::snprintf(chip_text, sizeof chip_text, "Logged by: %s",
                              producers[0]->id.c_str());
            } else {
                std::snprintf(chip_text, sizeof chip_text, "Logged by: %zu channels",
                              producers.size());
            }
            std::string tooltip = "Channels that emit this table's output:\n";
            for (auto const *p : producers) {
                tooltip += "  - ";
                tooltip += p->id;
                if (!p->name.empty() && p->name != p->id) {
                    tooltip += "  (";
                    tooltip += p->name;
                    tooltip += ")";
                }
                tooltip += "\n";
            }
            tooltip += "Right-click the matching gauge in the Gauge Cluster\n"
                       "panel to jump back to this table.";
            place_chip(chip_text, chip_fg_accent(), chip_bg_accent(), tooltip.c_str());
        }
    }

    // For 3D tables, project the chosen Z slice into a 2D view that the
    // renderers and stats can consume uniformly. The edit infrastructure
    // (Rect / Snapshot / History) is 2D-only, so editing is gated off for
    // 3D below — slice-aware edits are a follow-up.
    auto const &td_orig = *state.current_table_data;
    bool const is_3d = (tbl != nullptr && tbl->dimensions == 3 && !td_orig.slices.empty());
    if (is_3d && state.selected_z >= td_orig.slices.size()) {
        state.selected_z = 0;
    }
    st::Definition::TableData td_view;
    if (is_3d) {
        td_view.axis_x = td_orig.axis_x;
        td_view.axis_y = td_orig.axis_y;
        td_view.values = td_orig.slices[state.selected_z];
    } else {
        td_view = td_orig;
    }

    // Per-cell edited mask: working value != source value. The grid
    // renderer paints a small accent dot on these so user-modified
    // cells are scannable at a glance. read_table_values applies the
    // same scaling pipeline to both ROMs, so a byte-equal source
    // produces a bit-equal double — `==` is correct here.
    std::vector<std::vector<bool>> edited_mask;
    if (tbl != nullptr) {
        if (auto td_src_res =
                state.project->definition().read_table_values(state.project->source_rom(), *tbl);
            td_src_res.has_value()) {
            auto const &td_src = *td_src_res;
            std::vector<std::vector<double>> const *src_2d = nullptr;
            if (is_3d) {
                if (state.selected_z < td_src.slices.size()) {
                    src_2d = &td_src.slices[state.selected_z];
                }
            } else {
                src_2d = &td_src.values;
            }
            edited_mask.resize(td_view.values.size());
            for (std::size_t r = 0; r < td_view.values.size(); ++r) {
                edited_mask[r].assign(td_view.values[r].size(), false);
                if (src_2d == nullptr) {
                    continue;
                }
                for (std::size_t c = 0; c < td_view.values[r].size(); ++c) {
                    if (r < src_2d->size() && c < (*src_2d)[r].size()) {
                        edited_mask[r][c] = (td_view.values[r][c] != (*src_2d)[r][c]);
                    }
                }
            }
        }
    }

    if (is_3d) {
        char preview[64];
        if (state.selected_z < td_orig.axis_z.size()) {
            std::snprintf(preview, sizeof(preview), "z = %g", td_orig.axis_z[state.selected_z]);
        } else {
            std::snprintf(preview, sizeof(preview), "slice %zu", state.selected_z);
        }
        ImGui::TextUnformatted("Z slice:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo("##z_slice", preview)) {
            for (std::size_t i = 0; i < td_orig.slices.size(); ++i) {
                char label[64];
                if (i < td_orig.axis_z.size()) {
                    std::snprintf(label, sizeof(label), "z = %g", td_orig.axis_z[i]);
                } else {
                    std::snprintf(label, sizeof(label), "slice %zu", i);
                }
                bool const sel = (state.selected_z == i);
                if (ImGui::Selectable(label, sel)) {
                    state.selected_z = i;
                    // Refresh the slice view immediately so stats reflect
                    // the new choice on this same frame.
                    td_view.values = td_orig.slices[state.selected_z];
                }
                if (sel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        text_subtle("(%zu slices · 3D editing TBD)", td_orig.slices.size());
    }

    GridStats const stats = compute_stats(td_view);
    if (stats.count > 0) {
        // Table-wide stats moved into the Stats panel — they duplicated
        // what the right-rail panel already showed, and the header was
        // stacking 5-7 lines of meta before the grid. The selection-
        // scoped stats below still render inline because they're
        // contextual to the +5% / Smooth / Interpolate action the user
        // is about to take. The heatmap legend stays inline (load-
        // bearing in Grid view).
        // Heatmap legend — only meaningful in Grid view (Heatmap view
        // already has a vertical ColormapScale on the right via ImPlot).
        // Sampled from heatmap_color so the strip's gradient matches
        // what cells actually paint. A solid backing rect underneath
        // gives the mid-range alpha-zero region somewhere to sit —
        // otherwise the legend's middle is invisible against the
        // panel background, defeating the purpose.
        if (state.view_mode == TableViewMode::Grid && stats.max > stats.min) {
            constexpr float kBarW = 220.0f;
            constexpr float kBarH = 12.0f;
            constexpr int kSegs = 64;
            text_subtle("scale:");
            ImGui::SameLine();
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.*f", precision, stats.min);
            text_subtle("%s", buf);
            ImGui::SameLine();
            ImVec2 const p0 = ImGui::GetCursorScreenPos();
            ImVec2 const p1 = ImVec2(p0.x + kBarW, p0.y + kBarH);
            auto *const dl = ImGui::GetWindowDrawList();
            // Backing rect: subtle dark base so the transparent middle
            // of the heatmap ramp has something to be transparent
            // against. ~10% white over the table row bg matches the
            // visual weight of a cell with no heatmap shading.
            dl->AddRectFilled(p0, p1, IM_COL32(40, 42, 48, 255));
            // N small filled rects sample the ramp evenly. 64 segments
            // is more than enough to look smooth at 220 px wide.
            for (int i = 0; i < kSegs; ++i) {
                double const t0 = static_cast<double>(i) / static_cast<double>(kSegs);
                double const t1 = static_cast<double>(i + 1) / static_cast<double>(kSegs);
                double const v_mid = stats.min + 0.5 * (t0 + t1) * (stats.max - stats.min);
                ImU32 const col = heatmap_color(v_mid, stats.min, stats.max);
                if (col != 0u) {
                    float const x0 = p0.x + kBarW * static_cast<float>(t0);
                    float const x1 = p0.x + kBarW * static_cast<float>(t1);
                    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y), col);
                }
            }
            // Reserve the layout space the draw-list calls consumed so
            // the next widget after this lays out below correctly.
            ImGui::Dummy(ImVec2(kBarW, kBarH));
            ImGui::SameLine();
            std::snprintf(buf, sizeof buf, "%.*f", precision, stats.max);
            text_subtle("%s", buf);
        }
    }
    if (state.selection.enabled) {
        auto const rect = state.selection.as_rect();
        // Selection-scoped stats. Lets the user see what they're about
        // to apply +5% / Smooth / Interpolate to before they click.
        // Parallels the table-wide stats line above so the eye can
        // compare scope-to-scope at a glance.
        double smin = std::numeric_limits<double>::infinity();
        double smax = -std::numeric_limits<double>::infinity();
        double ssum = 0.0;
        std::size_t scount = 0;
        for (std::size_t r = rect.r_start; r <= rect.r_end && r < td_view.values.size(); ++r) {
            auto const &row = td_view.values[r];
            for (std::size_t c = rect.c_start; c <= rect.c_end && c < row.size(); ++c) {
                double const v = row[c];
                if (v < smin)
                    smin = v;
                if (v > smax)
                    smax = v;
                ssum += v;
                ++scount;
            }
        }
        if (scount > 0) {
            double const smean = ssum / static_cast<double>(scount);
            text_subtle("selection: rows %zu:%zu × cols %zu:%zu  ·  "
                        "min %.*f  ·  max %.*f  ·  mean %.*f  ·  %zu cells",
                        rect.r_start, rect.r_end, rect.c_start, rect.c_end, precision, smin,
                        precision, smax, precision, smean, scount);
        } else {
            // Selection rect lies outside the visible data (e.g.,
            // mid-3D-slice change with a stale selection). Fall back
            // to the previous shape so the user still sees the rect.
            text_subtle("selection: rows %zu:%zu × cols %zu:%zu  (%zu cells)", rect.r_start,
                        rect.r_end, rect.c_start, rect.c_end,
                        state.selection.rows() * state.selection.cols());
        }
    }

    // Edit toolbar — ops act on the current selection, undo/redo on
    // the active slot's history. Buttons are disabled when there's no
    // selection / nothing to undo, rather than hidden, so the
    // affordances stay visible. 3D editing is gated off — the edit
    // pipeline assumes a single 2D grid. Source ROM is the only
    // read-only active slot (Issue #10 phase 3): editing_allowed
    // false → toolbar locks regardless of selection.
    bool const editing_allowed = state.project->active_rom_mut() != nullptr;
    bool const can_edit = state.selection.enabled && !is_3d && editing_allowed;
    bool const can_undo = state.project->active_history().can_undo() && editing_allowed;
    bool const can_redo = state.project->active_history().can_redo() && editing_allowed;

    // Hover tooltips need to render even when the button is disabled — wrap
    // BeginDisabled with ImGuiItemFlags_AllowWhenDisabled on hover. When
    // editing is gated by the active-ROM check, the "why" message is
    // different from the no-selection case — pick whichever fits.
    auto const tip = [&](char const *body, char const *when_disabled = nullptr) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            char const *why = nullptr;
            if (!editing_allowed) {
                why = "Source ROM is read-only. Switch View → Active ROM "
                      "to an editable slot.";
            } else if (!can_edit && when_disabled != nullptr) {
                why = when_disabled;
            }
            if (why != nullptr) {
                ImGui::SetTooltip("%s\n\n%s", body, why);
            } else {
                ImGui::SetTooltip("%s", body);
            }
        }
    };
    constexpr char const *kNoSelMsg = "Select cells in the grid to enable.";

    ImGui::BeginDisabled(!can_edit);
    if (ImGui::Button("+5%")) {
        apply_op(state, "+5%",
                 [](auto &t, auto r) { return st::edit::percent_scale_cells(t, r, 5.0); });
    }
    tip("Increase each selected cell by 5% of its current value.", kNoSelMsg);
    ImGui::SameLine();
    if (ImGui::Button("-5%")) {
        apply_op(state, "-5%",
                 [](auto &t, auto r) { return st::edit::percent_scale_cells(t, r, -5.0); });
    }
    tip("Decrease each selected cell by 5% of its current value.", kNoSelMsg);
    ImGui::SameLine();
    if (ImGui::Button("Smooth")) {
        apply_op(state, "smooth", [](auto &t, auto r) { return st::edit::smooth_cells(t, r, 1); });
    }
    tip("Replace each selected cell with the average of its neighbors.\n"
        "Stays inside the selection; useful for evening out spikes.",
        kNoSelMsg);
    ImGui::SameLine();
    if (ImGui::Button("Interpolate")) {
        apply_op(state, "interpolate",
                 [](auto &t, auto r) { return st::edit::interpolate_cells(t, r); });
    }
    tip("Bilinear interpolation across the selection from its four corners.\n"
        "Fades edits smoothly between known anchor cells.",
        kNoSelMsg);
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 24.0f);

    // MDL2 icon codepoints for the toolbar (loaded via merged-mode font
    // in theme.cpp's load_icon_font_merged):
    //   E7A7 Undo            \xEE\x9E\xA7
    //   E7A6 Redo            \xEE\x9E\xA6
    //   E74E Save (floppy)   \xEE\x9D\x8E
    //   E945 LightningBolt   \xEE\xA5\x85  — "flash to ECU"
    ImGui::BeginDisabled(!can_undo);
    if (ImGui::Button("\xEE\x9E\xA7 Undo")) {
        do_undo(state);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Undo the last edit.  (Ctrl+Z)");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo);
    if (ImGui::Button("\xEE\x9E\xA6 Redo")) {
        do_redo(state);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Redo the next edit.  (Ctrl+Shift+Z or Ctrl+Y)");
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::BeginDisabled(!state.dirty);
    if (ImGui::Button("\xEE\x9D\x8E Save")) {
        save_project(state);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.dirty) {
            ImGui::SetTooltip("Write the working ROM + edits to disk.  (Ctrl+S)");
        } else {
            ImGui::SetTooltip("No unsaved edits.  (Ctrl+S)");
        }
    }
    ImGui::EndDisabled();

    // Copy table — writes the entire grid to clipboard as TSV.
    // Axis-Y label column on the left, axis-X label header row on
    // top. Pastes cleanly into a spreadsheet or markdown editor.
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.project.has_value() || !state.current_table_data.has_value());
    if (ImGui::Button("\xEE\xA2\xB4 Copy table")) { // E8B4 Copy
        auto const &td = *state.current_table_data;
        std::string tsv;
        tsv.reserve(td.values.size() * (td.values.empty() ? 0 : td.values[0].size()) * 8);
        // Header row: empty corner + axis_x labels (when present).
        if (!td.axis_x.empty()) {
            tsv.push_back('\t');
            for (std::size_t c = 0; c < td.axis_x.size(); ++c) {
                if (c > 0)
                    tsv.push_back('\t');
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.*f", precision, td.axis_x[c]);
                tsv.append(buf);
            }
            tsv.push_back('\n');
        }
        for (std::size_t r = 0; r < td.values.size(); ++r) {
            if (r < td.axis_y.size()) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.*f", precision, td.axis_y[r]);
                tsv.append(buf);
            }
            tsv.push_back('\t');
            for (std::size_t c = 0; c < td.values[r].size(); ++c) {
                if (c > 0)
                    tsv.push_back('\t');
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.*f", precision, td.values[r][c]);
                tsv.append(buf);
            }
            tsv.push_back('\n');
        }
        ImGui::SetClipboardText(tsv.c_str());
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Copy the entire grid to clipboard as TSV.\n"
                          "Axis-Y labels in the first column, axis-X in\n"
                          "the header row. Pastes cleanly into a spreadsheet.");
    }

    // Flash button — opens the policy-gate modal. Enabled whenever there's
    // a project loaded; the modal itself handles the "nothing to flash"
    // and "blocked by policy" branches.
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.project.has_value());
    if (ImGui::Button("\xEE\xA5\x85 Flash...")) {
        state.show_flash_modal = true;
        state.flash_confirm_checked = false;
        state.flash_reason[0] = '\0';
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Preview the flash plan (source -> working) and run the\n"
                          "EmissionsLinter under the project's jurisdiction profile.\n"
                          "Real transport not yet wired — the modal shows what would\n"
                          "happen without sending bytes to an ECU.");
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted("View:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", state.view_mode == TableViewMode::Grid)) {
        state.view_mode = TableViewMode::Grid;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Numerical grid with heatmap shading.\n"
                          "Click cells to select; Shift-click to extend.");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Heatmap", state.view_mode == TableViewMode::Heatmap)) {
        state.view_mode = TableViewMode::Heatmap;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Color-coded heatmap rendered against the real axis values.\n"
                          "Reading-only view; switch back to Grid to edit.");
    }

    // Knock overlay toggle (analyst Issue #16). Only meaningful when the
    // active table matches the last knock-pull autotune run AND we're
    // looking at the Grid view (heatmap-on-heatmap reads as noise).
    bool const knock_overlay_available =
        state.kp_at_result.has_value() &&
        std::string_view{state.kp_at_table_id} == state.selected_table_id &&
        state.view_mode == TableViewMode::Grid;
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::BeginDisabled(!knock_overlay_available);
    if (ImGui::Checkbox("Knock overlay", &state.show_knock_overlay)) {
        // No side-effect on toggle; render path reads the flag.
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (knock_overlay_available) {
            ImGui::SetTooltip("Paint per-cell knock-event heat over the grid:\n"
                              "  faint green = samples observed, no pull\n"
                              "  strong red  = sustained knock (cell pulled)\n"
                              "Source: the last Knock Pull autotune run.");
        } else if (!state.kp_at_result.has_value()) {
            ImGui::SetTooltip("Run Edit → Knock Pull autotune against a CSV log\n"
                              "to populate the per-cell knock heat for this table.");
        } else {
            ImGui::SetTooltip("Switch to the table the knock autotune was run\n"
                              "against, or rerun the autotune for this table.");
        }
    }

    ImGui::Separator();

    if (state.view_mode == TableViewMode::Heatmap) {
        render_table_heatmap(td_view, tbl, scal, stats);
    } else {
        render_table_grid(td_view, scal, stats, state.selection, fonts, state, edited_mask);
    }

    // Escape clears the current selection when the Table panel has focus.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.selection.reset();
    }
    ImGui::End();
}

} // namespace st::ui
