// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// History panel — GUI mirror of `subuwutuner-cli project-history`.
// Lists every edit in insertion order with a marker for the current
// History cursor, optional table-id filter, and a per-row "Jump"
// button that walks the cursor to make that edit the most-recently-
// applied. Jumps re-use do_undo / do_redo so the dirty flag, status
// line, and the currently-displayed table data all stay coherent with
// manual Ctrl+Z.

#include "panels/panels.hpp"

#include "actions.hpp"
#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace st::ui {

void render_history_panel(AppState &state) {
    if (!state.show_history_panel) {
        return;
    }
    // First-show fallback so toggling History on outside the Tune
    // workspace (whose layout docks it) doesn't drop a floating window
    // at the default top-left.
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("History", &state.show_history_panel)) {
        ImGui::End();
        return;
    }
    // F1 on the History panel routes to docs/21-stune-format — the
    // edits.toml schema documentation is the most useful starting
    // point for understanding what the history table is showing.
    track_help_context(state, AppState::HelpContext::History);
    if (!state.project.has_value()) {
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to see your edit history here. Ctrl+Z and Ctrl+Shift+Z navigate it.");
        ImGui::End();
        return;
    }
    // Each ROM carries its own per-ROM history (Issue #10 phase 3).
    // The panel renders the active slot's records so switching active
    // ROM also switches which timeline the user sees.
    auto const &records = state.project->active_history().records();
    auto const cursor = state.project->active_history().cursor();

    ImGui::Text("%zu edit(s), cursor at %zu", records.size(), cursor);
    if (cursor < records.size()) {
        ImGui::SameLine();
        text_subtle("(%zu redo step%s)", records.size() - cursor,
                    records.size() - cursor == 1 ? "" : "s");
    }

    ImGui::InputTextWithHint("##history_filter", "Filter by table id or op…", state.history_filter,
                             sizeof state.history_filter);
    std::string_view const filter{state.history_filter};

    // Op-kind filter chips. Extract a "kind" from each record's
    // description by lopping off everything from the first ' (' onward
    // — "autotune knock-pull (45 pulled)" → "autotune knock-pull". One
    // chip per distinct kind in the current records. Active chip
    // becomes the text filter; re-click clears.
    auto const op_kind_of = [](std::string_view desc) -> std::string_view {
        auto const paren = desc.find(" (");
        return paren == std::string_view::npos ? desc : desc.substr(0, paren);
    };
    if (!records.empty()) {
        std::vector<std::string_view> kinds_seen;
        for (auto const &r : records) {
            auto const kn = op_kind_of(r.description);
            if (kn.empty())
                continue;
            bool dup = false;
            for (auto k : kinds_seen) {
                if (k == kn) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                kinds_seen.push_back(kn);
            }
        }
        std::sort(kinds_seen.begin(), kinds_seen.end());
        for (auto kn : kinds_seen) {
            std::string const kn_str{kn};
            bool const active = (std::string_view{state.history_filter} == kn);
            if (active) {
                push_primary_button_colors();
            }
            ImGui::PushID(kn_str.c_str());
            if (ImGui::SmallButton(kn_str.c_str())) {
                if (active) {
                    state.history_filter[0] = '\0';
                } else {
                    std::snprintf(state.history_filter, sizeof state.history_filter, "%s",
                                  kn_str.c_str());
                }
            }
            ImGui::PopID();
            if (active) {
                pop_primary_button_colors();
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    if (records.empty()) {
        ImGui::Separator();
        // Empty-but-project-loaded: keep the header + filter visible
        // above so the user sees the panel is "alive" — just no rows
        // yet. The empty-state hint says how to fill it.
        render_empty_state(
            "No edits yet",
            "Click a cell in the table grid and type a new value. Every change is an undoable history step.");
        ImGui::End();
        return;
    }

    ImGui::Separator();
    // Quick-action row: walk the cursor all the way down or up.
    bool wants_revert_all = false;
    bool wants_reapply_all = false;
    // "Revert all" is destructive (walks the cursor to 0). Use a
    // two-click arm pattern so a fat-finger doesn't wipe 100+ edits.
    // First click → label flips to "Click again to confirm" for ~2s
    // and the button turns red-ish; second click within that window
    // runs the revert. Timer expires → state resets quietly.
    constexpr double kRevertArmTtl = 2.0;
    static double s_revert_armed_at = -1.0;
    double const now = ImGui::GetTime();
    bool const armed = s_revert_armed_at >= 0.0 && (now - s_revert_armed_at) < kRevertArmTtl;
    char const *revert_label = armed ? "Click again to confirm" : "Revert all";
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.36f, 0.36f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.40f, 0.40f, 1.0f));
    }
    bool const revert_clicked = ImGui::SmallButton(revert_label);
    if (armed)
        ImGui::PopStyleColor(3);
    if (revert_clicked) {
        if (armed) {
            wants_revert_all = true;
            s_revert_armed_at = -1.0;
        } else {
            s_revert_armed_at = now;
        }
    }
    if (ImGui::IsItemHovered()) {
        if (armed) {
            ImGui::SetTooltip("About to walk the cursor back through every edit "
                              "(redo stays available — new edits clear it).");
        } else {
            ImGui::SetTooltip("Undo every edit — cursor returns to 0 (ignores filter). "
                              "Click twice to confirm. Redo stays available until you "
                              "make a new edit.");
        }
    }
    ImGui::SameLine();
    bool const can_reapply = cursor < records.size();
    if (!can_reapply)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton("Re-apply all")) {
        wants_reapply_all = true;
    }
    if (!can_reapply)
        ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Redo every undone edit — cursor returns to the end "
                          "(ignores filter).");
    }

    ImGui::Separator();

    // We collect any jump target in this frame and apply it *after*
    // EndTable so the rendering loop isn't interleaved with mutations
    // of `records` (do_undo/do_redo don't actually invalidate the
    // vector — std::vector doesn't shrink on cursor moves — but it
    // keeps the control flow honest). `has_jump` is the only gate;
    // jump_target is only read when has_jump is true.
    std::size_t jump_target = 0;
    bool has_jump = false;

    // Tag-run grouping. Runs of consecutive edits carrying the same
    // non-empty `tag` field (e.g. all the [[edit]] entries a `ptm
    // import` writes with `tag = "ptm_import"`, or every cell-write a
    // workflow modal records under `tag = "fa24_swap"`) collapse to a
    // single chevron row by default once they exceed kCollapseAtN.
    // Expanded runs are tracked by the first record index so two
    // adjacent imports of the same tag stay independently
    // expandable. The set is panel-static so it survives panel
    // re-render but resets on session restart — a state-on-disk
    // version is a nice-to-have but not load-bearing.
    static std::unordered_set<std::size_t> s_expanded_runs;
    constexpr std::size_t kCollapseAtN = 10;
    struct TagRun {
        std::size_t start;
        std::size_t end; // one-past-last
        std::string tag;
        [[nodiscard]] std::size_t count() const noexcept { return end - start; }
    };
    std::vector<TagRun> tag_runs;
    for (std::size_t i = 0; i < records.size();) {
        auto const &r = records[i];
        if (r.tag.empty()) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < records.size() && records[j].tag == r.tag) {
            ++j;
        }
        if (j - i >= kCollapseAtN) {
            tag_runs.push_back({i, j, r.tag});
        }
        i = j;
    }
    // Helpers to map a record index to its containing tag-run and
    // whether that run is rendering collapsed this frame. Runs whose
    // filter-substring matches the tag string are force-expanded so
    // filter narrows correctly (otherwise a filter like "ptm_import"
    // would land on a collapsed header that hides the matched rows).
    auto const find_run = [&](std::size_t row_idx) -> TagRun const * {
        for (auto const &run : tag_runs) {
            if (row_idx >= run.start && row_idx < run.end) {
                return &run;
            }
        }
        return nullptr;
    };
    auto const is_collapsed = [&](TagRun const &run) {
        if (s_expanded_runs.contains(run.start)) {
            return false;
        }
        // Force-expand when the active filter matches the tag itself.
        if (!filter.empty() && icontains(run.tag, filter)) {
            return false;
        }
        return true;
    };

    if (ImGui::BeginTable("##history_tbl", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("table", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("op", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("flags", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::size_t matched = 0;
        for (std::size_t i = 0; i < records.size(); ++i) {
            // If this index is the start of a collapsed tag-run,
            // render a single header row that summarizes the whole
            // run, then skip to the row past the end. Mid-run indices
            // (i > run->start) inside a collapsed run are skipped
            // outright — they're folded under the header.
            if (auto const *run = find_run(i); run != nullptr && is_collapsed(*run)) {
                if (i == run->start) {
                    bool const run_cursor_inside = (cursor > run->start && cursor <= run->end);
                    bool const run_undone = (cursor <= run->start);
                    ImGui::TableNextRow();
                    // Index col: chevron + count, or cursor marker if
                    // cursor lands at the end of the run.
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(static_cast<int>(run->start) + 0x200000);
                    char chev_buf[16];
                    std::snprintf(chev_buf, sizeof chev_buf, "\xE2\x96\xB6 %zu",
                                  run->count());
                    if (ImGui::SmallButton(chev_buf)) {
                        s_expanded_runs.insert(run->start);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Expand this transaction (%zu edits, tag '%s').\n"
                                          "Click to show every individual edit.",
                                          run->count(), run->tag.c_str());
                    }
                    ImGui::PopID();
                    // Table col: tag name in accent color so it reads
                    // as a group, not as a regular table id.
                    ImGui::TableSetColumnIndex(1);
                    auto const acc = accent_for(current_theme());
                    ImGui::TextColored(acc.base, "[%s]", run->tag.c_str());
                    // Op col: short summary using the first row's
                    // description as representative, plus the bulk count.
                    ImGui::TableSetColumnIndex(2);
                    char opbuf[160];
                    auto const &head_desc = records[run->start].description;
                    std::snprintf(opbuf, sizeof opbuf, "%zu edits — first: %s",
                                  run->count(),
                                  head_desc.empty() ? "(no description)"
                                                    : head_desc.c_str());
                    if (run_undone) {
                        ImGui::TextDisabled("%s", opbuf);
                    } else {
                        ImGui::TextUnformatted(opbuf);
                    }
                    // Flags col: aggregate across the run. If any
                    // member touches an engine-safety / emissions
                    // table, surface the flag on the header.
                    ImGui::TableSetColumnIndex(3);
                    bool any_safety = false;
                    bool any_emissions = false;
                    for (std::size_t k = run->start; k < run->end; ++k) {
                        auto const *kte = records[k].as_table();
                        if (kte == nullptr) {
                            continue;
                        }
                        auto const *table = state.project->definition().find_table(kte->table_id);
                        if (table != nullptr) {
                            any_safety = any_safety || table->engine_safety_critical;
                            any_emissions = any_emissions || table->emissions_relevant;
                        }
                    }
                    bool had_flag = false;
                    if (any_safety) {
                        ImGui::TextColored(chip_fg_warn(), "S");
                        had_flag = true;
                    }
                    if (any_emissions) {
                        if (had_flag) {
                            ImGui::SameLine();
                        }
                        ImGui::TextColored(chip_fg_caution(), "E");
                        had_flag = true;
                    }
                    if (!had_flag) {
                        ImGui::TextDisabled("-");
                    }
                    // Action col: Jump to end-of-run. Lets the user
                    // walk the cursor across the entire transaction
                    // in one click — the underlying semantics match a
                    // batch revert of the workflow.
                    ImGui::TableSetColumnIndex(4);
                    ImGui::PushID(static_cast<int>(run->start) + 0x300000);
                    bool const at_end = (cursor == run->end);
                    if (at_end) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::SmallButton("Jump")) {
                        jump_target = run->end;
                        has_jump = true;
                    }
                    if (at_end) {
                        ImGui::EndDisabled();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Walk the cursor to the end of this transaction.\n"
                            "Use Ctrl+Z to step back across individual edits.");
                    }
                    ImGui::PopID();
                    if (run_cursor_inside) {
                        // The cursor is partway through the run.
                        // Tooltip-only signal lives on the chevron; a
                        // subtle "[partial]" suffix in the op column
                        // would be nicer but renders late — defer.
                        (void)run_cursor_inside;
                    }
                    ++matched;
                }
                // Either we just rendered the header, or we're at a
                // mid-run index; either way skip this iteration. The
                // for-loop will ++i to the next index, which will be
                // skipped again until we land past run->end.
                continue;
            }
            auto const &e = records[i];
            auto const *te = e.as_table();
            auto const *be = e.as_byte();
            // Per-row display strings differ between TableEdit (table id +
            // rect from the snapshot) and ByteEdit (synthetic label + byte
            // count). Pre-compute here so the column loops below don't
            // have to branch.
            std::string id_str = te != nullptr ? te->table_id : std::string{"(byte-edit)"};
            std::string rect_str;
            if (te != nullptr) {
                char rectbuf[32]{};
                std::snprintf(rectbuf, sizeof rectbuf, "[%zu:%zu, %zu:%zu]",
                              te->before.rect.r_start, te->before.rect.r_end,
                              te->before.rect.c_start, te->before.rect.c_end);
                rect_str = rectbuf;
            } else if (be != nullptr) {
                char rectbuf[32]{};
                std::snprintf(rectbuf, sizeof rectbuf, "%zu byte%s", be->changes.size(),
                              be->changes.size() == 1 ? "" : "s");
                rect_str = rectbuf;
            }
            if (!filter.empty() && !icontains(id_str, filter) &&
                !icontains(e.description, filter)) {
                continue;
            }
            ++matched;

            bool const is_undone = i >= cursor;
            bool const is_top = (i + 1 == cursor); // most-recent applied

            ImGui::TableNextRow();

            // Index column with a cursor marker. Accent color for the
            // "next to undo" row; muted text for already-undone rows.
            ImGui::TableSetColumnIndex(0);
            if (is_top) {
                ImGui::TextColored(chip_fg_info(), ">%zu", i);
            } else if (is_undone) {
                ImGui::TextDisabled(" %zu", i);
            } else {
                ImGui::Text(" %zu", i);
            }

            // Table-id cell. For TableEdit: clickable Selectable that
            // drives the main table view. For ByteEdit: a disabled label
            // (byte edits don't bind to any one table).
            ImGui::TableSetColumnIndex(1);
            if (is_undone) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            if (te != nullptr) {
                bool const is_selected = (te->table_id == state.selected_table_id);
                ImGui::PushID(static_cast<int>(i) + 0x100000);
                if (ImGui::Selectable(te->table_id.c_str(), is_selected)) {
                    state.select_table(te->table_id);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to open %s in the table view.", te->table_id.c_str());
                }
                ImGui::PopID();
            } else {
                ImGui::TextDisabled("%s", id_str.c_str());
            }
            if (is_undone) {
                ImGui::PopStyleColor();
            }

            // Op column = description plus the rect/size string.
            ImGui::TableSetColumnIndex(2);
            char opbuf[160]{};
            std::snprintf(opbuf, sizeof opbuf, "%s  %s",
                          e.description.empty() ? "(no description)" : e.description.c_str(),
                          rect_str.c_str());
            if (is_undone) {
                ImGui::TextDisabled("%s", opbuf);
            } else {
                ImGui::TextUnformatted(opbuf);
            }

            // Flags column: S (engine-safety) in red, E (emissions) in
            // amber, '-' if neither. Pulled from the live pack so a
            // pack swap re-classifies without rewriting history.
            // ByteEdits don't bind to a table, so '-'.
            ImGui::TableSetColumnIndex(3);
            bool had_flag = false;
            if (te != nullptr) {
                auto const *table = state.project->definition().find_table(te->table_id);
                if (table != nullptr) {
                    if (table->engine_safety_critical) {
                        ImGui::TextColored(chip_fg_warn(), "S");
                        had_flag = true;
                    }
                    if (table->emissions_relevant) {
                        if (had_flag)
                            ImGui::SameLine();
                        // E uses chip_fg_caution to match the sidebar's
                        // S/E badge convention (S = warn band, E = caution
                        // band). The history-panel E was previously the
                        // amber warn shade, drifting from the sidebar; the
                        // theme-aware migration is the right time to align.
                        ImGui::TextColored(chip_fg_caution(), "E");
                        had_flag = true;
                    }
                }
            }
            if (!had_flag)
                ImGui::TextDisabled("-");

            // Action column: Jump → make this edit the cursor (cursor = i + 1).
            // No-op when already at top: disable to remove the temptation.
            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(static_cast<int>(i));
            if (is_top)
                ImGui::BeginDisabled();
            if (ImGui::SmallButton("Jump")) {
                jump_target = i + 1;
                has_jump = true;
            }
            if (is_top)
                ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Walk the cursor to make this edit the "
                                  "most-recently-applied. Reversible.");
            }
            ImGui::PopID();
        }

        ImGui::EndTable();

        if (!filter.empty()) {
            ImGui::TextDisabled("Showing %zu of %zu.", matched, records.size());
        }
    }

    ImGui::Spacing();
    // If any tag-runs are currently expanded, offer a one-click
    // re-collapse so the user doesn't have to scroll to find each
    // group. Two-buttons-when-applicable beats a hidden affordance.
    bool any_run_expanded = false;
    for (auto const &run : tag_runs) {
        if (s_expanded_runs.contains(run.start)) {
            any_run_expanded = true;
            break;
        }
    }
    if (any_run_expanded) {
        if (ImGui::SmallButton("Collapse expanded groups")) {
            for (auto const &run : tag_runs) {
                s_expanded_runs.erase(run.start);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Re-fold every currently-expanded transaction group.");
        }
        ImGui::SameLine();
    }
    ImGui::TextDisabled("Flags: S=engine-safety, E=emissions, -=neither.");

    // Apply pending cursor moves AFTER the table is closed. Each step
    // re-uses do_undo/do_redo so dirty/status/current_table_data stay
    // coherent. If a step fails (apply_history_step's rollback re-bumps
    // the cursor back to where it started), the loop detects the lack
    // of progress and bails — no infinite spin.
    if (wants_revert_all) {
        jump_target = 0;
        has_jump = true;
    }
    if (wants_reapply_all) {
        jump_target = records.size();
        has_jump = true;
    }

    if (has_jump) {
        std::size_t prev = static_cast<std::size_t>(-1);
        while (true) {
            std::size_t const c = state.project->active_history().cursor();
            if (c == jump_target)
                break;
            if (c == prev)
                break; // step didn't move the cursor → stop.
            prev = c;
            if (c > jump_target)
                do_undo(state);
            else
                do_redo(state);
        }
    }

    ImGui::End();
}

} // namespace st::ui
