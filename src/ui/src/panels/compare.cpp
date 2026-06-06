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

#include "st/audit.hpp"
#include "st/diff.hpp"
#include "st/edit.hpp"
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

// Result of rendering a table row in the compare diff tree. Callers
// consume `open_in_editor` (jump-to-table) + `copy_b_to_a` (apply B's
// values into A's working slot via active_history) after the loop
// rather than mid-iteration — the actions mutate AppState in ways
// that would shift the per-frame compare result underneath us.
struct TableRowAction {
    bool open_in_editor{false};
    bool copy_b_to_a{false};
    bool toggle_pin{false};
};

// Render one table row in the changed-tables tree.
// `editable_a` is non-null when ROM A resolves to the project's
// working slot — that's the only case where Copy B→A makes sense
// (additional ROMs are read-only, source is immutable, file paths
// have no history target). When null the Copy action is hidden.
// `rom_b_ref` is the source bytes that would land in A; the actual
// read+write happens at the caller so that one place owns the
// edit::History invocation.
[[nodiscard]] TableRowAction render_table_row(AppState &state, st::diff::TableDelta const &t,
                                              bool a_is_working) {
    ImGui::PushID(t.table_id.c_str());

    TableRowAction action;

    // Pin/star toggle — leftmost column so pinned entries are
    // visually scannable. ★ when pinned, ☆ when not. Mirrors the
    // audit panel's 2a84616 pattern; the toggle persists via
    // save_compare_pinned (deferred to the caller via the action).
    bool const pinned =
        state.compare_pinned_table_ids.find(t.table_id) !=
        state.compare_pinned_table_ids.end();
    char const *star_glyph = pinned ? "\xE2\x98\x85" : "\xE2\x98\x86";
    if (ImGui::SmallButton(star_glyph)) {
        action.toggle_pin = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(pinned
                              ? "Unpin this table.  (Pinned tables\nshow ★ + survive a Compare re-run.)"
                              : "Pin this table.  (Pinned tables\nshow ★ + survive a Compare re-run.)");
    }
    ImGui::SameLine();

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
            action.open_in_editor = true;
        }
        // Copy B→A — only when A targets the project's working slot.
        // Engine-safety + emissions cells still pass through the
        // normal policy gate at flash time; the edit itself is just
        // a regular history-tracked write so Ctrl+Z reverses it.
        ImGui::SameLine();
        ImGui::BeginDisabled(!a_is_working || t.cells_changed == 0);
        if (ImGui::Button("\xE2\x86\x90  Copy B\xE2\x86\x92""A")) {
            action.copy_b_to_a = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!a_is_working) {
                ImGui::SetTooltip("Copy B→A needs ROM A to be the project's working\n"
                                  "slot (the only writable target). Pick <project\n"
                                  "working> in the A picker, then re-run Compare.");
            } else if (t.cells_changed == 0) {
                ImGui::SetTooltip("No differing cells to copy on this table.");
            } else {
                ImGui::SetTooltip("Write B's values for this table into the working ROM.\n"
                                  "Lands as a single undoable edit (Ctrl+Z reverses).\n"
                                  "Safety + emissions cells still gate at flash time.");
            }
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
    return action;
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
    track_help_context(state, AppState::HelpContext::Compare);

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
    glossary_tooltip_for(state, "ROM");
    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputText("##cmp_rom_a", state.compare_rom_a_path,
                     sizeof state.compare_rom_a_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cmp_a")) {
        pick_rom_into(state.compare_rom_a_path, sizeof state.compare_rom_a_path,
                      state.compare_error_msg);
    }

    ImGui::Text("ROM B (compare):");
    glossary_tooltip_for(state, "ROM");
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
    //
    // `pinned_filter` (nullable): when non-null, only changed tables
    // whose table_id is in the set are emitted. Used by the "Pinned
    // only" scope of Export Markdown ▾ — the star-pin → hand-to-tuner
    // workflow. nullptr = no filter, all changed tables emit.
    auto const render_diff_markdown =
        [](st::diff::DiffSet const &d,
           std::unordered_set<std::string> const *pinned_filter = nullptr)
        -> std::string {
        std::ostringstream ss;
        ss << "# ROM diff";
        if (pinned_filter != nullptr) {
            ss << " (pinned)";
        }
        ss << "\n\n";
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
            ss << "## Changed tables";
            if (pinned_filter != nullptr) {
                ss << " (pinned only)";
            }
            ss << "\n\n";
            for (auto const &t : d.tables) {
                if (!t.changed())
                    continue;
                if (pinned_filter != nullptr &&
                    pinned_filter->find(t.table_id) == pinned_filter->end()) {
                    continue;
                }
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

    // NDJSON renderer — one JSON object per changed table per line,
    // each self-contained (carries pack_id + ROM CRCs so a single line
    // is meaningful out of context). Matches the per-table shape in
    // st::diff::render_json's `tables` array so a downstream parser
    // built against the CLI envelope handles both. nullptr filter =
    // all changed tables; non-null = pinned subset only (stale ids
    // referencing tables not in this diff are silently dropped).
    auto const json_escape_inline = [](std::string &out, std::string_view s) {
        out.push_back('"');
        for (char c : s) {
            auto const u = static_cast<unsigned char>(c);
            switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (u < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04X", u);
                    out.append(b);
                } else {
                    out.push_back(c);
                }
            }
        }
        out.push_back('"');
    };
    auto const render_diff_ndjson =
        [&](st::diff::DiffSet const &d,
            std::unordered_set<std::string> const *pinned_filter) -> std::string {
        std::string out;
        out.reserve(4096);
        char buf[64];
        for (auto const &t : d.tables) {
            if (!t.changed()) {
                continue;
            }
            if (pinned_filter != nullptr &&
                pinned_filter->find(t.table_id) == pinned_filter->end()) {
                continue;
            }
            out.append("{\"schema\":\"subuwutuner.diff.table.v1\",\"pack_id\":");
            json_escape_inline(out, d.pack_id);
            out.append(",\"rom_a_crc32\":");
            out.append(std::to_string(d.rom_a_crc32));
            out.append(",\"rom_b_crc32\":");
            out.append(std::to_string(d.rom_b_crc32));
            out.append(",\"table_id\":");
            json_escape_inline(out, t.table_id);
            out.append(",\"table_name\":");
            json_escape_inline(out, t.table_name);
            out.append(",\"rows\":");
            out.append(std::to_string(t.rows));
            out.append(",\"cols\":");
            out.append(std::to_string(t.cols));
            out.append(",\"total_cells\":");
            out.append(std::to_string(t.total_cells));
            out.append(",\"cells_changed\":");
            out.append(std::to_string(t.cells_changed));
            std::snprintf(buf, sizeof buf, "%g", t.max_abs_delta);
            out.append(",\"max_abs_delta\":");
            out.append(buf);
            std::snprintf(buf, sizeof buf, "%g", t.mean_abs_delta);
            out.append(",\"mean_abs_delta\":");
            out.append(buf);
            out.append(",\"engine_safety_critical\":");
            out.append(t.engine_safety_critical ? "true" : "false");
            out.append(",\"emissions_relevant\":");
            out.append(t.emissions_relevant ? "true" : "false");
            out.append(",\"changes\":[");
            for (std::size_t j = 0; j < t.changes.size(); ++j) {
                if (j != 0) {
                    out.append(",");
                }
                auto const &c = t.changes[j];
                out.append("{\"row\":");
                out.append(std::to_string(c.row));
                out.append(",\"col\":");
                out.append(std::to_string(c.col));
                std::snprintf(buf, sizeof buf, "%g", c.value_a);
                out.append(",\"value_a\":");
                out.append(buf);
                std::snprintf(buf, sizeof buf, "%g", c.value_b);
                out.append(",\"value_b\":");
                out.append(buf);
                std::snprintf(buf, sizeof buf, "%g", c.delta());
                out.append(",\"delta\":");
                out.append(buf);
                out.push_back('}');
            }
            out.append("]}\n");
        }
        return out;
    };

    ImGui::BeginDisabled(!state.compare_result.has_value());
    if (ImGui::Button("Export Markdown  \xE2\x96\xBE", ImVec2(170.0f, 0.0f))) { // ▾
        ImGui::OpenPopup("##compare_export_md_scope");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.compare_result.has_value()) {
            ImGui::SetTooltip(
                "Write a shareable Markdown summary of the diff.\n"
                "Pick All (every changed table) or Pinned only\n"
                "(your star-marked subset — round-trips the\n"
                "share-with-e-tuner workflow).");
        } else {
            ImGui::SetTooltip("Run Compare first to enable export.");
        }
    }
    if (state.compare_result.has_value() &&
        ImGui::BeginPopup("##compare_export_md_scope")) {
        auto const &d_for_count = *state.compare_result;
        // Count changed-table intersections with the pinned set. Stale
        // pin ids (rows referencing tables no longer in this diff)
        // don't pad the count — same shape audit's a2ebdb4 used.
        std::size_t pinned_in_diff = 0;
        for (auto const &t : d_for_count.tables) {
            if (!t.changed())
                continue;
            if (state.compare_pinned_table_ids.find(t.table_id) !=
                state.compare_pinned_table_ids.end()) {
                ++pinned_in_diff;
            }
        }
        auto const run_md_export = [&](bool pinned_only,
                                       std::string const &default_name) {
            NFD::UniquePath out;
            nfdfilteritem_t const filters[] = {{"Markdown", "md"}};
            nfdresult_t const r = NFD::SaveDialog(out, filters, 1, nullptr,
                                                  default_name.c_str());
            if (r == NFD_OKAY) {
                std::filesystem::path const target{out.get()};
                std::ofstream fh{target, std::ios::binary};
                if (!fh) {
                    state.compare_error_msg =
                        "Export Markdown: cannot open " + target.string();
                    return;
                }
                auto const md = render_diff_markdown(
                    *state.compare_result,
                    pinned_only ? &state.compare_pinned_table_ids : nullptr);
                fh << md;
                if (!fh) {
                    state.compare_error_msg = "Export Markdown: write failed";
                    return;
                }
                if (pinned_only) {
                    enqueue_toast(state, ToastKind::Success,
                                  "Wrote " + std::to_string(pinned_in_diff) +
                                      " pinned tables to " + target.string());
                } else {
                    enqueue_toast(state, ToastKind::Success,
                                  "Wrote " + target.string());
                }
            } else if (r == NFD_ERROR) {
                state.compare_error_msg =
                    std::string{"Export Markdown dialog error: "} + NFD::GetError();
            }
        };
        char buf[80];
        std::snprintf(buf, sizeof buf, "All changed tables (%zu)",
                      d_for_count.tables_changed);
        if (ImGui::MenuItem(buf)) {
            run_md_export(false, "rom-diff.md");
        }
        std::snprintf(buf, sizeof buf, "Pinned only (%zu)", pinned_in_diff);
        ImGui::BeginDisabled(pinned_in_diff == 0);
        if (ImGui::MenuItem(buf)) {
            run_md_export(true, "rom-diff-pinned.md");
        }
        ImGui::EndDisabled();
        // Surface the gap when pinned ids reference tables that aren't
        // in the current diff (e.g. pack changed, or the diff scope is
        // narrower than what was starred). Quiet — doesn't gate the
        // action; just lets the user notice the mismatch.
        std::size_t const total_pinned = state.compare_pinned_table_ids.size();
        if (total_pinned > pinned_in_diff) {
            text_subtle("(%zu pinned ids reference tables not in this diff)",
                        total_pinned - pinned_in_diff);
        }
        ImGui::EndPopup();
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
    // Export NDJSON — same All / Pinned scope shape as Markdown, but
    // emits one JSON object per changed table per line. Suited for
    // tooling that wants to stream-parse a tune diff (CI hooks,
    // analyst notebooks). Pinned scope is the natural share-with-tuner
    // wire: star the relevant tables, export, attach to the question.
    ImGui::BeginDisabled(!state.compare_result.has_value());
    if (ImGui::Button("Export NDJSON  \xE2\x96\xBE", ImVec2(150.0f, 0.0f))) { // ▾
        ImGui::OpenPopup("##compare_export_ndjson_scope");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.compare_result.has_value()) {
            ImGui::SetTooltip(
                "Write one JSON object per changed table per line.\n"
                "Pick All (every changed table) or Pinned only\n"
                "(your star-marked subset). Per-line shape matches\n"
                "subuwutuner.diff.v1's `tables[]` entries so a parser\n"
                "built against the CLI envelope handles both.");
        } else {
            ImGui::SetTooltip("Run Compare first to enable export.");
        }
    }
    if (state.compare_result.has_value() &&
        ImGui::BeginPopup("##compare_export_ndjson_scope")) {
        auto const &d_for_count = *state.compare_result;
        std::size_t pinned_in_diff = 0;
        for (auto const &t : d_for_count.tables) {
            if (!t.changed()) {
                continue;
            }
            if (state.compare_pinned_table_ids.find(t.table_id) !=
                state.compare_pinned_table_ids.end()) {
                ++pinned_in_diff;
            }
        }
        auto const run_ndjson_export = [&](bool pinned_only,
                                           std::string const &default_name) {
            NFD::UniquePath out;
            nfdfilteritem_t const filters[] = {{"NDJSON", "ndjson"},
                                                {"JSON Lines", "jsonl"}};
            nfdresult_t const r = NFD::SaveDialog(out, filters, 2, nullptr,
                                                  default_name.c_str());
            if (r == NFD_OKAY) {
                std::filesystem::path const target{out.get()};
                std::ofstream fh{target, std::ios::binary};
                if (!fh) {
                    state.compare_error_msg =
                        "Export NDJSON: cannot open " + target.string();
                    return;
                }
                auto const ndjson = render_diff_ndjson(
                    *state.compare_result,
                    pinned_only ? &state.compare_pinned_table_ids : nullptr);
                fh << ndjson;
                if (!fh) {
                    state.compare_error_msg = "Export NDJSON: write failed";
                    return;
                }
                if (pinned_only) {
                    enqueue_toast(state, ToastKind::Success,
                                  "Wrote " + std::to_string(pinned_in_diff) +
                                      " pinned tables to " + target.string());
                } else {
                    enqueue_toast(state, ToastKind::Success,
                                  "Wrote " + target.string());
                }
            } else if (r == NFD_ERROR) {
                state.compare_error_msg =
                    std::string{"Export NDJSON dialog error: "} + NFD::GetError();
            }
        };
        char buf[80];
        std::snprintf(buf, sizeof buf, "All changed tables (%zu)",
                      d_for_count.tables_changed);
        if (ImGui::MenuItem(buf)) {
            run_ndjson_export(false, "rom-diff.ndjson");
        }
        std::snprintf(buf, sizeof buf, "Pinned only (%zu)", pinned_in_diff);
        ImGui::BeginDisabled(pinned_in_diff == 0);
        if (ImGui::MenuItem(buf)) {
            run_ndjson_export(true, "rom-diff-pinned.ndjson");
        }
        ImGui::EndDisabled();
        std::size_t const total_pinned = state.compare_pinned_table_ids.size();
        if (total_pinned > pinned_in_diff) {
            text_subtle("(%zu pinned ids reference tables not in this diff)",
                        total_pinned - pinned_in_diff);
        }
        ImGui::EndPopup();
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
        // Tooltip on the SmallButton's hover state — fires for the
        // previous item, which is the chip just rendered.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Filter to tables tagged engine-safety-critical.\n"
                              "These are the maps where a wrong edit can\n"
                              "damage the engine; they always gate at flash\n"
                              "time regardless of jurisdiction profile.");
        }
        chip_button("Emissions", "@emissions");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Filter to tables tagged emissions-relevant.\n"
                              "Treatment depends on the active jurisdiction\n"
                              "profile (silent / warn / confirm / block).");
        }
        chip_button("Either flag", "@flagged");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Tables that carry either flag — the union of\n"
                              "Safety-critical + Emissions.");
        }
        // Pinned-only toggle. Separate from chip_filter_chip because
        // pin is composable with the category / safety filter — a
        // user can star a handful of tables AND keep filtering by
        // category. Stays as a Checkbox so the AND semantics read
        // naturally next to the OR-of-chip selector.
        ImGui::Checkbox("Pinned only", &state.compare_pinned_only);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show only tables you've starred (\xE2\x98\x85)\n"
                              "via the per-row pin button. Composes with the\n"
                              "category / safety filter — both filters apply.");
        }
        ImGui::SameLine();
        // Bulk pin operations. Scope = current chip filter + the
        // include_identical toggle; explicitly NOT the "Pinned only"
        // checkbox (acting on the pinned-only set would make "Pin N
        // visible" always 0). Mirrors the audit panel pattern from
        // 5797fc3 — counts snapshot when popup opens so menu labels
        // read "Pin 12 visible" instead of an unqualified verb.
        ImGui::BeginDisabled(d.tables.empty());
        if (ImGui::Button("Bulk pins  \xE2\x96\xBE")) { // ▾
            ImGui::OpenPopup("##compare_bulk_pins");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Bulk pin / unpin actions against the\n"
                              "current chip filter scope. The Pinned-only\n"
                              "checkbox is excluded — bulk ops act on the\n"
                              "chip-filtered set.");
        }
        if (ImGui::BeginPopup("##compare_bulk_pins")) {
            std::string_view const bulk_chip{state.compare_filter_chip};
            auto const passes_filter = [&](st::diff::TableDelta const &t) {
                if (!t.changed() && !state.compare_include_identical) {
                    return false;
                }
                if (bulk_chip.empty()) {
                    return true;
                }
                if (bulk_chip == "@safety") {
                    return t.engine_safety_critical;
                }
                if (bulk_chip == "@emissions") {
                    return t.emissions_relevant;
                }
                if (bulk_chip == "@flagged") {
                    return t.engine_safety_critical || t.emissions_relevant;
                }
                if (state.project.has_value()) {
                    auto const *tbl =
                        state.project->definition().find_table(t.table_id);
                    return tbl != nullptr && tbl->category == bulk_chip;
                }
                return false;
            };
            std::size_t visible_total = 0;
            std::size_t visible_unpinned = 0;
            std::size_t visible_pinned = 0;
            for (auto const &t : d.tables) {
                if (!passes_filter(t)) {
                    continue;
                }
                ++visible_total;
                if (state.compare_pinned_table_ids.contains(t.table_id)) {
                    ++visible_pinned;
                } else {
                    ++visible_unpinned;
                }
            }
            auto persist_pins = [&]() {
                if (!state.project.has_value()) {
                    return;
                }
                std::vector<std::string> v(state.compare_pinned_table_ids.begin(),
                                           state.compare_pinned_table_ids.end());
                std::sort(v.begin(), v.end()); // stable on-disk order
                save_compare_pinned(state.project->dir(), v);
            };
            char buf[64];
            std::snprintf(buf, sizeof buf, "Pin %zu visible", visible_unpinned);
            ImGui::BeginDisabled(visible_unpinned == 0);
            if (ImGui::MenuItem(buf)) {
                for (auto const &t : d.tables) {
                    if (passes_filter(t)) {
                        state.compare_pinned_table_ids.insert(t.table_id);
                    }
                }
                persist_pins();
            }
            ImGui::EndDisabled();
            std::snprintf(buf, sizeof buf, "Unpin %zu visible", visible_pinned);
            ImGui::BeginDisabled(visible_pinned == 0);
            if (ImGui::MenuItem(buf)) {
                for (auto const &t : d.tables) {
                    if (passes_filter(t)) {
                        state.compare_pinned_table_ids.erase(t.table_id);
                    }
                }
                persist_pins();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            std::snprintf(buf, sizeof buf, "Clear all pins (%zu)",
                          state.compare_pinned_table_ids.size());
            ImGui::BeginDisabled(state.compare_pinned_table_ids.empty());
            if (ImGui::MenuItem(buf)) {
                state.compare_pinned_table_ids.clear();
                if (state.project.has_value()) {
                    save_compare_pinned(state.project->dir(), {});
                }
            }
            ImGui::EndDisabled();
            text_subtle("Chip: %s",
                        bulk_chip.empty() ? "(none — acts on all changed tables)"
                                          : state.compare_filter_chip);
            text_subtle("Visible: %zu / %zu", visible_total, d.tables.size());
            ImGui::EndPopup();
        }
        ImGui::SameLine();
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

        // Precompute "is A == project working ROM" once per frame
        // rather than per-row — it's a path-resolution + pointer
        // comparison, but cheaper to hoist. The Copy B→A action also
        // needs B's bytes; we re-resolve them on demand inside the
        // per-row handler so the diff cache isn't holding stale ROM
        // pointers across recompute calls.
        bool const a_is_working =
            (std::string_view{state.compare_rom_a_path} == kProjectWorkingSentinel);
        std::string open_id;
        std::string copy_id; // table to apply B→A after the render pass
        std::string pin_toggle_id; // table whose pin star was clicked
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
            // Pinned-only filter applies AFTER the chip filter so the
            // two compose: e.g. "@safety + Pinned only" lists the
            // user-starred safety-critical entries.
            if (state.compare_pinned_only &&
                state.compare_pinned_table_ids.find(t->table_id) ==
                    state.compare_pinned_table_ids.end()) {
                continue;
            }
            ++shown;
            auto const row_action = render_table_row(state, *t, a_is_working);
            if (row_action.open_in_editor) {
                open_id = t->table_id;
            }
            if (row_action.copy_b_to_a) {
                copy_id = t->table_id;
            }
            if (row_action.toggle_pin) {
                pin_toggle_id = t->table_id;
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
        // Pin toggle — also deferred for the same defer-to-post-loop
        // reason (the std::unordered_set mutation would invalidate
        // any concurrent iteration). Persist on each toggle so the
        // star markers survive a session restart.
        if (!pin_toggle_id.empty() && state.project.has_value()) {
            auto it = state.compare_pinned_table_ids.find(pin_toggle_id);
            if (it != state.compare_pinned_table_ids.end()) {
                state.compare_pinned_table_ids.erase(it);
            } else {
                state.compare_pinned_table_ids.insert(pin_toggle_id);
            }
            std::vector<std::string> v(state.compare_pinned_table_ids.begin(),
                                        state.compare_pinned_table_ids.end());
            std::sort(v.begin(), v.end()); // stable on-disk order
            save_compare_pinned(state.project->dir(), v);
        }
        // Copy B→A — also deferred. Re-resolves ROM B at apply time
        // rather than caching the resolved pointer in compare_result
        // (the diff cache is allowed to hold scaled-cell views; raw
        // ROM byte references would be a cross-frame lifetime risk).
        if (!copy_id.empty() && state.project.has_value()) {
            // Recover the diff metadata for the just-clicked table so
            // the audit entry can record cell-change count without
            // re-walking the diff.
            std::size_t copy_cells = 0;
            for (auto const &td : d.tables) {
                if (td.table_id == copy_id) {
                    copy_cells = td.cells_changed;
                    break;
                }
            }
            auto const *tbl = state.project->definition().find_table(copy_id);
            std::string b_err;
            auto rom_b = resolve_rom_input(state, state.compare_rom_b_path, b_err, "ROM B");
            if (tbl == nullptr) {
                state.status_msg = "Copy B→A: table '" + copy_id + "' missing from pack.";
            } else if (!rom_b.ok) {
                state.status_msg = "Copy B→A: " + b_err;
            } else {
                Rom const &rom_b_ref = rom_b.in_memory ? *rom_b.in_memory : *rom_b.loaded;
                Rom *target = &state.project->working_rom();
                auto b_td = state.project->definition().read_table_values(rom_b_ref, *tbl);
                auto a_td = state.project->definition().read_table_values(*target, *tbl);
                if (!b_td.has_value()) {
                    state.status_msg = "Copy B→A: read B: " + b_td.error().to_string();
                } else if (!a_td.has_value()) {
                    state.status_msg = "Copy B→A: read A: " + a_td.error().to_string();
                } else {
                    auto const rect = st::edit::whole_table(*a_td);
                    auto before = st::edit::snapshot(*a_td, rect);
                    auto after = st::edit::snapshot(*b_td, rect);
                    if (!before.has_value()) {
                        state.status_msg = "Copy B→A: before snapshot: " +
                                           before.error().to_string();
                    } else if (!after.has_value()) {
                        state.status_msg = "Copy B→A: after snapshot: " +
                                           after.error().to_string();
                    } else {
                        auto wb = state.project->definition().write_table_values(
                            *target, *tbl, *b_td);
                        if (!wb.has_value()) {
                            state.status_msg =
                                "Copy B→A: writeback: " + wb.error().to_string();
                        } else {
                            std::string label =
                                "Copy B→A on " + copy_id +
                                " (" + std::to_string(copy_cells) + " cells)";
                            std::string const audited = label;
                            state.project->active_history().record(st::edit::Edit::table(
                                copy_id, std::move(*before), std::move(*after),
                                std::move(label)));
                            state.dirty = true;
                            state.status_msg = audited;
                            if (state.audit_log.has_value()) {
                                std::vector<std::pair<std::string, std::string>> fields{
                                    {"table", copy_id},
                                    {"cells", std::to_string(copy_cells)},
                                    {"source", "compare_panel.b_to_a"}};
                                (void)state.audit_log->log(
                                    st::audit::EntryKind::EditCommitted,
                                    "ui.compare", audited, std::move(fields));
                            }
                            // Recompute diff so the row's cells_changed
                            // count flips to 0 (or shows what's still
                            // different if the user copies again).
                            recompute_compare(state);
                        }
                    }
                }
            }
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
