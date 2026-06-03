// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// DTCs panel — GUI mirror of CLI's `project-{enable,disable}-dtc`: each
// toggle writes the bit through `st::set_dtc_enabled` and records the
// byte change as a ByteEdit in `edit::History` so Ctrl+Z restores the
// original bit pattern. Emissions-flagged codes get a yellow "E" chip;
// the flash-time policy gate still enforces the jurisdiction profile.

#include "panels/panels.hpp"

#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/audit.hpp"
#include "st/defs.hpp"
#include "st/edit.hpp"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::ui {

void render_dtcs_panel(AppState &state) {
    if (!state.show_dtcs_panel) {
        return;
    }
    if (!ImGui::Begin("DTCs", &state.show_dtcs_panel)) {
        ImGui::End();
        return;
    }
    if (!state.project.has_value()) {
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to view its declared DTCs and toggle them.");
        ImGui::End();
        return;
    }
    auto const &def = state.project->definition();
    if (def.dtcs().empty()) {
        render_empty_state(
            "No DTCs in this pack",
            "Toggleable DTCs need pack-side support — see docs/11-definition-format.md.");
        ImGui::End();
        return;
    }

    std::size_t emissions_total = 0;
    for (auto const &d : def.dtcs()) {
        if (d.emissions_relevant)
            ++emissions_total;
    }
    text_subtle("%zu DTC(s), %zu emissions-flagged", def.dtcs().size(), emissions_total);
    glossary_tooltip_for(state, "DTC");

    // Issue #10: DTC reads + writes target working_rom. When a non-
    // working ROM is active the bulk row + per-row checkbox lock out;
    // the list still renders so the user can see what the WORKING
    // tune has set, just read-only against accidental mutation. A
    // future polish could swap the read side to view_rom and label
    // the panel "viewing source DTCs" when applicable.
    bool const editing_allowed = state.viewing_working_rom();
    if (!editing_allowed) {
        ImGui::TextDisabled(
            "(read-only — switch View → Active ROM → Working to toggle)");
    }
    ImGui::BeginDisabled(!editing_allowed);

    // Bulk-toggle row — covers the two common workflows:
    //   "Disable all emissions"  — emissions-delete; rolls all
    //                              emissions-flagged DTCs OFF in one
    //                              ByteEdit so Ctrl+Z reverses the
    //                              entire batch.
    //   "Enable all"             — restore to "everything reporting"
    //                              (factory-ish state).
    auto const bulk_toggle = [&](char const *label, bool emissions_only,
                                 bool enable, char const *desc_prefix) {
        std::vector<st::edit::ByteEdit::Change> changes;
        std::vector<std::string> codes;
        codes.reserve(def.dtcs().size());
        for (auto const &d : def.dtcs()) {
            if (emissions_only && !d.emissions_relevant)
                continue;
            auto const *bm = def.find_dtc_bitmap(d.bitmap_id);
            if (bm == nullptr)
                continue;
            auto const cur = st::is_dtc_enabled(state.project->working_rom(), *bm, d);
            if (!cur.has_value())
                continue;
            if (*cur == enable)
                continue; // already in the desired state — skip
            auto change = st::set_dtc_enabled(state.project->working_rom(), *bm, d, enable);
            if (!change.has_value())
                continue;
            if (change->before != change->after) {
                changes.push_back({change->address, change->before, change->after});
                codes.push_back(d.code);
            }
        }
        if (changes.empty()) {
            enqueue_toast(state, ToastKind::Info,
                          std::string{label} + ": nothing to change.");
            return;
        }
        std::string desc{desc_prefix};
        desc += " (" + std::to_string(codes.size()) + " DTC";
        if (codes.size() != 1)
            desc += "s";
        desc += ")";
        state.project->history().record(
            st::edit::Edit::bytes(std::move(changes), std::move(desc)));
        state.dirty = true;
        state.status_msg = std::string{label} + ": " +
                           std::to_string(codes.size()) + " DTC(s) toggled.";
        if (state.audit_log.has_value()) {
            (void)state.audit_log->log(
                st::audit::EntryKind::EditCommitted, "ui.dtcs",
                std::string{desc_prefix},
                {{"count", std::to_string(codes.size())},
                 {"scope", emissions_only ? "emissions" : "all"}});
        }
    };
    if (ImGui::SmallButton("Disable all emissions")) {
        bulk_toggle("Disable all emissions", /*emissions_only=*/true, /*enable=*/false,
                    "disable all emissions DTCs");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Roll every emissions-flagged DTC to OFF in one\n"
                          "history step. Ctrl+Z reverses the whole batch.\n"
                          "Common emissions-delete workflow.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Enable all")) {
        bulk_toggle("Enable all", /*emissions_only=*/false, /*enable=*/true,
                    "enable all DTCs");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Roll every DTC to ON in one history step.\n"
                          "Useful to restore factory-ish reporting state.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Disable all")) {
        bulk_toggle("Disable all", /*emissions_only=*/false, /*enable=*/false,
                    "disable all DTCs");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Roll every DTC to OFF in one history step.\n"
                          "Aggressive — silences EVERY trouble code,\n"
                          "not just emissions.");
    }

    ImGui::Separator();

    // Filter input — substring against the code or name. Same shape as the
    // sidebar's table filter but no global Ctrl+F focus binding (DTCs are
    // a secondary surface).
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##dtc_filter", "Filter DTCs…", state.dtc_filter,
                             sizeof state.dtc_filter, ImGuiInputTextFlags_EscapeClearsAll);
    std::string_view const filter{state.dtc_filter};

    auto const &rom = state.project->working_rom();
    if (ImGui::BeginTable("dtc_table", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::size_t shown = 0;
        for (auto const &d : def.dtcs()) {
            if (!filter.empty() && !icontains(d.code, filter) && !icontains(d.name, filter)) {
                continue;
            }
            ++shown;
            auto const *bm = def.find_dtc_bitmap(d.bitmap_id);
            if (bm == nullptr) {
                // Validation should have caught this at load time, but if it
                // somehow snuck through, render a disabled row rather than
                // crashing on the bit read.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", d.code.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("(broken bitmap reference '%s')", d.bitmap_id.c_str());
                continue;
            }
            auto const enabled_r = st::is_dtc_enabled(rom, *bm, d);
            bool enabled = enabled_r.has_value() ? *enabled_r : true;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(d.code.c_str());
            bool toggled = enabled;
            if (ImGui::Checkbox("##en", &toggled) && toggled != enabled) {
                auto change = st::set_dtc_enabled(state.project->working_rom(), *bm, d, toggled);
                if (!change.has_value()) {
                    state.status_msg = "DTC toggle failed: " + change.error().to_string();
                } else if (change->before != change->after) {
                    // Record one ByteEdit per toggle so Ctrl+Z reverses
                    // each gesture individually. Coalescing into a batch
                    // (like the CLI does for a `--code A,B,C` list) would
                    // need a debounce and surface less clearly in the
                    // history panel — one bit per click is the GUI
                    // semantic users expect.
                    std::string desc = (toggled ? "enable DTC " : "disable DTC ") + d.code;
                    state.project->history().record(st::edit::Edit::bytes(
                        {{change->address, change->before, change->after}}, std::move(desc)));
                    state.dirty = true;
                    state.status_msg = (toggled ? "Enabled " : "Disabled ") + d.code;
                }
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(d.code.c_str());
            if (d.emissions_relevant) {
                ImGui::SameLine();
                ImGui::TextColored(chip_fg_caution(), "E");
            }

            ImGui::TableSetColumnIndex(2);
            if (d.name.empty()) {
                text_subtle("(no name)");
            } else {
                ImGui::TextUnformatted(d.name.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(d.code.c_str());
                if (!d.name.empty()) {
                    text_subtle("%s", d.name.c_str());
                }
                ImGui::Separator();
                ImGui::Text("Bitmap:    %s", d.bitmap_id.c_str());
                ImGui::Text("Address:   0x%08zX + %zu", bm->address, d.byte_offset);
                ImGui::Text("Bit:       %d", d.bit);
                if (d.emissions_relevant) {
                    ImGui::TextColored(chip_fg_caution(), "emissions-relevant");
                }
                ImGui::EndTooltip();
            }
        }
        ImGui::EndTable();

        if (!filter.empty()) {
            text_subtle("Showing %zu of %zu.", shown, def.dtcs().size());
        }
    }

    ImGui::Spacing();
    text_subtle("Toggles record as ByteEdits; Ctrl+Z reverses each.");
    ImGui::EndDisabled();
    ImGui::End();
}

} // namespace st::ui
