// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Compare panel — GUI for the structured ROM diff workflow shipped
// in st::diff (and the CLI `diff` subcommand). Two-pane synchronized
// tree per analyst Issue #4 / docs/33:
//
//   - Toolbar: ROM A + ROM B file pickers, epsilon slider, include-
//     identical toggle, Compare button. ROM A defaults to the open
//     project's source.bin when the panel first opens against a
//     loaded project.
//   - Summary line: pack id, tables compared/changed, cells changed.
//   - Changed-tables list: each row shows [S][E] safety/emissions
//     chips + table name + cells_changed/total + max/mean |Δ|.
//     Click a row to expand the per-cell change list. Click "Open in
//     editor" to jump to the table via state.select_table().
//   - Skipped-tables list at the bottom (errors per-table).
//
// The DiffSet is recomputed on demand (Compare button); cached on
// state.compare_result for re-render without recompute. Same epsilon
// + include-identical knobs as the CLI; uses st::diff::compare under
// the hood so the panel and CLI share one source of truth.

#include "panels/panels.hpp"

#include "actions.hpp" // jump_to_table
#include "app_state.hpp"
#include "widgets/widgets.hpp"

#include "st/diff.hpp"
#include "st/rom.hpp"

#include <imgui.h>
#include <nfd.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

namespace {

// Sentinel string for ROM A — when the path equals this, the compare
// uses the open project's in-memory source ROM rather than reading
// from disk. Saves the user from typing the project's source.bin
// path; works even when the project's source file has been renamed
// or moved relative to the .stune dir.
inline constexpr char const *kProjectSourceSentinel = "<project source>";
inline constexpr char const *kProjectWorkingSentinel = "<project working>";
inline constexpr char const *kProjectRomSentinelPrefix = "<project:";

// Resolve a ROM path or sentinel against the active project. Returns
// pointer-to-Rom for an in-memory source / working / additional-rom
// reference, or nullopt (with a loaded Rom in `out_loaded`) for a
// disk path. Failure populates `error_msg` and returns nullopt-with-no-out.
struct RomResolution {
    Rom const *in_memory{nullptr};
    std::optional<Rom> loaded;
    bool ok{false};
};

RomResolution resolve_rom_input(AppState const &state, char const *path,
                                std::string &error_msg, char const *label) {
    RomResolution out;
    std::string_view const sv{path};
    if (sv.empty() || sv == kProjectSourceSentinel) {
        if (!state.project.has_value()) {
            error_msg = std::string{label} + ": no project loaded.";
            return out;
        }
        out.in_memory = &state.project->source_rom();
        out.ok = true;
        return out;
    }
    if (sv == kProjectWorkingSentinel) {
        if (!state.project.has_value()) {
            error_msg = std::string{label} + ": no project loaded.";
            return out;
        }
        out.in_memory = &state.project->working_rom();
        out.ok = true;
        return out;
    }
    if (sv.starts_with(kProjectRomSentinelPrefix) && sv.ends_with(">")) {
        if (!state.project.has_value()) {
            error_msg = std::string{label} + ": no project loaded.";
            return out;
        }
        std::string_view const id =
            sv.substr(std::strlen(kProjectRomSentinelPrefix),
                      sv.size() - std::strlen(kProjectRomSentinelPrefix) - 1);
        for (auto const &r : state.project->additional_roms()) {
            if (r.id == id) {
                out.in_memory = &r.rom;
                out.ok = true;
                return out;
            }
        }
        error_msg = std::string{label} + ": project has no [[rom]] entry id '" +
                    std::string{id} + "'.";
        return out;
    }
    auto loaded = st::Rom::from_file(std::filesystem::path{path});
    if (!loaded.has_value()) {
        error_msg = std::string{label} + ": " + loaded.error().to_string();
        return out;
    }
    out.loaded = std::move(*loaded);
    out.ok = true;
    return out;
}

// Run the compare; populate state.compare_result on success or
// compare_error_msg on failure. ROM A and ROM B both support sentinel
// strings (<project source>, <project working>, <project:rom_id>) in
// addition to plain disk paths.
void recompute_compare(AppState &state) {
    state.compare_error_msg.clear();
    state.compare_result.reset();

    if (!state.project.has_value()) {
        state.compare_error_msg = "No project open — Compare needs a loaded "
                                  "definition pack to drive the table diff.";
        return;
    }
    if (state.compare_rom_b_path[0] == '\0') {
        state.compare_error_msg = "Pick ROM B before running Compare.";
        return;
    }

    auto rom_a = resolve_rom_input(state, state.compare_rom_a_path,
                                   state.compare_error_msg, "ROM A");
    if (!rom_a.ok)
        return;
    auto rom_b = resolve_rom_input(state, state.compare_rom_b_path,
                                   state.compare_error_msg, "ROM B");
    if (!rom_b.ok)
        return;

    Rom const &rom_a_ref = rom_a.in_memory ? *rom_a.in_memory : *rom_a.loaded;
    Rom const &rom_b_ref = rom_b.in_memory ? *rom_b.in_memory : *rom_b.loaded;

    st::diff::Options opts;
    opts.cell_epsilon = static_cast<double>(state.compare_epsilon);
    opts.include_identical = state.compare_include_identical;
    auto result = st::diff::compare(rom_a_ref, rom_b_ref, state.project->definition(), opts);
    if (!result.has_value()) {
        state.compare_error_msg = result.error().to_string();
        return;
    }
    state.compare_result = std::move(*result);
}

// Open a "pick ROM" dialog and copy the chosen path into `out`. NFD's
// filter list takes a name + comma-separated extension list.
void pick_rom_into(char *out, std::size_t out_size, std::string &error_msg) {
    nfdu8filteritem_t filters[2] = {{"ROM image", "bin"}, {"Hex / SREC", "hex,srec,mot"}};
    NFD::UniquePathU8 picked;
    nfdresult_t const r = NFD::OpenDialog(picked, filters, 2);
    if (r == NFD_OKAY) {
        std::snprintf(out, out_size, "%s", picked.get());
        error_msg.clear();
    } else if (r == NFD_ERROR) {
        error_msg = std::string{"Open dialog error: "} + NFD::GetError();
    }
}

// Color the headline value of cells_changed by intensity: green if
// zero changes, amber otherwise (engine-safety tables always promoted
// to danger color regardless).
ImVec4 changed_count_color(std::size_t changed, bool is_safety) {
    if (is_safety && changed > 0)
        return chip_fg_danger();
    if (changed == 0)
        return chip_fg_ok();
    return chip_fg_caution();
}

// Render one table row in the changed-tables tree. Returns true when
// the user clicked the "Open in editor" action.
[[nodiscard]] bool render_table_row(AppState &state, st::diff::TableDelta const &t) {
    ImGui::PushID(t.table_id.c_str());

    bool open_in_editor = false;

    // Safety / emissions chips — small badges in front of the name.
    if (t.engine_safety_critical) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("[S]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (t.emissions_relevant) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
        ImGui::TextUnformatted("[E]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    // Expandable tree node for the table.
    bool const was_expanded =
        state.compare_expanded_tables.find(t.table_id) != state.compare_expanded_tables.end();
    ImGui::SetNextItemOpen(was_expanded, ImGuiCond_Always);
    bool const is_open = ImGui::TreeNode(t.table_id.c_str(), "%s  (%s)", t.table_name.c_str(),
                                         t.table_id.c_str());
    bool const now_expanded = is_open;
    if (now_expanded && !was_expanded) {
        state.compare_expanded_tables.insert(t.table_id);
    } else if (!now_expanded && was_expanded) {
        state.compare_expanded_tables.erase(t.table_id);
    }

    // Cells-changed badge — colored by zone.
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,
                         changed_count_color(t.cells_changed, t.engine_safety_critical));
    ImGui::Text("    %zu/%zu cells   max |Δ|=%.3g   mean |Δ|=%.3g", t.cells_changed,
                t.total_cells, t.max_abs_delta, t.mean_abs_delta);
    ImGui::PopStyleColor();

    if (is_open) {
        // Open-in-editor action — jumps to the table in the side panel
        // + central editor.
        if (ImGui::Button("\xEE\x9C\xA9  Open in editor")) { // E709 Edit
            open_in_editor = true;
        }
        ImGui::SameLine();
        text_subtle("(click to load this table in the Table viewer)");

        // Per-cell change list. Cap visible rows for huge tables —
        // ImPlot tables can render thousands but the user rarely
        // scans more than 50; offer a "show all N" toggle later.
        if (!t.changes.empty()) {
            constexpr std::size_t kMaxCellRowsDefault = 50;
            auto const total = t.changes.size();
            auto const show = std::min<std::size_t>(total, kMaxCellRowsDefault);

            if (ImGui::BeginTable("##cell_changes", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 180.0f))) {
                ImGui::TableSetupColumn("row");
                ImGui::TableSetupColumn("col");
                ImGui::TableSetupColumn("A");
                ImGui::TableSetupColumn("B");
                ImGui::TableSetupColumn("Δ");
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < show; ++i) {
                    auto const &c = t.changes[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", c.row);
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", c.col);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.4g", c.value_a);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.4g", c.value_b);
                    ImGui::TableNextColumn();
                    // Color delta by sign for at-a-glance scanning.
                    double const d = c.delta();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          d > 0 ? chip_fg_caution() : chip_fg_ok());
                    ImGui::Text("%+.4g", d);
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
            if (show < total) {
                text_subtle("Showing first %zu of %zu changes — open the JSON / CSV "
                            "export via `subuwutuner-cli diff --format` for the full "
                            "list.",
                            show, total);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
    return open_in_editor;
}

} // namespace

void render_compare_panel(AppState &state) {
    if (!state.show_compare_panel) {
        return;
    }
    if (ImGuiID const central = central_dock_node_id(); central != 0) {
        ImGui::SetNextWindowDockID(central, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Compare###Compare (Preview)", &state.show_compare_panel)) {
        ImGui::End();
        return;
    }

    preview_pill();
    ImGui::SameLine();
    text_subtle("Structured ROM diff via st::diff. Picks per-table cell-level "
                "changes against the open project's pack. Same engine as the "
                "`subuwutuner-cli diff` subcommand. See docs/33 + analyst Issue #4.");
    ImGui::Separator();

    // ROM A — default to the in-memory project source on first open.
    // The sentinel string keeps the input field readable and signals
    // to recompute_compare() to skip the file read.
    if (state.compare_rom_a_path[0] == '\0' && state.project.has_value()) {
        std::snprintf(state.compare_rom_a_path, sizeof state.compare_rom_a_path, "%s",
                      kProjectSourceSentinel);
    }
    // ROM B — when the user is viewing an additional ROM (Issue #10)
    // and B is empty, pre-fill with that ROM so opening Compare is a
    // zero-click "what's different about this tune vs source" view.
    // Only fires once; user can clear / overwrite freely.
    if (state.compare_rom_b_path[0] == '\0' && state.project.has_value() &&
        !state.active_rom_id.empty() && state.active_rom_id != "working" &&
        state.active_rom_id != "source") {
        std::string const sentinel = std::string{kProjectRomSentinelPrefix} +
                                     state.active_rom_id + ">";
        std::snprintf(state.compare_rom_b_path, sizeof state.compare_rom_b_path, "%s",
                      sentinel.c_str());
    }

    ImGui::Text("ROM A (baseline):");
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("##cmp_rom_a", state.compare_rom_a_path,
                     sizeof state.compare_rom_a_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cmp_a")) {
        pick_rom_into(state.compare_rom_a_path, sizeof state.compare_rom_a_path,
                      state.compare_error_msg);
    }

    ImGui::Text("ROM B (compare):");
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("##cmp_rom_b", state.compare_rom_b_path,
                     sizeof state.compare_rom_b_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cmp_b")) {
        pick_rom_into(state.compare_rom_b_path, sizeof state.compare_rom_b_path,
                      state.compare_error_msg);
    }

    // Swap A ↔ B. Convenience over retyping both fields when the user
    // realizes they had the diff inverted (positive delta vs negative
    // depends on which way you point the compare). U+21C4 ⇄ Rightwards
    // Arrow Over Leftwards Arrow.
    if (ImGui::Button("\xE2\x87\x84  Swap A \xE2\x86\x94 B", ImVec2(160.0f, 0.0f))) {
        char tmp[sizeof state.compare_rom_a_path];
        std::snprintf(tmp, sizeof tmp, "%s", state.compare_rom_a_path);
        std::snprintf(state.compare_rom_a_path, sizeof state.compare_rom_a_path, "%s",
                      state.compare_rom_b_path);
        std::snprintf(state.compare_rom_b_path, sizeof state.compare_rom_b_path, "%s", tmp);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Swap the contents of the ROM A and ROM B fields.\n"
                          "Useful when the diff sign reads the wrong way around.");
    }

    // Project ROMs (Issue #10 read slice). Lists the project's source +
    // working + any [[rom]] entries from project.toml. Click → A / → B
    // wires the sentinel into the corresponding path field so the
    // compare reads the in-memory bytes instead of round-tripping
    // through disk. Hidden when no project is loaded (else there's
    // nothing to pick from).
    if (state.project.has_value()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        text_subtle("Project ROMs — click an arrow button to slot a project ROM into A or B "
                    "without leaving the project dir.");
        // Surface any project-toml warnings — [[rom]] entries that
        // were declared but couldn't load. Better than silently
        // dropping; user can fix the path or remove the entry.
        for (auto const &w : state.project->additional_rom_warnings()) {
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
            ImGui::TextWrapped("%s", w.c_str());
            ImGui::PopStyleColor();
        }
        auto const row_btns = [&](char const *label, char const *sentinel, char const *id_suffix) {
            ImGui::PushID(id_suffix);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            if (ImGui::SmallButton("\xE2\x86\x92 A")) {
                std::snprintf(state.compare_rom_a_path, sizeof state.compare_rom_a_path, "%s",
                              sentinel);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("\xE2\x86\x92 B")) {
                std::snprintf(state.compare_rom_b_path, sizeof state.compare_rom_b_path, "%s",
                              sentinel);
            }
            ImGui::PopID();
        };
        row_btns("Project source (read-only)", kProjectSourceSentinel, "src");
        row_btns("Project working (with edits)", kProjectWorkingSentinel, "wrk");
        for (auto const &r : state.project->additional_roms()) {
            std::string const sentinel = std::string{"<project:"} + r.id + ">";
            char id_suffix[96];
            std::snprintf(id_suffix, sizeof id_suffix, "ar_%s", r.id.c_str());
            ImGui::PushID(id_suffix);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", r.display_name.empty() ? r.id.c_str() : r.display_name.c_str());
            if (!r.id.empty()) {
                ImGui::SameLine();
                text_subtle("(%s)", r.id.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("\xE2\x86\x92 A")) {
                std::snprintf(state.compare_rom_a_path, sizeof state.compare_rom_a_path, "%s",
                              sentinel.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("\xE2\x86\x92 B")) {
                std::snprintf(state.compare_rom_b_path, sizeof state.compare_rom_b_path, "%s",
                              sentinel.c_str());
            }
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Cell epsilon", &state.compare_epsilon, 0.0f, 10.0f, "%.3f");
    ImGui::SameLine();
    ImGui::Checkbox("Show identical tables", &state.compare_include_identical);
    ImGui::SameLine();
    // Compare button — pinned to the right for thumb reach.
    if (ImGui::Button("\xEE\x9C\xA0  Compare", ImVec2(120.0f, 0.0f))) { // E700 GlobalNav
        recompute_compare(state);
    }
    ImGui::SameLine();
    // Export Markdown — companion to the CSV exporter. Markdown is the
    // shareable format for forum posts and PR descriptions. Rendered
    // inline here rather than in st::diff because the shape is
    // UI-presentation, not a library concern. Shared renderer used
    // by both Export (file) and Copy (clipboard).
    auto const render_diff_markdown = [](st::diff::DiffSet const &d) -> std::string {
        std::ostringstream ss;
        ss << "# ROM diff\n\n";
        ss << "- **Pack**: `" << d.pack_id << "`\n";
        ss << "- **Tables compared**: " << d.tables_compared
           << " (" << d.tables_changed << " changed)\n";
        ss << "- **Cells changed**: " << d.total_cells_changed
           << " of " << d.total_cells_compared << "\n";
        ss << "- **ROM A CRC32**: `0x" << std::hex << d.rom_a_crc32 << std::dec << "`\n";
        ss << "- **ROM B CRC32**: `0x" << std::hex << d.rom_b_crc32 << std::dec << "`\n\n";
        if (d.identical()) {
            ss << "_All tables identical between ROM A and ROM B._\n";
        } else {
            ss << "## Changed tables\n\n";
            for (auto const &t : d.tables) {
                if (!t.changed())
                    continue;
                ss << "### `" << t.table_id << "`";
                if (!t.table_name.empty() && t.table_name != t.table_id)
                    ss << " — " << t.table_name;
                ss << "\n\n";
                if (t.engine_safety_critical)
                    ss << "> **Engine-safety critical.**\n\n";
                if (t.emissions_relevant)
                    ss << "> Emissions-relevant table.\n\n";
                ss << t.cells_changed << " of " << t.total_cells
                   << " cells changed. ";
                char stats_buf[128];
                std::snprintf(stats_buf, sizeof stats_buf,
                              "max |Δ| = %.6g, mean |Δ| = %.6g.\n\n",
                              t.max_abs_delta, t.mean_abs_delta);
                ss << stats_buf;
                if (!t.changes.empty()) {
                    ss << "| Row | Col | A | B | Δ |\n";
                    ss << "|---:|---:|---:|---:|---:|\n";
                    for (auto const &c : t.changes) {
                        char row_buf[256];
                        std::snprintf(row_buf, sizeof row_buf,
                                      "| %zu | %zu | %g | %g | %+g |\n",
                                      c.row, c.col, c.value_a, c.value_b,
                                      c.delta());
                        ss << row_buf;
                    }
                    ss << "\n";
                }
            }
        }
        if (!d.skipped.empty()) {
            ss << "## Skipped tables\n\n";
            for (auto const &s : d.skipped) {
                ss << "- `" << s.table_id << "` — " << s.reason << "\n";
            }
        }
        return std::move(ss).str();
    };

    ImGui::BeginDisabled(!state.compare_result.has_value());
    if (ImGui::Button("Export Markdown…", ImVec2(150.0f, 0.0f))) {
        NFD::UniquePath out;
        nfdfilteritem_t const filters[] = {{"Markdown", "md"}};
        nfdresult_t const r =
            NFD::SaveDialog(out, filters, 1, nullptr, "rom-diff.md");
        if (r == NFD_OKAY) {
            std::filesystem::path const target{out.get()};
            std::ofstream fh{target, std::ios::binary};
            if (!fh) {
                state.compare_error_msg = "Export Markdown: cannot open " + target.string();
            } else {
                fh << render_diff_markdown(*state.compare_result);
                if (!fh) {
                    state.compare_error_msg = "Export Markdown: write failed";
                } else {
                    enqueue_toast(state, ToastKind::Success,
                                  "Wrote " + target.string());
                }
            }
        } else if (r == NFD_ERROR) {
            state.compare_error_msg =
                std::string{"Export Markdown dialog error: "} + NFD::GetError();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.compare_result.has_value()) {
            ImGui::SetTooltip(
                "Write a shareable Markdown summary of the diff.\n"
                "Headings + tables per changed entry — paste into a\n"
                "PR description, forum post, or support thread.");
        } else {
            ImGui::SetTooltip("Run Compare first to enable export.");
        }
    }
    ImGui::SameLine();
    // Copy the same Markdown body to clipboard — no file dialog.
    // Quick path for the user who wants to paste straight into chat.
    ImGui::BeginDisabled(!state.compare_result.has_value());
    if (ImGui::Button("Copy MD", ImVec2(90.0f, 0.0f))) {
        auto const md = render_diff_markdown(*state.compare_result);
        ImGui::SetClipboardText(md.c_str());
        enqueue_toast(state, ToastKind::Success,
                      "Copied Markdown diff to clipboard.");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.compare_result.has_value()) {
            ImGui::SetTooltip("Copy the Markdown body to clipboard — same\n"
                              "shape as Export Markdown, no file dialog.\n"
                              "Quick path for chat / Slack / forum paste.");
        } else {
            ImGui::SetTooltip("Run Compare first to enable.");
        }
    }
    ImGui::SameLine();
    // Export CSV — write the cached compare_result to a file. Useful
    // for sharing tune diffs in PRs, attaching to support threads,
    // or feeding into downstream analysis. Uses st::diff::render_csv
    // so the wire format matches `subuwutuner-cli diff --format csv`.
    ImGui::BeginDisabled(!state.compare_result.has_value());
    if (ImGui::Button("Export CSV…", ImVec2(120.0f, 0.0f))) {
        NFD::UniquePath out;
        nfdfilteritem_t const filters[] = {{"CSV", "csv"}};
        nfdresult_t const r =
            NFD::SaveDialog(out, filters, 1, nullptr, "rom-diff.csv");
        if (r == NFD_OKAY) {
            std::filesystem::path const target{out.get()};
            std::ofstream fh{target, std::ios::binary};
            if (!fh) {
                state.compare_error_msg = "Export CSV: cannot open " + target.string();
            } else {
                auto const csv = st::diff::render_csv(*state.compare_result,
                                                      state.compare_include_identical);
                fh << csv;
                if (!fh) {
                    state.compare_error_msg = "Export CSV: write failed";
                } else {
                    enqueue_toast(state, ToastKind::Success,
                                  "Wrote " + target.string());
                }
            }
        } else if (r == NFD_ERROR) {
            state.compare_error_msg =
                std::string{"Export CSV dialog error: "} + NFD::GetError();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.compare_result.has_value()) {
            ImGui::SetTooltip("Write the current diff to a CSV file.\n"
                              "Same format as `subuwutuner-cli diff --format csv`.");
        } else {
            ImGui::SetTooltip("Run Compare first to enable export.");
        }
    }

    if (!state.compare_error_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.compare_error_msg.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (!state.compare_result.has_value()) {
        text_subtle("No comparison yet — pick two ROMs and click Compare.");
        ImGui::End();
        return;
    }

    // Jump to first change — finds the changed table with the most
    // cell deltas (after the active chip filter) and calls
    // jump_to_table on it. Ctrl+G binding triggers the same path so
    // keyboard-driven users can sweep the diff list without scrolling.
    auto const jump_to_biggest = [&]() {
        auto const &d = *state.compare_result;
        st::diff::TableDelta const *winner = nullptr;
        std::string_view const chip{state.compare_filter_chip};
        for (auto const &t : d.tables) {
            if (!t.changed())
                continue;
            if (!chip.empty()) {
                bool pass = false;
                if (chip == "@safety") {
                    pass = t.engine_safety_critical;
                } else if (chip == "@emissions") {
                    pass = t.emissions_relevant;
                } else if (chip == "@flagged") {
                    pass = t.engine_safety_critical || t.emissions_relevant;
                } else if (state.project.has_value()) {
                    auto const *tbl =
                        state.project->definition().find_table(t.table_id);
                    pass = (tbl != nullptr && tbl->category == chip);
                }
                if (!pass)
                    continue;
            }
            if (winner == nullptr || t.cells_changed > winner->cells_changed) {
                winner = &t;
            }
        }
        if (winner != nullptr) {
            jump_to_table(state, winner->table_id);
        }
    };
    if (ImGui::Button("\xEE\xAE\x86  Jump to biggest change", ImVec2(220.0f, 0.0f))) {
        jump_to_biggest();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open the most-changed table in the editor.\n"
                          "Honors the active chip filter.  (Ctrl+G)");
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) {
        jump_to_biggest();
    }
    ImGui::SameLine();
    text_subtle("Ctrl+G");

    auto const &d = *state.compare_result;

    // Summary line.
    ImGui::Text("Pack: %s    Tables: %zu compared / %zu changed    Cells: %zu / %zu changed",
                d.pack_id.c_str(), d.tables_compared, d.tables_changed,
                d.total_cells_changed, d.total_cells_compared);
    if (d.identical()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
        ImGui::TextUnformatted("Identical — every table compared equal.");
        ImGui::PopStyleColor();
    }

    // Changed-tables tree.
    if (!d.tables.empty()) {
        ImGui::Spacing();

        // Filter chips. @safety / @emissions / @flagged are always
        // surfaced because they're universally meaningful. Category
        // chips are derived from changed-tables' Table::category in
        // the loaded pack — so a project's actual table categorization
        // drives the chip set without code changes.
        auto const chip_button = [&](char const *label, char const *val) {
            bool const active = (std::string_view{state.compare_filter_chip} == val);
            if (active) {
                push_primary_button_colors();
            }
            ImGui::PushID(val);
            if (ImGui::SmallButton(label)) {
                if (active) {
                    state.compare_filter_chip[0] = '\0';
                } else {
                    std::snprintf(state.compare_filter_chip,
                                  sizeof state.compare_filter_chip, "%s", val);
                }
            }
            ImGui::PopID();
            if (active) {
                pop_primary_button_colors();
            }
            ImGui::SameLine();
        };
        chip_button("All", "");
        chip_button("Safety-critical", "@safety");
        chip_button("Emissions", "@emissions");
        chip_button("Either flag", "@flagged");
        // Category chips — one per distinct Table::category present in
        // the changed tables (looked up against the live pack so a pack
        // swap reflows them).
        std::vector<std::string_view> categories;
        if (state.project.has_value()) {
            auto const &def = state.project->definition();
            for (auto const &td : d.tables) {
                if (!td.changed() && !state.compare_include_identical)
                    continue;
                auto const *tbl = def.find_table(td.table_id);
                if (tbl == nullptr || tbl->category.empty())
                    continue;
                bool dup = false;
                for (auto c : categories) {
                    if (c == tbl->category) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    categories.push_back(tbl->category);
                }
            }
            std::sort(categories.begin(), categories.end());
        }
        for (auto cat : categories) {
            std::string const cat_str{cat};
            chip_button(cat_str.c_str(), cat_str.c_str());
        }
        ImGui::NewLine();

        // Sort by cells_changed desc so the biggest changes float to
        // the top; ties broken by table_id for stability across renders.
        std::vector<st::diff::TableDelta const *> sorted;
        sorted.reserve(d.tables.size());
        for (auto const &t : d.tables) {
            sorted.push_back(&t);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](st::diff::TableDelta const *x, st::diff::TableDelta const *y) {
                      if (x->cells_changed != y->cells_changed)
                          return x->cells_changed > y->cells_changed;
                      return x->table_id < y->table_id;
                  });

        std::string open_id;
        std::string_view const chip{state.compare_filter_chip};
        std::size_t shown = 0;
        for (auto const *t : sorted) {
            // include_identical=true surfaces unchanged tables too;
            // collapse them into a "(no changes)" line so the user can
            // still see the table was inspected.
            if (!t->changed() && !state.compare_include_identical) {
                continue;
            }
            // Chip filter — applied before render. Empty chip lets
            // everything through.
            if (!chip.empty()) {
                bool pass = false;
                if (chip == "@safety") {
                    pass = t->engine_safety_critical;
                } else if (chip == "@emissions") {
                    pass = t->emissions_relevant;
                } else if (chip == "@flagged") {
                    pass = t->engine_safety_critical || t->emissions_relevant;
                } else if (state.project.has_value()) {
                    auto const *tbl =
                        state.project->definition().find_table(t->table_id);
                    pass = (tbl != nullptr && tbl->category == chip);
                }
                if (!pass)
                    continue;
            }
            ++shown;
            if (render_table_row(state, *t)) {
                open_id = t->table_id;
            }
        }
        if (!chip.empty()) {
            text_subtle("%zu of %zu changed tables match the filter.", shown,
                        d.tables_changed);
        }
        // Defer the select_table call until after the loop so we don't
        // mutate state mid-render (select_table may rebuild the side
        // panel).
        if (!open_id.empty()) {
            state.select_table(open_id);
        }
    }

    // Skipped tables — surface so the user knows what didn't compare.
    if (!d.skipped.empty()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Skipped tables (could not compare)")) {
            for (auto const &k : d.skipped) {
                ImGui::BulletText("%s — %s", k.table_id.c_str(), k.reason.c_str());
            }
        }
    }

    ImGui::End();
}

} // namespace st::ui
