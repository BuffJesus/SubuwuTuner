// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// subuwutuner-gui — Dear ImGui front-end.
//
// Opens a .stune project passed as argv[1], renders a dockable sidebar of
// tables, and shows the selected table as a read-only grid. Editing,
// project-management menus, and file-open dialogs land in follow-ups; this
// pass lays the polish foundation: docking + viewports, tuned palette,
// system-font probing.

#include "st/core/version.hpp"
#include "st/edit.hpp"
#include "st/flash.hpp"
#include "st/policy.hpp"
#include "st/project.hpp"

// ImGui + backends.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // DockBuilder*
#include <implot.h>

#include <GLFW/glfw3.h>
#include <nfd.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct Fonts {
    ImFont *ui   = nullptr; // Sans for UI chrome (menus, labels, panels)
    ImFont *mono = nullptr; // Monospace for grids, hex, log output
};

// ---------------------------------------------------------------------
// Recents — one-line-per-entry config persisted between cold starts.
// Lives next to other user-config in the OS-conventional location:
//   Windows: %LOCALAPPDATA%\SubuwuTuner\recents.txt
//   Mac:     ~/Library/Application Support/SubuwuTuner/recents.txt
//   Linux:   $XDG_CONFIG_HOME/subuwutuner/recents.txt
//            (fallback: $HOME/.config/subuwutuner/recents.txt)
//
// Format: one entry per line, "<ISO-8601 UTC>\t<absolute path>".
// Cap at 8 entries; most recent first. A malformed line is silently
// skipped — recents are a convenience, not a source of truth.
// ---------------------------------------------------------------------

struct RecentEntry {
    std::string opened_at;   // ISO 8601 UTC, e.g. "2026-05-12T15:30:00Z"
    std::filesystem::path path;
};

constexpr std::size_t kRecentsCap = 8;

std::filesystem::path config_dir_root() {
    auto const env = [](char const *name) -> std::filesystem::path {
        auto const *v = std::getenv(name);
        return v != nullptr ? std::filesystem::path{v}
                            : std::filesystem::path{};
    };
#if defined(_WIN32)
    auto base = env("LOCALAPPDATA");
    if (base.empty()) base = env("USERPROFILE");
    if (base.empty()) base = std::filesystem::current_path();
    return base / "SubuwuTuner";
#elif defined(__APPLE__)
    auto base = env("HOME");
    if (base.empty()) base = std::filesystem::current_path();
    return base / "Library" / "Application Support" / "SubuwuTuner";
#else
    auto base = env("XDG_CONFIG_HOME");
    if (base.empty()) {
        auto home = env("HOME");
        if (home.empty()) home = std::filesystem::current_path();
        base = home / ".config";
    }
    return base / "subuwutuner";
#endif
}

std::filesystem::path recents_config_path() {
    return config_dir_root() / "recents.txt";
}

std::filesystem::path settings_config_path() {
    return config_dir_root() / "settings.txt";
}

std::string iso8601_utc_now() {
    auto const  now = std::chrono::system_clock::now();
    auto const  t   = std::chrono::system_clock::to_time_t(now);
    std::tm     tm{};
#if defined(_WIN32)
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf};
}

// Render an ISO-8601-UTC timestamp as a human relative phrase ("2 hours
// ago", "Yesterday", "3 weeks ago"). Empty string on parse failure or
// future timestamps (clock skew or hand-edited recents file).
std::string format_relative_time(std::string const &iso) {
    int Y = 0;
    int M = 0;
    int D = 0;
    int h = 0;
    int m = 0;
    int s = 0;
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
                    &Y, &M, &D, &h, &m, &s) != 6) {
        return {};
    }
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon  = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min  = m;
    tm.tm_sec  = s;
#if defined(_WIN32)
    std::time_t const then = ::_mkgmtime(&tm);
#else
    std::time_t const then = ::timegm(&tm);
#endif
    if (then == static_cast<std::time_t>(-1)) {
        return {};
    }
    auto const now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    auto const diff = static_cast<long long>(std::difftime(now, then));
    if (diff < 60) {
        return "just now";
    }
    auto const plural = [](long long n, char const *one, char const *many) {
        return std::to_string(n) + " " + (n == 1 ? one : many) + " ago";
    };
    if (diff < 3600) {
        return plural(diff / 60, "minute", "minutes");
    }
    if (diff < 86400) {
        return plural(diff / 3600, "hour", "hours");
    }
    if (diff < 86400 * 2) {
        return "yesterday";
    }
    if (diff < 86400 * 7) {
        return plural(diff / 86400, "day", "days");
    }
    if (diff < 86400 * 30) {
        return plural(diff / (86400 * 7), "week", "weeks");
    }
    if (diff < 86400 * 365) {
        return plural(diff / (86400 * 30), "month", "months");
    }
    return plural(diff / (86400 * 365), "year", "years");
}

std::vector<RecentEntry> load_recents() {
    std::vector<RecentEntry> out;
    std::ifstream            in{recents_config_path()};
    if (!in) return out;
    std::string line;
    while (std::getline(in, line) && out.size() < kRecentsCap) {
        auto const tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) {
            continue;
        }
        RecentEntry e;
        e.opened_at = line.substr(0, tab);
        e.path      = std::filesystem::path{line.substr(tab + 1)};
        out.push_back(std::move(e));
    }
    return out;
}

void save_recents(std::vector<RecentEntry> const &recents) {
    auto const  path = recents_config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Failure to create the directory is non-fatal — recents is best-
    // effort. The next save attempt will retry.
    std::ofstream out{path, std::ios::trunc};
    if (!out) return;
    for (auto const &e : recents) {
        out << e.opened_at << '\t' << e.path.generic_string() << '\n';
    }
}

// User-preferences persistence. Stored next to recents.txt as a
// `key=value\n` plain-text file (one setting per line). New settings can
// land without breaking older builds — unknown keys are silently
// ignored on load. Currently:
//   default_policy_profile = motorsport-only|alberta-ca|eu-roadworthy|california-us
struct Settings {
    st::policy::Profile default_policy_profile{st::policy::Profile::MotorsportOnly};
};

Settings load_settings() {
    Settings s;
    std::ifstream in{settings_config_path()};
    if (!in) return s;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto const eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string_view const key{line.data(), eq};
        std::string_view const val{line.data() + eq + 1,
                                    line.size() - eq - 1};
        if (key == "default_policy_profile") {
            if (auto p = st::policy::parse_profile(val); p.has_value()) {
                s.default_policy_profile = *p;
            }
        }
    }
    return s;
}

void save_settings(Settings const &s) {
    auto const  path = settings_config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::trunc};
    if (!out) return;
    out << "default_policy_profile="
        << st::policy::profile_name(s.default_policy_profile) << '\n';
}

// Move `path` to the front of `recents`, deduplicating by canonical
// path comparison and capping the list at `kRecentsCap`. Idempotent.
void push_recent(std::vector<RecentEntry>     &recents,
                 std::filesystem::path const &path) {
    std::error_code ec;
    auto const      canon =
        std::filesystem::weakly_canonical(path, ec);
    auto const compare_to = canon.empty() ? path : canon;
    // Remove any existing entry pointing at the same canonical path.
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                 [&](RecentEntry const &e) {
                                     std::error_code ec2;
                                     auto const ec_path =
                                         std::filesystem::weakly_canonical(
                                             e.path, ec2);
                                     return (ec_path.empty() ? e.path : ec_path)
                                            == compare_to;
                                 }),
                  recents.end());
    // Insert at the front.
    RecentEntry e;
    e.opened_at = iso8601_utc_now();
    e.path      = compare_to;
    recents.insert(recents.begin(), std::move(e));
    if (recents.size() > kRecentsCap) recents.resize(kRecentsCap);
}

// Anchor + cursor selection model. Click sets both; shift-click moves only
// the cursor — the cell rect runs between anchor and cursor inclusively.
// `enabled` distinguishes "nothing selected" from "single cell at (0,0)".
struct Selection {
    bool        enabled{false};
    std::size_t r_anchor{0};
    std::size_t c_anchor{0};
    std::size_t r_cursor{0};
    std::size_t c_cursor{0};

    [[nodiscard]] bool contains(std::size_t r, std::size_t c) const noexcept {
        if (!enabled) {
            return false;
        }
        auto const rmin = std::min(r_anchor, r_cursor);
        auto const rmax = std::max(r_anchor, r_cursor);
        auto const cmin = std::min(c_anchor, c_cursor);
        auto const cmax = std::max(c_anchor, c_cursor);
        return r >= rmin && r <= rmax && c >= cmin && c <= cmax;
    }

    [[nodiscard]] std::size_t rows() const noexcept {
        return enabled ? (std::max(r_anchor, r_cursor) - std::min(r_anchor, r_cursor) + 1) : 0;
    }
    [[nodiscard]] std::size_t cols() const noexcept {
        return enabled ? (std::max(c_anchor, c_cursor) - std::min(c_anchor, c_cursor) + 1) : 0;
    }

    [[nodiscard]] st::edit::Rect as_rect() const noexcept {
        return st::edit::Rect{std::min(r_anchor, r_cursor), std::max(r_anchor, r_cursor),
                              std::min(c_anchor, c_cursor), std::max(c_anchor, c_cursor)};
    }

    void click(std::size_t r, std::size_t c, bool shift) noexcept {
        if (shift && enabled) {
            r_cursor = r;
            c_cursor = c;
        } else {
            r_anchor = r_cursor = r;
            c_anchor = c_cursor = c;
            enabled  = true;
        }
    }

    void reset() noexcept { enabled = false; }
};

enum class TableViewMode {
    Grid,
    Heatmap,
};

// What the user was trying to do when the unsaved-changes modal fired.
// Captured so the modal's Save/Discard handlers know what to do next.
enum class ConfirmAction {
    None,
    OpenDialog,
    OpenRecent,
    Close,
    Quit,
};

struct AppState {
    std::optional<st::Project>               project;
    std::string                              status_msg;
    std::string                              selected_table_id;
    std::optional<st::Definition::TableData> current_table_data;
    Selection                                selection;
    TableViewMode                            view_mode{TableViewMode::Grid};
    std::size_t                              selected_z{0};
    bool                                     show_imgui_demo{false};
    // Loaded once at startup, persisted on every successful open. See
    // recents_config_path() for the on-disk location.
    std::vector<RecentEntry>                 recents;
    Settings                                 settings;

    // Sidebar filter. Substring-matched (case-insensitive) against table
    // name + id. 128 chars is generous — table identifiers in real packs
    // top out around 40. `focus_table_filter` is the Ctrl+F handoff: set
    // by the main-loop shortcut, consumed by the sidebar's next render.
    char                                     table_filter[128]{};
    bool                                     focus_table_filter{false};

    // DTC-panel filter buffer. Same shape as table_filter; matched against
    // DTC code (P0401) or name.
    char                                     dtc_filter[128]{};

    // Inline cell-value editor state. Active iff `editing_cell` is true;
    // the cell being edited is identified by selection.r_cursor /
    // selection.c_cursor (these don't change while editing — the
    // arrow-key nav block reads `editing_cell` and skips its movement
    // logic). `editor_just_opened` is a one-frame handoff so the new
    // InputText gets SetKeyboardFocusHere on its first render.
    bool                                     editing_cell{false};
    bool                                     editor_just_opened{false};
    char                                     edit_buf[64]{};

    // Unsaved-changes tracking. `dirty` is conservative: any edit flips
    // it true, save flips it false. An undo-back-to-clean leaves dirty
    // true, which is harmless because the resulting save is a no-op
    // against an already-correct file. Switching projects via the
    // modal's Discard option also clears dirty.
    bool                                     dirty{false};
    ConfirmAction                            next_action{ConfirmAction::None};
    std::filesystem::path                    next_recent{};
    bool                                     show_unsaved_modal{false};
    // Set when the user has confirmed (or there was nothing to confirm)
    // that they want to quit. Main loop reads this AFTER rendering each
    // frame and breaks when true.
    bool                                     user_confirmed_quit{false};

    // Flash-flow modal state. `show_flash_modal` opens the popup on the
    // next frame; the rest are buffers for the confirm / reason UI bound
    // to the active project's profile. Fixed-size char buffer keeps the
    // ImGui call free of the `imgui_stdlib` std::string helper.
    bool                                     show_flash_modal{false};
    bool                                     flash_confirm_checked{false};
    char                                     flash_reason[512]{};

    void try_open_project(std::filesystem::path const &path) {
        auto r = st::Project::open(path);
        if (!r.has_value()) {
            status_msg = "Failed to open " + path.string() + ": " + r.error().to_string();
            project.reset();
            selected_table_id.clear();
            current_table_data.reset();
            selection.reset();
            return;
        }
        project = std::move(*r);
        status_msg.clear();
        selected_table_id.clear();
        current_table_data.reset();
        selection.reset();
        // New project = clean state.
        dirty = false;
        // Successful open → bump in recents so the welcome panel shows
        // this project at the top next cold start.
        push_recent(recents, path);
        save_recents(recents);
        // Auto-select the first table so the user lands on data
        // instead of the "Pick a table from the left panel" empty
        // state. Predictable: always the first in pack order — pack
        // authors get to control the landing experience by ordering
        // their `[[table]]` entries.
        auto const &tables = project->definition().tables();
        if (!tables.empty()) {
            select_table(tables.front().id);
        }
    }

    void select_table(std::string const &id) {
        selected_table_id = id;
        current_table_data.reset();
        selection.reset();
        selected_z = 0;
        if (!project.has_value()) {
            return;
        }
        auto const *table = project->definition().find_table(id);
        if (table == nullptr) {
            return;
        }
        auto td = project->definition().read_table_values(project->working_rom(), *table);
        if (td.has_value()) {
            current_table_data = std::move(*td);
        }
    }

    void close_project() {
        project.reset();
        selected_table_id.clear();
        current_table_data.reset();
        selection.reset();
        selected_z = 0;
        status_msg.clear();
        dirty = false;
    }
};

// Native folder picker for a .stune project directory. nfd handles the OS
// dialog; we only have to feed the result back through Project::open.
void open_project_dialog(AppState &state) {
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::PickFolder(out_path);
    if (r == NFD_OKAY) {
        state.try_open_project(std::filesystem::path(out_path.get()));
    } else if (r == NFD_ERROR) {
        state.status_msg = std::string{"Open dialog error: "} + NFD::GetError();
    }
}

void save_project(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    if (auto s = state.project->save_working_rom(); !s.has_value()) {
        state.status_msg = "Save failed: " + s.error().to_string();
        return;
    }
    state.status_msg = "Saved.";
    state.dirty      = false;
}

// Snapshot, mutate, snapshot, writeback, record. If the writeback fails we
// restore the in-memory TableData so the rendered grid stays consistent with
// the ROM bytes — better than silently diverging.
template <typename Op>
void apply_op(AppState &state, std::string label, Op &&op) {
    if (!state.project.has_value() || !state.current_table_data.has_value()
        || !state.selection.enabled) {
        return;
    }
    auto &td = *state.current_table_data;
    auto const rect = state.selection.as_rect();

    auto before = st::edit::snapshot(td, rect);
    if (!before.has_value()) {
        state.status_msg = label + ": snapshot: " + before.error().to_string();
        return;
    }

    if (auto s = op(td, rect); !s.has_value()) {
        state.status_msg = label + ": " + s.error().to_string();
        return;
    }

    auto after = st::edit::snapshot(td, rect);
    if (!after.has_value()) {
        // op succeeded but post-snapshot failed — try to roll back td so the
        // view matches what's still on disk.
        (void) st::edit::restore(td, *before);
        state.status_msg = label + ": snapshot: " + after.error().to_string();
        return;
    }

    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl == nullptr) {
        (void) st::edit::restore(td, *before);
        state.status_msg = label + ": table missing from pack";
        return;
    }

    auto wb = state.project->definition().write_table_values(
        state.project->working_rom(), *tbl, td);
    if (!wb.has_value()) {
        (void) st::edit::restore(td, *before);
        state.status_msg = label + ": writeback: " + wb.error().to_string();
        return;
    }

    state.project->history().record(st::edit::Edit{state.selected_table_id,
                                                   std::move(*before),
                                                   std::move(*after),
                                                   std::move(label)});
    state.status_msg.clear();
    state.dirty = true;
}

// Undo/redo share the same restore-and-writeback shape; only the snapshot
// side and the rollback direction differ. `forward = false` for undo,
// `forward = true` for redo.
void apply_history_step(AppState &state, st::edit::Edit const &edit, bool forward) {
    auto const *tbl = state.project->definition().find_table(edit.table_id);
    auto const  rollback_cursor = [&] {
        if (forward) {
            (void) state.project->history().undo();
        } else {
            (void) state.project->history().redo();
        }
    };
    if (tbl == nullptr) {
        state.status_msg = "history: table not in pack: " + edit.table_id;
        rollback_cursor();
        return;
    }

    auto td = state.project->definition().read_table_values(
        state.project->working_rom(), *tbl);
    if (!td.has_value()) {
        state.status_msg = "history re-read: " + td.error().to_string();
        rollback_cursor();
        return;
    }

    auto const &snap = forward ? edit.after : edit.before;
    if (auto s = st::edit::restore(*td, snap); !s.has_value()) {
        state.status_msg = "history restore: " + s.error().to_string();
        rollback_cursor();
        return;
    }

    auto wb = state.project->definition().write_table_values(
        state.project->working_rom(), *tbl, *td);
    if (!wb.has_value()) {
        state.status_msg = "history writeback: " + wb.error().to_string();
        rollback_cursor();
        return;
    }

    if (edit.table_id == state.selected_table_id) {
        state.current_table_data = std::move(*td);
    }
    state.status_msg.clear();
    // Undo / redo modifies the working ROM in memory; flag dirty so the
    // unsaved-changes guard catches an undo-then-quit-without-save.
    state.dirty = true;
}

// Forward decl — parse_tsv lives further down in the anon namespace
// near the other clipboard helpers. paste_clipboard_at_cursor uses it.
std::vector<std::vector<double>> parse_tsv(std::string_view text);

// Reads the system clipboard, parses it as TSV, and pastes the values
// starting at the cursor cell. The selection rect is set to the paste
// destination first (clipped to table bounds) so apply_op's single
// history record covers exactly the cells that changed.
void paste_clipboard_at_cursor(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value()
        || !state.selection.enabled) {
        return;
    }
    char const *clip = ImGui::GetClipboardText();
    if (clip == nullptr || *clip == '\0') return;
    auto grid = parse_tsv(std::string_view{clip});
    if (grid.empty() || grid.front().empty()) return;

    auto &td = *state.current_table_data;
    std::size_t const cur_r = state.selection.r_cursor;
    std::size_t const cur_c = state.selection.c_cursor;
    if (cur_r >= td.values.size() || td.values[cur_r].empty()) return;

    // Clip the paste rect to actual table bounds (Excel-style truncate,
    // not wrap).
    std::size_t const grid_rows = grid.size();
    std::size_t       grid_cols = 0;
    for (auto const &row : grid) {
        if (row.size() > grid_cols) grid_cols = row.size();
    }
    std::size_t const r1 =
        std::min<std::size_t>(cur_r + grid_rows - 1, td.values.size() - 1);
    std::size_t const c1 =
        std::min<std::size_t>(cur_c + grid_cols - 1,
                               td.values[cur_r].size() - 1);

    // Snap selection to the paste destination so apply_op picks the
    // right rect.
    state.selection.r_anchor = cur_r;
    state.selection.r_cursor = r1;
    state.selection.c_anchor = cur_c;
    state.selection.c_cursor = c1;

    apply_op(state, "paste",
             [&grid, cur_r, cur_c](auto &t, auto rect) -> st::Status {
                 for (std::size_t dr = 0;
                      dr < grid.size()
                      && cur_r + dr < t.values.size()
                      && cur_r + dr <= rect.r_end; ++dr) {
                     auto       &tt_row = t.values[cur_r + dr];
                     auto const &g_row  = grid[dr];
                     for (std::size_t dc = 0;
                          dc < g_row.size()
                          && cur_c + dc < tt_row.size()
                          && cur_c + dc <= rect.c_end; ++dc) {
                         tt_row[cur_c + dc] = g_row[dc];
                     }
                 }
                 return st::ok();
             });
}

// Read the source ROM's values for the selection and copy them onto
// the working ROM via apply_op. One history entry per call so a single
// Ctrl+Z restores the user's edits in that region. Useful workflow:
// "I edited a corner of this map and don't like it — revert just
// those cells, not the whole table."
void reset_selection_to_source(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value()
        || !state.selection.enabled) {
        return;
    }
    auto const *tbl = state.project->definition().find_table(
        state.selected_table_id);
    if (tbl == nullptr) {
        state.status_msg = "Reset: table missing from pack";
        return;
    }
    // Read the source ROM through the same scaling pipeline as the
    // working ROM — so what we copy back is the exact value a fresh
    // open of the source would show, not raw bytes.
    auto src_td = state.project->definition().read_table_values(
        state.project->source_rom(), *tbl);
    if (!src_td.has_value()) {
        state.status_msg =
            "Reset: read source: " + src_td.error().to_string();
        return;
    }
    auto const rect = state.selection.as_rect();
    apply_op(state, "reset to source",
             [&src = *src_td, rect](auto &t, auto r) -> st::Status {
                 for (std::size_t row = r.r_start;
                      row <= r.r_end && row < t.values.size()
                      && row < src.values.size(); ++row) {
                     for (std::size_t col = r.c_start;
                          col <= r.c_end && col < t.values[row].size()
                          && col < src.values[row].size(); ++col) {
                         t.values[row][col] = src.values[row][col];
                     }
                 }
                 (void) rect;  // rect == r when apply_op fires
                 return st::ok();
             });
}

void do_undo(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto const *e = state.project->history().undo();
    if (e != nullptr) {
        apply_history_step(state, *e, /*forward=*/false);
    }
}

void do_redo(AppState &state) {
    if (!state.project.has_value()) {
        return;
    }
    auto const *e = state.project->history().redo();
    if (e != nullptr) {
        apply_history_step(state, *e, /*forward=*/true);
    }
}

// ---------------------------------------------------------------------
// Unsaved-changes guard.
//
// `request_action` is the single entry point for any user gesture that
// would lose in-memory edits if executed directly: Open Project (which
// replaces the current project), Open a recent (same), Close Project,
// and Quit (Ctrl+Q / menu / window-X click). When the project has
// pending edits, the modal opens and the action is queued; otherwise
// the action runs immediately. `execute_action` performs the action.
// ---------------------------------------------------------------------

void execute_action(AppState &state, ConfirmAction action,
                    std::filesystem::path const &path) {
    switch (action) {
        case ConfirmAction::None:
            break;
        case ConfirmAction::OpenDialog:
            open_project_dialog(state);
            break;
        case ConfirmAction::OpenRecent:
            state.try_open_project(path);
            break;
        case ConfirmAction::Close:
            state.close_project();
            break;
        case ConfirmAction::Quit:
            state.user_confirmed_quit = true;
            break;
    }
}

void request_action(AppState &state, ConfirmAction action,
                    std::filesystem::path path = {}) {
    if (action == ConfirmAction::None) return;
    bool const need_confirm = state.dirty && state.project.has_value();
    if (need_confirm) {
        state.next_action       = action;
        state.next_recent       = std::move(path);
        state.show_unsaved_modal = true;
    } else {
        execute_action(state, action, path);
    }
}

// Action-specific labels for the unsaved-changes modal. Keeping these
// pure verbs scoped to the modal so they don't accidentally read like
// general "open / close / quit" handlers elsewhere.
char const *modal_save_label(ConfirmAction a) noexcept {
    switch (a) {
        case ConfirmAction::OpenDialog:
        case ConfirmAction::OpenRecent: return "Save and open";
        case ConfirmAction::Close:      return "Save and close";
        case ConfirmAction::Quit:       return "Save and quit";
        case ConfirmAction::None:       break;
    }
    return "Save and continue";
}

char const *modal_discard_label(ConfirmAction a) noexcept {
    switch (a) {
        case ConfirmAction::OpenDialog:
        case ConfirmAction::OpenRecent: return "Discard and open";
        case ConfirmAction::Close:      return "Discard and close";
        case ConfirmAction::Quit:       return "Discard and quit";
        case ConfirmAction::None:       break;
    }
    return "Discard changes";
}

char const *modal_subtitle(ConfirmAction a) noexcept {
    switch (a) {
        case ConfirmAction::OpenDialog:
        case ConfirmAction::OpenRecent:
            return "Opening another project will replace this one.";
        case ConfirmAction::Close:
            return "Closing this project will reset the editor.";
        case ConfirmAction::Quit:
            return "Quitting will exit SubuwuTuner.";
        case ConfirmAction::None:
            break;
    }
    return "Continuing without saving will discard them.";
}

void render_unsaved_modal(AppState &state) {
    if (state.show_unsaved_modal) {
        ImGui::OpenPopup("Unsaved changes##unsaved");
        state.show_unsaved_modal = false;
    }
    ImVec2 const center =
        ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                             ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes##unsaved", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ConfirmAction const what = state.next_action;

        ImGui::TextUnformatted("You have unsaved edits in this project.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("%s", modal_subtitle(what));
        ImGui::Dummy(ImVec2(0.0f, 16.0f));

        // Keyboard shortcuts: Enter = the safe default (Save).
        // Esc = the safe undo (Cancel). Destructive Discard
        // requires an explicit click — no accelerator on purpose.
        bool const want_save =
            ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false)
            || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
        bool const want_cancel =
            ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);

        constexpr float kBtnW = 180.0f;
        // Save is the default action (Enter) and the safe path — give it
        // accent fill so the eye lands on it first.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.21f, 0.46f, 0.76f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.31f, 0.56f, 0.86f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.65f, 0.94f, 1.00f));
        bool const save_clicked =
            ImGui::Button(modal_save_label(what), ImVec2(kBtnW, 0.0f));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write the working ROM + edits to disk, "
                              "then proceed.  (Enter)");
        }
        ImGui::SameLine();
        bool const discard_clicked =
            ImGui::Button(modal_discard_label(what), ImVec2(kBtnW, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Throw away every edit since the last save "
                              "and proceed.");
        }
        ImGui::SameLine();
        bool const cancel_clicked =
            ImGui::Button("Cancel", ImVec2(kBtnW * 0.7f, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stay here. Don't open, close, or quit.  "
                              "(Esc)");
        }

        if (save_clicked || want_save) {
            save_project(state);
            execute_action(state, state.next_action, state.next_recent);
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        } else if (discard_clicked) {
            state.dirty       = false;
            execute_action(state, state.next_action, state.next_recent);
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        } else if (cancel_clicked || want_cancel) {
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Build the FlashPlan + PolicyDecision for the currently-loaded project.
// Returns nullopt if there's no project, no delta, or a size mismatch.
struct PendingFlash {
    st::flash::FlashPlan       plan;
    st::flash::PolicyDecision  decision;
    std::size_t                total_bytes;
};

std::optional<PendingFlash> build_pending_flash(AppState const &state) {
    if (!state.project.has_value()) return std::nullopt;
    auto const &proj = *state.project;
    if (proj.source_rom().size() != proj.working_rom().size()) return std::nullopt;

    constexpr std::uint32_t kSectorSize  = 0x1000;
    constexpr std::uint32_t kBaseAddress = 0;
    auto const sectors = st::flash::Flasher::compute_delta(
        proj.source_rom().data(), proj.working_rom().data(),
        kSectorSize, kBaseAddress);
    if (sectors.empty()) return std::nullopt;

    PendingFlash pf;
    pf.total_bytes = 0;
    pf.plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite sw;
        sw.sector = s;
        std::size_t const off = static_cast<std::size_t>(s.address - kBaseAddress);
        sw.data.assign(
            proj.working_rom().data().begin() + static_cast<std::ptrdiff_t>(off),
            proj.working_rom().data().begin()
                + static_cast<std::ptrdiff_t>(off + s.length));
        pf.total_bytes += s.length;
        pf.plan.writes.push_back(std::move(sw));
    }
    pf.decision = st::flash::evaluate_plan_policy(
        pf.plan, proj.definition(), proj.source_rom().data(),
        proj.policy_profile());
    return pf;
}

void render_flash_modal(AppState &state) {
    if (state.show_flash_modal) {
        ImGui::OpenPopup("Flash...##flash_modal");
        state.show_flash_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                             ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Flash...##flash_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto const pending = build_pending_flash(state);

    if (!state.project.has_value()) {
        ImGui::TextUnformatted("No project loaded.");
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))
                || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    if (!pending.has_value()) {
        ImGui::TextUnformatted("Working ROM matches source — nothing to flash.");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))
                || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    auto const &d        = pending->decision;
    auto const  profile  = state.project->policy_profile();
    auto const  pname    = std::string{st::policy::profile_name(profile)};
    using A              = st::policy::Action;

    // Header: plan stats.
    ImGui::Text("Sectors: %zu   Bytes: %zu   Profile: %s",
                pending->plan.writes.size(),
                pending->total_bytes, pname.c_str());
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // Engine-safety is a hard refusal across every profile.
    if (!d.engine_safety_tables.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
        ImGui::TextUnformatted("REFUSED: engine-safety-critical tables in plan");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        for (auto const &id : d.engine_safety_tables) {
            ImGui::BulletText("%s", id.c_str());
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled(
            "Engine-safety violations block in every profile (docs/06).");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))
                || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // Emissions-flagged list (informational; the action below decides what
    // the user has to do about it).
    if (!d.emissions_tables.empty()) {
        ImGui::TextUnformatted("Emissions-relevant tables in plan:");
        ImGui::Indent();
        for (auto const &id : d.emissions_tables) {
            ImGui::BulletText("%s", id.c_str());
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    } else {
        ImGui::TextDisabled("No emissions-flagged tables touched.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    // Action-specific UI: silent / confirm / confirm+reason.
    bool ready_to_send = true;
    switch (d.overall_action) {
        case A::Silent:
        case A::Badge:
            // The profile demands no user interaction — just go.
            ImGui::TextDisabled(
                "Profile '%s' raises no gate for this plan.", pname.c_str());
            break;
        case A::Warn:
            ImGui::TextDisabled(
                "Profile '%s' would flag this on save; no flash-time gate.",
                pname.c_str());
            break;
        case A::Confirm:
            ImGui::Checkbox("I confirm flashing these emissions edits",
                             &state.flash_confirm_checked);
            ready_to_send = state.flash_confirm_checked;
            break;
        case A::ConfirmWithReason:
            ImGui::Checkbox("I confirm flashing these emissions edits",
                             &state.flash_confirm_checked);
            ImGui::TextUnformatted("Reason (required):");
            ImGui::InputTextMultiline("##flash_reason",
                                       state.flash_reason,
                                       sizeof state.flash_reason,
                                       ImVec2(-FLT_MIN, 60.0f));
            ready_to_send = state.flash_confirm_checked
                            && state.flash_reason[0] != '\0';
            break;
        case A::Block:
            // Distinct from the engine-safety branch above: profile-level
            // Block, e.g. a hypothetical future strict profile.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
            ImGui::TextUnformatted("REFUSED by policy.");
            ImGui::PopStyleColor();
            ready_to_send = false;
            break;
    }

    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // Buttons. "Send to ECU" is intentionally never enabled in this build:
    // the GUI doesn't have a transport binding yet. The dry-run "Verify"
    // button completes the gate workflow without contacting hardware.
    ImGui::BeginDisabled(!ready_to_send);
    if (ImGui::Button("Verify policy", ImVec2(140.0f, 0.0f))) {
        state.status_msg = "Flash plan cleared policy gate ("
                         + pname + ").";
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (ready_to_send) {
            ImGui::SetTooltip(
                "Acknowledge that the plan cleared the policy gate.\n"
                "No bytes are sent to any ECU.");
        } else {
            ImGui::SetTooltip(
                "Tick the confirm box (and fill the reason) first.");
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(true);
    ImGui::Button("Send to ECU", ImVec2(140.0f, 0.0f));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Real transport not yet wired. Use the CLI for a MockTransport-\n"
            "replayed UDS trace: subuwutuner-cli project-flash <dir> --trace …");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))
            || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

// Format the rect of `td` as TSV (rows on lines, cells tab-separated)
// and put it on the system clipboard via ImGui's clipboard helper.
// Format matches Excel/Sheets clipboard convention so pasted values
// round-trip cleanly to a spreadsheet for batch analysis.
void copy_rect_to_clipboard(st::Definition::TableData const &td,
                             st::edit::Rect const             &rect,
                             int                              precision) {
    std::string out;
    char        buf[32];
    for (std::size_t r = rect.r_start;
         r <= rect.r_end && r < td.values.size(); ++r) {
        bool first = true;
        for (std::size_t c = rect.c_start;
             c <= rect.c_end && c < td.values[r].size(); ++c) {
            if (!first) out.push_back('\t');
            first = false;
            std::snprintf(buf, sizeof buf, "%.*f", precision, td.values[r][c]);
            out += buf;
        }
        out.push_back('\n');
    }
    ImGui::SetClipboardText(out.c_str());
}

// Menubar wrapper around copy_rect_to_clipboard. Pulls scaling precision
// off the current selected table so the clipboard formatting matches the
// grid. Edit menu and Ctrl+C both route through here.
void do_copy_selection(AppState &state) {
    if (!state.project.has_value() || !state.current_table_data.has_value()
        || !state.selection.enabled) {
        return;
    }
    int         precision = 0;
    auto const *tbl       = state.project->definition().find_table(
        state.selected_table_id);
    if (tbl != nullptr) {
        auto const *scal =
            state.project->definition().find_scaling(tbl->scaling);
        if (scal != nullptr) {
            precision = scal->precision;
        }
    }
    copy_rect_to_clipboard(*state.current_table_data,
                           state.selection.as_rect(),
                           precision);
}

// Parse a TSV-ish payload (Excel/Sheets clipboard format) into a 2D
// grid of doubles. Tolerant of trailing newlines, mixed CRLF/LF, and
// non-numeric cells (those become 0.0). Empty input returns an empty
// grid.
std::vector<std::vector<double>> parse_tsv(std::string_view text) {
    std::vector<std::vector<double>> grid;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t line_end = i;
        while (line_end < text.size()
               && text[line_end] != '\n' && text[line_end] != '\r') {
            ++line_end;
        }
        std::string_view line{text.data() + i, line_end - i};
        std::vector<double> row;
        std::size_t cs = 0;
        while (cs <= line.size()) {
            std::size_t ce = cs;
            while (ce < line.size() && line[ce] != '\t') ++ce;
            std::string_view cell{line.data() + cs, ce - cs};
            // Trim whitespace at both ends.
            while (!cell.empty()
                   && std::isspace(static_cast<unsigned char>(cell.front()))) {
                cell.remove_prefix(1);
            }
            while (!cell.empty()
                   && std::isspace(static_cast<unsigned char>(cell.back()))) {
                cell.remove_suffix(1);
            }
            double v = 0.0;
            if (!cell.empty()) {
                std::string tmp{cell};
                (void) std::sscanf(tmp.c_str(), "%lf", &v);
                // sscanf failure leaves v at 0; tolerate so a stray
                // empty cell doesn't fail the whole paste.
            }
            row.push_back(v);
            if (ce >= line.size()) break;
            cs = ce + 1;
        }
        if (!row.empty()) grid.push_back(std::move(row));
        // Advance past the newline (handle both CRLF and LF).
        if (line_end < text.size()) {
            if (text[line_end] == '\r'
                && line_end + 1 < text.size()
                && text[line_end + 1] == '\n') {
                line_end += 2;
            } else {
                line_end += 1;
            }
        }
        i = line_end;
    }
    return grid;
}

// Case-insensitive substring search. Used by the sidebar table filter
// to match against both human-readable name and the snake_case id.
// Empty needle matches everything (so an empty filter shows the full
// list). ASCII-only — calibration table names use ASCII identifiers.
bool icontains(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    auto const eq = [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (!eq(static_cast<unsigned char>(hay[i + j]),
                    static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// Probe a few candidate paths and load the first one that exists. Returns
// nullptr if none was loadable, in which case ImGui's default font is used.
ImFont *load_first_existing(std::initializer_list<char const *> candidates,
                            float                               size_px) {
    auto &io = ImGui::GetIO();
    for (auto const *path : candidates) {
        if (path == nullptr) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (auto *f = io.Fonts->AddFontFromFileTTF(path, size_px); f != nullptr) {
            return f;
        }
    }
    return nullptr;
}

Fonts load_fonts() {
    Fonts f;
    // UI font — sans for menus, panels, labels. Tries Inter from a bundled
    // assets/ dir first (drop in to get the polished look), then a sane
    // system font per platform, finally falls back to ImGui's default.
    f.ui = load_first_existing({
                                   "assets/fonts/Inter-Regular.ttf",
                                   "C:/Windows/Fonts/segoeui.ttf",
                                   "/System/Library/Fonts/Helvetica.ttc",
                                   "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                               },
                               15.0f);

    // Mono — for grids, hex dumps, log output where alignment matters.
    f.mono = load_first_existing({
                                     "assets/fonts/JetBrainsMono-Regular.ttf",
                                     "C:/Windows/Fonts/consola.ttf",
                                     "/System/Library/Fonts/Menlo.ttc",
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                 },
                                 14.0f);

    if (f.ui == nullptr) {
        ImGui::GetIO().Fonts->AddFontDefault();
    }
    return f;
}

// Custom dark palette tuned for long tuning sessions: high contrast for the
// numerical grids, low chroma so the chrome reads as neutral. Accent colour
// nods to Subaru rally blue without dominating.
void apply_theme() {
    auto &s = ImGui::GetStyle();

    s.WindowPadding        = ImVec2(10.0f, 10.0f);
    s.FramePadding         = ImVec2(8.0f, 5.0f);
    s.CellPadding          = ImVec2(6.0f, 4.0f);
    s.ItemSpacing          = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing     = ImVec2(6.0f, 6.0f);
    s.IndentSpacing        = 20.0f;
    s.ScrollbarSize        = 14.0f;
    s.GrabMinSize          = 12.0f;
    s.WindowBorderSize     = 1.0f;
    s.ChildBorderSize      = 1.0f;
    s.PopupBorderSize      = 1.0f;
    s.FrameBorderSize      = 0.0f;
    s.TabBorderSize        = 0.0f;
    s.WindowRounding       = 4.0f;
    s.ChildRounding        = 4.0f;
    s.FrameRounding        = 3.0f;
    s.PopupRounding        = 4.0f;
    s.GrabRounding         = 3.0f;
    s.TabRounding          = 3.0f;
    s.ScrollbarRounding    = 8.0f;
    s.WindowTitleAlign     = ImVec2(0.0f, 0.5f);
    s.DockingSeparatorSize = 1.0f;

    constexpr ImVec4 accent       (0.21f, 0.46f, 0.76f, 1.00f);
    constexpr ImVec4 accent_hover (0.31f, 0.56f, 0.86f, 1.00f);
    constexpr ImVec4 accent_active(0.38f, 0.65f, 0.94f, 1.00f);

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.13f, 0.14f, 0.16f, 0.98f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text]                  = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.28f, 0.45f, 0.71f, 0.45f);

    c[ImGuiCol_FrameBg]               = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.27f, 0.30f, 0.35f, 1.00f);

    c[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.31f, 0.34f, 0.39f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.42f, 0.49f, 1.00f);

    c[ImGuiCol_CheckMark]             = accent_active;
    c[ImGuiCol_SliderGrab]            = accent;
    c[ImGuiCol_SliderGrabActive]      = accent_active;

    c[ImGuiCol_Button]                = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive]          = accent;
    c[ImGuiCol_Header]                = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_HeaderActive]          = accent;
    c[ImGuiCol_Separator]             = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorHovered]      = accent_hover;
    c[ImGuiCol_SeparatorActive]       = accent_active;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]     = accent_hover;
    c[ImGuiCol_ResizeGripActive]      = accent_active;

    c[ImGuiCol_Tab]                   = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]            = accent_hover;
    c[ImGuiCol_TabSelected]           = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]   = accent;
    c[ImGuiCol_TabDimmed]             = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);

    c[ImGuiCol_DockingPreview]        = ImVec4(0.21f, 0.46f, 0.76f, 0.70f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    c[ImGuiCol_PlotLines]             = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = accent_hover;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_PlotHistogramHovered]  = accent_hover;

    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    // NavCursor was previously `accent`, which made it render as a
    // bright outline around whichever widget last received focus.
    // Combined with viewports/docking, this manifested as "one cell
    // is highlighted and never unhighlights" in the data grid — even
    // after disabling NavEnableKeyboard, the cursor color was still
    // being applied wherever the nav system happened to land. We
    // don't ship any explicit nav-focus indicator, so making this
    // fully transparent is harmless and removes the offending visual.
    c[ImGuiCol_NavCursor]             = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    // With viewports enabled, OS-level windows render with their own alpha;
    // force fully-opaque WindowBg so detached panels don't show through to
    // the desktop.
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        c[ImGuiCol_WindowBg].w = 1.0f;
    }
}

constexpr float kStatusBarHeight = 26.0f;

// One transparent host window covering the work area (minus our manual status
// bar) hosts the dockspace. ImGuiDockNodeFlags_PassthruCentralNode keeps the
// empty central area transparent so the GL clear color shows through.
void render_dockspace_host() {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(
        ImVec2(vp->WorkSize.x, vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    auto const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                     | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoBringToFrontOnFocus
                     | ImGuiWindowFlags_NoNavFocus
                     | ImGuiWindowFlags_NoBackground
                     | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##dockspace_host", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID const id = ImGui::GetID("MainDockSpace");

    // On first ever run (no imgui.ini), set up a default split: Tables on the
    // left, Table view central. Once the user has a saved layout it's
    // respected automatically.
    if (ImGui::DockBuilderGetNode(id) == nullptr) {
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);

        ImGuiID left  = 0;
        ImGuiID middle = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.22f, &left, &middle);
        ImGuiID right = 0;
        ImGuiID center = 0;
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, 0.25f, &right, &center);

        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Table",  center);
        ImGui::DockBuilderDockWindow("Stats",  right);

        ImGui::DockBuilderFinish(id);
    }

    ImGui::DockSpace(id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

void render_menubar(AppState &state) {
    bool const has_project = state.project.has_value();
    bool const can_undo    = has_project && state.project->history().can_undo();
    bool const can_redo    = has_project && state.project->history().can_redo();

    // Tooltip on a menu item even when it's disabled — so the user
    // understands WHY it's grayed out rather than just seeing the
    // affordance and wondering. AllowWhenDisabled is the hover flag
    // that makes IsItemHovered fire on a disabled item.
    auto const disabled_tip = [](char const *body) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", body);
        }
    };

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
                request_action(state, ConfirmAction::OpenDialog);
            }
            if (ImGui::MenuItem("Save Project", "Ctrl+S", false, has_project)) {
                save_project(state);
            }
            if (!has_project) {
                disabled_tip("No project open — there's nothing to save.\n"
                             "Open a project first (Ctrl+O).");
            }
            if (ImGui::MenuItem("Close Project", nullptr, false, has_project)) {
                request_action(state, ConfirmAction::Close);
            }
            if (!has_project) {
                disabled_tip("No project open.");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
                request_action(state, ConfirmAction::Quit);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit", has_project)) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, can_undo)) {
                do_undo(state);
            }
            if (has_project && !can_undo) {
                disabled_tip("Nothing to undo — no edits have been made.");
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, can_redo)) {
                do_redo(state);
            }
            if (has_project && !can_redo) {
                disabled_tip("Nothing to redo.\n"
                             "Use Undo first, then Redo to step forward.");
            }
            ImGui::Separator();
            bool const has_selection = state.selection.enabled;
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_selection)) {
                do_copy_selection(state);
            }
            if (has_project && !has_selection) {
                disabled_tip("Select cells in the grid first.");
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, has_selection)) {
                paste_clipboard_at_cursor(state);
            }
            if (has_project && !has_selection) {
                disabled_tip("Select a target cell, then paste TSV from the\n"
                             "clipboard at the cursor.");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset to source", nullptr, false,
                                 has_selection)) {
                reset_selection_to_source(state);
            }
            if (has_project && !has_selection) {
                disabled_tip("Reverts the selected cells to their source-ROM\n"
                             "values (undoable).");
            }
            ImGui::EndMenu();
        }
        if (!has_project
            && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            // Edit menu itself is disabled (BeginMenu second arg = false).
            ImGui::SetTooltip("No project open — open one to enable editing.");
        }
        if (ImGui::BeginMenu("View")) {
            bool const is_grid = state.view_mode == TableViewMode::Grid;
            bool const is_heat = state.view_mode == TableViewMode::Heatmap;
            if (ImGui::MenuItem("Grid", nullptr, is_grid, has_project)) {
                state.view_mode = TableViewMode::Grid;
            }
            if (ImGui::MenuItem("Heatmap", nullptr, is_heat, has_project)) {
                state.view_mode = TableViewMode::Heatmap;
            }
            ImGui::Separator();
            // Dev-only escape hatch. Tucked one level deeper so the
            // ImGui example isn't a peer of the user-facing view modes.
            if (ImGui::BeginMenu("Debug")) {
                ImGui::MenuItem("ImGui demo window", nullptr,
                                 &state.show_imgui_demo);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("SubuwuTuner %.*s",
                        static_cast<int>(st::Version::string().size()),
                        st::Version::string().data());
            ImGui::Separator();
            ImGui::TextDisabled("Getting started");
            ImGui::BulletText("File \xE2\x86\x92 Open Project\xE2\x80\xA6 (Ctrl+O) to pick a .stune directory.");
            ImGui::BulletText("Or pass one on the command line: subuwutuner-gui my.stune");
            ImGui::Separator();
            ImGui::TextDisabled("Editing");
            ImGui::BulletText("Click cells to select; Shift-click to extend.");
            ImGui::BulletText("Arrow keys move the cursor; Shift+arrows extend.");
            ImGui::BulletText("F2 or double-click a cell to type a new value.  Enter commits, Esc cancels.");
            ImGui::BulletText("Ctrl+Enter while editing fills every selected cell with the typed value.");
            ImGui::BulletText("Ctrl+C / Ctrl+V copy and paste the selection as tab-separated values.");
            ImGui::BulletText("Right-click any cell for Copy / Paste / Reset to source.");
            ImGui::BulletText("Toolbar buttons (+5%%, -5%%, Smooth, Interpolate) act on the selection.");
            ImGui::BulletText("Ctrl+Z / Ctrl+Shift+Z to undo / redo.  Ctrl+S to save.");
            ImGui::Separator();
            ImGui::TextDisabled("Navigation");
            ImGui::BulletText("Ctrl+F focuses the table-filter box.  Esc clears it.");
            ImGui::BulletText("Filter matches both the table's name and its snake_case id.");
            ImGui::Separator();
            ImGui::TextDisabled("Viewing");
            ImGui::BulletText("Switch View: Grid \xE2\x86\x94 Heatmap to inspect a map two ways.");
            ImGui::BulletText("For 3D tables, pick a Z slice above the grid.");
            ImGui::Separator();
            ImGui::TextDisabled("Documentation");
            ImGui::BulletText("Repo:    https://github.com/BuffJesus/SubuwuTuner");
            ImGui::BulletText("Design:  docs/00-overview.md \xE2\x80\xA6 docs/16-custom-features.md");
            ImGui::BulletText("License: Apache 2.0 (see LICENSE in the repo root)");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void render_sidebar(AppState &state) {
    ImGui::Begin("Tables");

    if (!state.project.has_value()) {
        // Quiet empty state. The welcome panel on the right owns the
        // primary Open Project CTA; the sidebar just acknowledges that
        // tables will appear here once a project is loaded.
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextDisabled("Tables will appear here.");
        ImGui::End();
        return;
    }

    auto const &def = state.project->definition();

    // Filter input. Ctrl+F (handled in main loop) hands keyboard focus
    // here for the next frame; Esc clears the buffer and unfocuses by
    // virtue of EscapeClearsAll. Width matches the full panel so the
    // affordance is unambiguous.
    if (state.focus_table_filter) {
        ImGui::SetKeyboardFocusHere();
        state.focus_table_filter = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##table_filter", "Filter tables…  (Ctrl+F)",
                              state.table_filter, sizeof state.table_filter,
                              ImGuiInputTextFlags_EscapeClearsAll);

    std::string_view const filter{state.table_filter};
    // Count matches once so the active-filter header line can report
    // "N of M". When the filter is empty, the count is noise — the tree
    // below already conveys "how many tables" at a glance.
    std::size_t matched = 0;
    for (auto const &t : def.tables()) {
        if (filter.empty()
            || icontains(t.name, filter) || icontains(t.id, filter)) {
            ++matched;
        }
    }
    if (!filter.empty()) {
        ImGui::TextDisabled("%zu of %zu tables", matched,
                             def.tables().size());
        ImGui::Separator();
    }

    if (!filter.empty() && matched == 0) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextWrapped("No tables match \"%s\".",
                            state.table_filter);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled("Try a shorter prefix, or clear the filter "
                             "(Esc).");
        ImGui::End();
        return;
    }

    // Group tables by category, preserving first-occurrence order so
    // pack authors get to control the group ordering. Tables without a
    // category fall into "Other"; that group lands wherever the first
    // uncategorized table appears (predictable, not magic-sorted).
    struct Group {
        std::string_view name;
        std::vector<std::size_t> indices;  // into def.tables()
    };
    std::vector<Group> groups;
    groups.reserve(8);
    auto const find_or_make = [&](std::string_view cat) -> Group & {
        for (auto &g : groups) {
            if (g.name == cat) return g;
        }
        groups.push_back({cat, {}});
        return groups.back();
    };
    for (std::size_t i = 0; i < def.tables().size(); ++i) {
        auto const &t = def.tables()[i];
        std::string_view const cat =
            t.category.empty() ? std::string_view{"Other"}
                                : std::string_view{t.category};
        find_or_make(cat).indices.push_back(i);
    }

    auto const table_matches = [&](st::Table const &t) {
        return filter.empty()
               || icontains(t.name, filter) || icontains(t.id, filter);
    };

    auto const render_table_row = [&](st::Table const &t) {
        bool const selected = state.selected_table_id == t.id;
        // Prefer the human-readable name as the primary label.
        // Snake-case IDs are developer-facing — surface them in the
        // tooltip instead.
        char const *label = t.name.empty() ? t.id.c_str() : t.name.c_str();
        ImGui::PushID(t.id.c_str());
        if (ImGui::Selectable(label, selected,
                              ImGuiSelectableFlags_AllowOverlap)) {
            state.select_table(t.id);
        }
        // Right-aligned policy badges: S (engine-safety-critical, warm
        // amber) and E (emissions-relevant, muted yellow). Drawn AFTER
        // the Selectable so AllowOverlap is needed; reads "tagged" at a
        // glance while browsing.
        if (t.engine_safety_critical || t.emissions_relevant) {
            char buf[4]{};
            std::size_t bi = 0;
            if (t.engine_safety_critical) buf[bi++] = 'S';
            if (t.emissions_relevant)     buf[bi++] = 'E';
            float const w = ImGui::CalcTextSize(buf).x;
            float const right_x =
                ImGui::GetWindowContentRegionMax().x - w
                - ImGui::GetStyle().FramePadding.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_x);
            ImVec4 const color = t.engine_safety_critical
                ? ImVec4(1.00f, 0.86f, 0.55f, 1.0f)
                : ImVec4(0.96f, 0.94f, 0.65f, 1.0f);
            ImGui::TextColored(color, "%s", buf);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (!t.name.empty()) {
                ImGui::TextUnformatted(t.name.c_str());
                ImGui::TextDisabled("%s", t.id.c_str());
            } else {
                ImGui::TextUnformatted(t.id.c_str());
            }
            ImGui::Separator();
            ImGui::Text("%dD  \xC2\xB7  0x%08zX", t.dimensions, t.address);
            if (!t.category.empty()) {
                ImGui::TextDisabled("category: %s", t.category.c_str());
            }
            if (t.engine_safety_critical) {
                ImGui::TextColored(ImVec4(1.00f, 0.86f, 0.55f, 1.0f),
                                   "engine safety critical");
            } else if (t.emissions_relevant) {
                ImGui::TextColored(ImVec4(0.96f, 0.94f, 0.65f, 1.0f),
                                   "emissions-relevant");
            }
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    };

    for (auto const &g : groups) {
        // Count matches in this group up-front so the header line can
        // report it AND so we can skip an entirely-filtered-out group
        // (don't render an empty TreeNode that just clutters the panel).
        std::size_t group_matched = 0;
        for (auto idx : g.indices) {
            if (table_matches(def.tables()[idx])) {
                ++group_matched;
            }
        }
        if (group_matched == 0) continue;

        char header[96];
        if (filter.empty()) {
            std::snprintf(header, sizeof header, "%.*s (%zu)",
                          static_cast<int>(g.name.size()), g.name.data(),
                          g.indices.size());
        } else {
            std::snprintf(header, sizeof header, "%.*s (%zu of %zu)",
                          static_cast<int>(g.name.size()), g.name.data(),
                          group_matched, g.indices.size());
        }

        // When filtering, force the group open so matches are always
        // visible. Otherwise default-open on first run; the user can
        // collapse manually and that state persists via imgui.ini.
        if (!filter.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        ImGuiTreeNodeFlags const tn_flags =
            ImGuiTreeNodeFlags_DefaultOpen
            | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (ImGui::TreeNodeEx(header, tn_flags)) {
            for (auto idx : g.indices) {
                auto const &t = def.tables()[idx];
                if (!table_matches(t)) continue;
                render_table_row(t);
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();
}

struct GridStats {
    double      min{0.0};
    double      max{0.0};
    double      mean{0.0};
    std::size_t count{0};
};

GridStats compute_stats(st::Definition::TableData const &td) {
    GridStats   s;
    double      lo  = std::numeric_limits<double>::infinity();
    double      hi  = -std::numeric_limits<double>::infinity();
    double      sum = 0.0;
    std::size_t n   = 0;
    for (auto const &row : td.values) {
        for (auto const v : row) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
            ++n;
        }
    }
    if (n > 0) {
        s.min   = lo;
        s.max   = hi;
        s.mean  = sum / static_cast<double>(n);
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
    t        = std::clamp(t, 0.0, 1.0);

    auto const lerp = [](double a, double b, double f) {
        return a + (b - a) * f;
    };
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
    if (t < 0.5) {
        double const s = t * 2.0;
        r = lerp(45.0, 20.0, s);
        g = lerp(80.0, 22.0, s);
        b = lerp(140.0, 26.0, s);
        a = lerp(70.0, 0.0, s);  // cold end max ~28%, halved from prior 55%
    } else {
        double const s = (t - 0.5) * 2.0;
        r = lerp(20.0, 180.0, s);
        g = lerp(22.0, 90.0, s);
        b = lerp(26.0, 50.0, s);
        a = lerp(0.0, 140.0, s);  // warm end unchanged
    }
    return IM_COL32(static_cast<int>(r), static_cast<int>(g),
                    static_cast<int>(b), static_cast<int>(a));
}

// ImGui table cells default to left-aligned text. For numerical grids,
// right-alignment so decimal points line up across rows is more legible.
void text_right_aligned(char const *text) {
    float const w     = ImGui::CalcTextSize(text).x;
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
void render_table_heatmap(st::Definition::TableData const &td,
                          st::Table const *                tbl,
                          st::Scaling const *              scal,
                          GridStats const &                stats) {
    if (td.values.empty() || td.values.front().empty()) {
        ImGui::TextDisabled("(no values)");
        return;
    }

    auto const rows = td.values.size();
    auto const cols = td.values.front().size();

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
                                std::vector<double>       &positions,
                                std::vector<std::string>  &labels,
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
    std::vector<double>       x_pos, y_pos;
    std::vector<std::string>  x_lbl, y_lbl;
    std::vector<char const *> x_ptrs, y_ptrs;
    build_ticks(td.axis_x, x_pos, x_lbl, x_ptrs);
    build_ticks(td.axis_y, y_pos, y_lbl, y_ptrs);

    double const   min_v  = stats.min;
    double const   max_v  = (stats.max > stats.min) ? stats.max : stats.min + 1.0;
    int const      n_cells = static_cast<int>(rows * cols);
    char const    *fmt    = (n_cells <= 256) ? "%.1f" : nullptr;
    int const      prec   = scal != nullptr ? scal->precision : 1;
    char           fmt_buf[8];
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
    ImVec2 const    avail       = ImGui::GetContentRegionAvail();
    ImVec2 const    plot_size   = ImVec2(avail.x - kScaleWidth - 8.0f, avail.y);

    if (ImPlot::BeginPlot("##table_heatmap", plot_size,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText
                              | ImPlotFlags_NoTitle)) {
        auto const x_flags = ImPlotAxisFlags_NoGridLines;
        auto const y_flags = ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Invert;
        ImPlot::SetupAxes(
            (tbl != nullptr && tbl->axis_x.has_value()) ? tbl->axis_x->c_str() : nullptr,
            (tbl != nullptr && tbl->axis_y.has_value()) ? tbl->axis_y->c_str() : nullptr,
            x_flags, y_flags);

        if (!x_pos.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_X1, x_pos.data(),
                                   static_cast<int>(x_pos.size()), x_ptrs.data(),
                                   false);
        }
        if (!y_pos.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_Y1, y_pos.data(),
                                   static_cast<int>(y_pos.size()), y_ptrs.data(),
                                   false);
        }

        ImPlot::PlotHeatmap("##h", flat.data(),
                            static_cast<int>(rows), static_cast<int>(cols),
                            min_v, max_v, fmt,
                            ImPlotPoint(0, 0),
                            ImPlotPoint(static_cast<double>(cols),
                                        static_cast<double>(rows)));
        ImPlot::EndPlot();
    }

    ImGui::SameLine();
    ImPlot::ColormapScale("##scale", min_v, max_v, ImVec2(kScaleWidth, plot_size.y));
    ImPlot::PopColormap();
}

void render_table_grid(st::Definition::TableData const &td,
                       st::Scaling const *             scal,
                       GridStats const &               stats,
                       Selection &                     selection,
                       Fonts const &                   fonts,
                       AppState &                      state,
                       std::vector<std::vector<bool>> const &edited_mask) {
    int const  precision = scal != nullptr ? scal->precision : 0;
    auto const cols      = static_cast<int>(td.axis_x.size()) + 1;
    if (cols < 2) {
        ImGui::TextDisabled("(table has no X axis)");
        return;
    }

    auto const grid_rows = td.values.size();
    auto const grid_cols_count = grid_rows == 0 ? std::size_t{0}
                                                 : td.values.front().size();

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
    if (!state.editing_cell && selection.enabled
        && grid_rows > 0 && grid_cols_count > 0
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        bool const shift = ImGui::GetIO().KeyShift;
        bool       moved = false;
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
        // canonical "edit this cell" shortcut.
        if (ImGui::IsKeyPressed(ImGuiKey_F2, /*repeat=*/false)) {
            std::snprintf(state.edit_buf, sizeof state.edit_buf, "%.*f",
                          precision,
                          (selection.r_cursor < td.values.size()
                           && selection.c_cursor < td.values[selection.r_cursor].size())
                              ? td.values[selection.r_cursor][selection.c_cursor]
                              : 0.0);
            state.editing_cell       = true;
            state.editor_just_opened = true;
        }
        // Ctrl+C / Ctrl+V — TSV clipboard interop. Same gating as
        // arrow nav (Table window focused, not currently editing a
        // cell) so the cell editor's InputText sees Ctrl+C/V as text
        // operations when it's active.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copy_rect_to_clipboard(td, selection.as_rect(), precision);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
            paste_clipboard_at_cursor(state);
        }
    }

    auto const flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                     | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY
                     | ImGuiTableFlags_SizingFixedFit;

    // Grids are numerical — push monospace so column alignment is honest.
    // Right-align Selectable text so cells read like a calculator pad.
    // Selectable's selected background uses ImGuiCol_Header; override to
    // the accent at ~55% alpha so it overlays the heatmap instead of
    // hiding it.
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(1.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.21f, 0.46f, 0.76f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.31f, 0.56f, 0.86f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.38f, 0.65f, 0.94f, 0.65f));

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
    for (auto const x : td.axis_x) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", x);
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
    char       buf[32];
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
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            text_right_aligned(buf);
            ImGui::PopStyleColor();
        }
        for (std::size_t c = 0; c < td.values[r].size(); ++c) {
            double const v = td.values[r][c];
            ImGui::TableNextColumn();
            ImU32 const bg = heatmap_color(v, stats.min, stats.max);
            if (bg != 0u) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, bg);
            }
            std::snprintf(buf, sizeof(buf), "%.*f", precision, v);

            ImGui::PushID(static_cast<int>(r * grid_cols + c));
            bool const is_sel    = selection.contains(r, c);
            bool const is_cursor = selection.enabled
                                   && r == selection.r_cursor
                                   && c == selection.c_cursor;

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
                    "##cell_editor", state.edit_buf,
                    sizeof state.edit_buf,
                    ImGuiInputTextFlags_EnterReturnsTrue
                    | ImGuiInputTextFlags_AutoSelectAll
                    | ImGuiInputTextFlags_CharsScientific);
                bool const deactivated = ImGui::IsItemDeactivated();
                bool const escaped =
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false);

                auto const exit_edit = [&] {
                    state.editing_cell       = false;
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
                if (ImGui::Selectable(buf, is_sel,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selection.click(r, c, ImGui::GetIO().KeyShift);
                    // Double-click promotes the click into edit mode,
                    // mirroring Excel/Sheets. AllowDoubleClick on the
                    // Selectable above is what lets us see the second
                    // click here.
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::snprintf(state.edit_buf,
                                      sizeof state.edit_buf,
                                      "%.*f", precision, v);
                        state.editing_cell       = true;
                        state.editor_just_opened = true;
                    }
                }
                // Right-click selects the cell (if not already in the
                // selection) and opens a Copy/Paste context menu. The
                // implicit re-select matches Excel — right-clicking a
                // cell outside the current selection makes that cell
                // the new scope of the operation, rather than
                // silently using the previous selection.
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)
                    && !selection.contains(r, c)) {
                    selection.click(r, c, /*shift=*/false);
                }
                if (ImGui::BeginPopupContextItem("##cell_ctx")) {
                    bool const has_sel = selection.enabled;
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_sel)) {
                        copy_rect_to_clipboard(td, selection.as_rect(),
                                                precision);
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false, has_sel)) {
                        paste_clipboard_at_cursor(state);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset to source", nullptr,
                                         false, has_sel)) {
                        reset_selection_to_source(state);
                    }
                    if (ImGui::IsItemHovered(
                            ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip(
                            "Set the selected cells back to the values "
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
                r < edited_mask.size()
                && c < edited_mask[r].size()
                && edited_mask[r][c];
            if (edited) {
                ImVec2 const a = ImGui::GetItemRectMin();
                ImVec2 const b = ImGui::GetItemRectMax();
                if (b.x > a.x && b.y > a.y) {
                    auto * const dl = ImGui::GetWindowDrawList();
                    constexpr float pad   = 3.0f;
                    constexpr float r_dot = 2.5f;
                    ImVec2 const center(a.x + pad + r_dot,
                                         a.y + pad + r_dot);
                    dl->AddCircleFilled(center, r_dot,
                                         IM_COL32(190, 215, 255, 235));
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
                    auto * const dl = ImGui::GetWindowDrawList();
                    dl->AddRect(a, b,
                                 IM_COL32(120, 180, 250, 255),
                                 0.0f,
                                 0,
                                 2.0f);
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

// Center the current cursor's X for a piece of content of width `w` within
// the panel's current content region.
void center_cursor_x(float w) {
    float const avail = ImGui::GetContentRegionAvail().x;
    if (w < avail) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
    }
}

// Centered single-line text. Scale > 1.0 temporarily enlarges the font.
void text_centered(char const *text, float scale = 1.0f) {
    if (scale != 1.0f) {
        ImGui::SetWindowFontScale(scale);
    }
    center_cursor_x(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
    if (scale != 1.0f) {
        ImGui::SetWindowFontScale(1.0f);
    }
}

void text_centered_disabled(char const *text) {
    center_cursor_x(ImGui::CalcTextSize(text).x);
    ImGui::TextDisabled("%s", text);
}

// Small framed "tag" used to highlight a per-table attribute (unit, safety
// flag, …) without it competing with the title. Looks like a button but
// stays purely visual: the return value is ignored and the hover/active
// states match the resting state.
void chip(char const *text, ImVec4 fg, ImVec4 bg) {
    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bg);
    ImGui::PushStyleColor(ImGuiCol_Text,          fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8.0f, 2.0f));
    (void) ImGui::SmallButton(text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

// Common chip palettes — kept centrally so future flags pick from a small,
// coherent set rather than each call site rolling its own RGB.
inline ImVec4 chip_fg_accent()    { return ImVec4(0.79f, 0.88f, 1.00f, 1.0f); }
inline ImVec4 chip_bg_accent()    { return ImVec4(0.16f, 0.28f, 0.48f, 0.55f); }
inline ImVec4 chip_fg_warn()      { return ImVec4(1.00f, 0.86f, 0.55f, 1.0f); }
inline ImVec4 chip_bg_warn()      { return ImVec4(0.42f, 0.30f, 0.08f, 0.60f); }
inline ImVec4 chip_fg_caution()   { return ImVec4(0.96f, 0.94f, 0.65f, 1.0f); }
inline ImVec4 chip_bg_caution()   { return ImVec4(0.34f, 0.32f, 0.08f, 0.55f); }
inline ImVec4 chip_fg_muted()     { return ImVec4(0.78f, 0.80f, 0.82f, 1.0f); }
inline ImVec4 chip_bg_muted()     { return ImVec4(0.22f, 0.24f, 0.28f, 0.55f); }

// Cold-start panel — what the user sees before any project is loaded. The
// goal is welcoming, not utilitarian: clean type hierarchy, one obvious
// next action, no jargon above the fold.
void render_welcome_panel(AppState &state) {
    ImVec2 const avail = ImGui::GetContentRegionAvail();
    // Balance content vertically. On short panels (default window) the
    // welcome cluster sits in the upper third — first-run feel,
    // recents have room without scrolling. On tall panels (maximized,
    // multi-monitor) the cluster shifts toward vertical-center so it
    // doesn't float in a huge empty space.
    //
    // Heuristic: take whichever is larger between a 10%-of-panel
    // anchor and a "would-be centered if content is ~320px tall"
    // calculation scaled at 40% of the remainder. This keeps the
    // default-window layout unchanged but pushes content meaningfully
    // lower on a 1300+px panel.
    bool const  has_recents = !state.recents.empty();
    float const min_pad     = avail.y * (has_recents ? 0.10f : 0.22f);
    float const center_bias = (avail.y - 320.0f) * 0.40f;
    float const top_pad     = std::max(min_pad, center_bias);
    ImGui::Dummy(ImVec2(0.0f, top_pad));

    text_centered("SubuwuTuner", 2.4f);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    text_centered_disabled("Open a Subaru ECU calibration to read, edit, and save.");
    ImGui::Dummy(ImVec2(0.0f, 28.0f));

    constexpr float kBtnW = 240.0f;
    constexpr float kBtnH = 38.0f;
    center_cursor_x(kBtnW);
    if (ImGui::Button("Open Project…", ImVec2(kBtnW, kBtnH))) {
        request_action(state, ConfirmAction::OpenDialog);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a .stune project directory.  (Ctrl+O)");
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    text_centered_disabled("Ctrl+O");

    // Recents block. Empty list → render nothing here; first-run users
    // see the original clean welcome.
    if (has_recents) {
        ImGui::Dummy(ImVec2(0.0f, 28.0f));
        // Centered "Recent projects" rule. We draw it inside a fixed-
        // width region so multiple windows / wide screens don't make
        // the list stretch oddly across the viewport.
        constexpr float kRowW = 480.0f;
        center_cursor_x(kRowW);
        ImGui::BeginGroup();
        // Heading: regular (not TextDisabled) so it reads as a
        // section break against the dimmed path text beneath each
        // row. The hand-drawn separator below is bounded to kRowW —
        // ImGui::Separator() ignores group width and would span the
        // whole panel, which looked broken on a maximized window.
        ImGui::TextUnformatted("Recent projects");
        {
            ImVec2 const p   = ImGui::GetCursorScreenPos();
            auto * const dl  = ImGui::GetWindowDrawList();
            ImU32 const  col = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(p.x, p.y + 2.0f),
                        ImVec2(p.x + kRowW, p.y + 2.0f), col);
            ImGui::Dummy(ImVec2(kRowW, 4.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        // Snapshot indices to act on — modifying recents inside the
        // iteration (via try_open_project) would invalidate iterators.
        std::optional<std::size_t> clicked_idx;
        for (std::size_t i = 0; i < state.recents.size(); ++i) {
            auto const     &e         = state.recents[i];
            auto const      basename  = e.path.filename().empty()
                                            ? e.path.string()
                                            : e.path.filename().string();
            std::error_code ec;
            bool const      exists =
                std::filesystem::exists(e.path, ec);

            ImGui::PushID(static_cast<int>(i));
            // Each row is a button with two-line content (basename on
            // top, dimmed full path beneath). Dead entries are
            // disabled — visible so the user knows the project moved
            // rather than silently dropped.
            float const button_left_x = ImGui::GetCursorPosX();
            ImGui::BeginDisabled(!exists);
            if (ImGui::Button(basename.c_str(), ImVec2(kRowW, 0.0f))) {
                clicked_idx = i;
            }
            ImGui::EndDisabled();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (exists) {
                    ImGui::SetTooltip("%s\nOpened %s",
                                      e.path.string().c_str(),
                                      e.opened_at.c_str());
                } else {
                    ImGui::SetTooltip(
                        "%s\n\nPath no longer exists — the project may "
                        "have moved.\nOpen Project… to locate it manually.",
                        e.path.string().c_str());
                }
            }
            // Subtitle: dimmed full path + relative time, aligned under
            // the row. Center inside the button's kRowW span so the
            // subtitle shares the button's centerline; GetContentRegionAvail
            // here measures from the button's left edge to the panel's
            // right edge, which would land the subtitle off-center to the
            // right.
            std::string subtitle;
            if (exists) {
                subtitle = e.path.string();
                auto const rel = format_relative_time(e.opened_at);
                if (!rel.empty()) {
                    subtitle += "  ·  ";
                    subtitle += rel;
                }
            } else {
                subtitle = e.path.string() + "  (missing)";
            }
            float const text_w = ImGui::CalcTextSize(subtitle.c_str()).x;
            if (text_w < kRowW) {
                ImGui::SetCursorPosX(button_left_x + (kRowW - text_w) * 0.5f);
            } else {
                ImGui::SetCursorPosX(button_left_x);
            }
            ImGui::TextDisabled("%s", subtitle.c_str());
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::PopID();
        }
        ImGui::EndGroup();

        if (clicked_idx.has_value()) {
            // Capture by value: try_open_project mutates recents.
            auto const path = state.recents[*clicked_idx].path;
            request_action(state, ConfirmAction::OpenRecent, path);
        }
    }

    if (!state.status_msg.empty()) {
        ImGui::Dummy(ImVec2(0.0f, has_recents ? 16.0f : 32.0f));
        text_centered_disabled(state.status_msg.c_str());
    }
}

// Right-rail quick-stats panel for the currently-selected table.
// Min/max/mean/stddev across all cells, count of edited cells (diff
// against source), and an ImPlot histogram. Read-only; updates each
// frame the table data is loaded.
void render_stats_panel(AppState &state) {
    ImGui::Begin("Stats");
    if (!state.project.has_value()) {
        ImGui::TextDisabled("No project loaded.");
        ImGui::End();
        return;
    }
    if (state.selected_table_id.empty() || !state.current_table_data.has_value()) {
        ImGui::TextDisabled("Select a table to see its stats.");
        ImGui::End();
        return;
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        ImGui::TextDisabled("Selected table not found in pack.");
        ImGui::End();
        return;
    }
    auto const &td = *state.current_table_data;

    // Flatten cells for stats + histogram. Skip dim=0 scalar — stats of
    // one value are useless.
    std::vector<float> cells;
    cells.reserve(td.values.size() * (td.values.empty() ? 0 : td.values[0].size()));
    for (auto const &row : td.values) {
        for (auto v : row) cells.push_back(static_cast<float>(v));
    }
    if (cells.empty() || (cells.size() == 1 && table->dimensions == 0)) {
        ImGui::TextDisabled("Scalar table — stats N/A.");
        ImGui::Separator();
        if (!cells.empty()) {
            ImGui::Text("Value: %g", static_cast<double>(cells[0]));
        }
        ImGui::End();
        return;
    }

    float min = cells[0], max = cells[0];
    double sum = 0.0;
    for (auto v : cells) {
        if (v < min) min = v;
        if (v > max) max = v;
        sum += static_cast<double>(v);
    }
    double const mean = sum / static_cast<double>(cells.size());
    double sq = 0.0;
    for (auto v : cells) {
        double const d = static_cast<double>(v) - mean;
        sq += d * d;
    }
    double const stddev =
        cells.size() > 1 ? std::sqrt(sq / static_cast<double>(cells.size() - 1)) : 0.0;

    // Count edited cells (diff working vs source). Cheap: re-read source
    // table data once per frame. For 1D / 2D tables of moderate size this
    // is well under a millisecond.
    std::size_t edited = 0;
    auto const  source_td = state.project->definition().read_table_values(
        state.project->source_rom(), *table);
    if (source_td.has_value()
        && source_td->values.size() == td.values.size()) {
        for (std::size_t r = 0; r < td.values.size(); ++r) {
            if (source_td->values[r].size() != td.values[r].size()) continue;
            for (std::size_t c = 0; c < td.values[r].size(); ++c) {
                if (td.values[r][c] != source_td->values[r][c]) ++edited;
            }
        }
    }

    auto const *scal = state.project->definition().find_scaling(table->scaling);
    auto const  unit = (scal != nullptr) ? scal->unit : std::string{};
    auto const  prec = (scal != nullptr) ? scal->precision : 2;

    ImGui::Text("%s", table->id.c_str());
    if (!unit.empty()) ImGui::TextDisabled("unit: %s", unit.c_str());
    ImGui::Separator();

    auto stat_row = [&](char const *label, double v) {
        ImGui::Text("%-7s %.*f%s%s", label, prec, v,
                    unit.empty() ? "" : " ", unit.c_str());
    };
    stat_row("min",    static_cast<double>(min));
    stat_row("max",    static_cast<double>(max));
    stat_row("mean",   mean);
    stat_row("stddev", stddev);
    ImGui::Text("cells:  %zu", cells.size());
    ImGui::Text("edited: %zu", edited);

    ImGui::Separator();
    ImGui::TextDisabled("histogram");
    if (ImPlot::BeginPlot("##stats_hist", ImVec2(-FLT_MIN, 160.0f),
                          ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend
                          | ImPlotFlags_NoTitle | ImPlotFlags_NoMenus
                          | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
        // Pick a reasonable bin count; ImPlot::PlotHistogram chooses
        // smart defaults when bins=ImPlotBin_Sturges.
        ImPlot::PlotHistogram("##bins", cells.data(),
                              static_cast<int>(cells.size()),
                              ImPlotBin_Sturges);
        ImPlot::EndPlot();
    }
    ImGui::End();
}

// "DTCs" panel — surface the diagnostic trouble codes declared by the pack
// with their current enable/disable state, and let the user toggle them.
// Mirrors the CLI's `project-{enable,disable}-dtc`: each toggle directly
// writes the bit through `st::set_dtc_enabled` and marks the project
// dirty. DTC edits bypass `edit::History` (the Edit struct is rect-based,
// not a byte-level operation), so Ctrl+Z won't roll them back. Emissions-
// flagged codes get a yellow "E" chip; the flash-time policy gate still
// enforces the jurisdiction profile.
void render_dtcs_panel(AppState &state) {
    ImGui::Begin("DTCs");
    if (!state.project.has_value()) {
        ImGui::TextDisabled("No project loaded.");
        ImGui::End();
        return;
    }
    auto const &def = state.project->definition();
    if (def.dtcs().empty()) {
        ImGui::TextDisabled("This pack declares no DTC bitmaps.");
        ImGui::End();
        return;
    }

    std::size_t emissions_total = 0;
    for (auto const &d : def.dtcs()) {
        if (d.emissions_relevant) ++emissions_total;
    }
    ImGui::TextDisabled("%zu DTC(s), %zu emissions-flagged",
                         def.dtcs().size(), emissions_total);
    ImGui::Separator();

    // Filter input — substring against the code or name. Same shape as the
    // sidebar's table filter but no global Ctrl+F focus binding (DTCs are
    // a secondary surface).
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##dtc_filter", "Filter DTCs…",
                              state.dtc_filter, sizeof state.dtc_filter,
                              ImGuiInputTextFlags_EscapeClearsAll);
    std::string_view const filter{state.dtc_filter};

    auto const &rom = state.project->working_rom();
    if (ImGui::BeginTable("dtc_table", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                          | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::size_t shown = 0;
        for (auto const &d : def.dtcs()) {
            if (!filter.empty() && !icontains(d.code, filter)
                && !icontains(d.name, filter)) {
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
                ImGui::TextDisabled("(broken bitmap reference '%s')",
                                     d.bitmap_id.c_str());
                continue;
            }
            auto const enabled_r = st::is_dtc_enabled(rom, *bm, d);
            bool enabled = enabled_r.has_value() ? *enabled_r : true;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(d.code.c_str());
            bool toggled = enabled;
            if (ImGui::Checkbox("##en", &toggled) && toggled != enabled) {
                auto change = st::set_dtc_enabled(state.project->working_rom(),
                                                   *bm, d, toggled);
                if (!change.has_value()) {
                    state.status_msg = "DTC toggle failed: "
                                       + change.error().to_string();
                } else {
                    state.dirty = true;
                    state.status_msg = (toggled ? "Enabled " : "Disabled ")
                                       + d.code;
                }
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(d.code.c_str());
            if (d.emissions_relevant) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.96f, 0.94f, 0.65f, 1.0f), "E");
            }

            ImGui::TableSetColumnIndex(2);
            if (d.name.empty()) {
                ImGui::TextDisabled("(no name)");
            } else {
                ImGui::TextUnformatted(d.name.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(d.code.c_str());
                if (!d.name.empty()) {
                    ImGui::TextDisabled("%s", d.name.c_str());
                }
                ImGui::Separator();
                ImGui::Text("Bitmap:    %s", d.bitmap_id.c_str());
                ImGui::Text("Address:   0x%08zX + %zu",
                             bm->address, d.byte_offset);
                ImGui::Text("Bit:       %d", d.bit);
                if (d.emissions_relevant) {
                    ImGui::TextColored(ImVec4(0.96f, 0.94f, 0.65f, 1.0f),
                                       "emissions-relevant");
                }
                ImGui::EndTooltip();
            }
        }
        ImGui::EndTable();

        if (!filter.empty()) {
            ImGui::TextDisabled("Showing %zu of %zu.", shown, def.dtcs().size());
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("DTC edits bypass undo (Ctrl+Z); save to persist.");
    ImGui::End();
}

void render_table_view(AppState &state, Fonts const &fonts) {
    ImGui::Begin("Table");

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
        ImGui::TextWrapped("Could not read table '%s' from the working ROM.",
                           state.selected_table_id.c_str());
        ImGui::End();
        return;
    }

    auto const *tbl  = state.project->definition().find_table(state.selected_table_id);
    auto const *scal = tbl != nullptr
                           ? state.project->definition().find_scaling(tbl->scaling)
                           : nullptr;
    int const   precision = scal != nullptr ? scal->precision : 0;

    // Header: human-readable name as the title; id/dim/address/category
    // tucked into a subtitle. Chips on the right of the title carry the
    // unit and the safety/emissions flags so they catch the eye without
    // shouting.
    bool const have_name = (tbl != nullptr && !tbl->name.empty());
    char const *title    = have_name ? tbl->name.c_str()
                                     : state.selected_table_id.c_str();

    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.0f);

    // Place a chip on the heading row, wrapping to a new line when it
    // would overflow the panel's right edge. Without this, narrow
    // window widths push chips off-screen instead of stacking them.
    auto place_chip = [](char const *text, ImVec4 fg, ImVec4 bg,
                         char const *tooltip = nullptr) {
        constexpr float kChipPad = 16.0f;  // FramePadding.x * 2 from chip()
        float const     w     = ImGui::CalcTextSize(text).x + kChipPad;
        ImGui::SameLine();
        float const cx    = ImGui::GetCursorPosX();
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
    if (tbl != nullptr && tbl->engine_safety_critical) {
        place_chip("Engine safety critical", chip_fg_warn(), chip_bg_warn(),
                   "Cells in this table affect engine safety — wrong values can\n"
                   "damage the engine. Make small changes, verify, and keep a\n"
                   "stock backup of the working ROM before flashing.");
    }
    if (tbl != nullptr && tbl->emissions_relevant) {
        place_chip("Emissions-relevant", chip_fg_caution(), chip_bg_caution(),
                   "Cells in this table influence the vehicle's emissions\n"
                   "behavior. Jurisdiction profile (see docs/06-legal-ethics)\n"
                   "governs warnings; engine-safety refusals still apply.");
    }

    if (tbl != nullptr) {
        // Subtitle: dev metadata (id, dim, address, category). Single line,
        // disabled gray, separators chosen to read as a path.
        if (have_name && !tbl->category.empty()) {
            ImGui::TextDisabled("%s  \xC2\xB7  %dD  \xC2\xB7  0x%08zX  \xC2\xB7  %s",
                                state.selected_table_id.c_str(),
                                tbl->dimensions, tbl->address,
                                tbl->category.c_str());
        } else if (have_name) {
            ImGui::TextDisabled("%s  \xC2\xB7  %dD  \xC2\xB7  0x%08zX",
                                state.selected_table_id.c_str(),
                                tbl->dimensions, tbl->address);
        } else if (!tbl->category.empty()) {
            ImGui::TextDisabled("%dD  \xC2\xB7  0x%08zX  \xC2\xB7  %s",
                                tbl->dimensions, tbl->address,
                                tbl->category.c_str());
        } else {
            ImGui::TextDisabled("%dD  \xC2\xB7  0x%08zX",
                                tbl->dimensions, tbl->address);
        }
    }

    // For 3D tables, project the chosen Z slice into a 2D view that the
    // renderers and stats can consume uniformly. The edit infrastructure
    // (Rect / Snapshot / History) is 2D-only, so editing is gated off for
    // 3D below — slice-aware edits are a follow-up.
    auto const &td_orig = *state.current_table_data;
    bool const  is_3d   = (tbl != nullptr && tbl->dimensions == 3
                          && !td_orig.slices.empty());
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
                state.project->definition().read_table_values(
                    state.project->source_rom(), *tbl);
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
                        edited_mask[r][c] =
                            (td_view.values[r][c] != (*src_2d)[r][c]);
                    }
                }
            }
        }
    }

    if (is_3d) {
        char preview[64];
        if (state.selected_z < td_orig.axis_z.size()) {
            std::snprintf(preview, sizeof(preview), "z = %g",
                          td_orig.axis_z[state.selected_z]);
        } else {
            std::snprintf(preview, sizeof(preview), "slice %zu",
                          state.selected_z);
        }
        ImGui::TextUnformatted("Z slice:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::BeginCombo("##z_slice", preview)) {
            for (std::size_t i = 0; i < td_orig.slices.size(); ++i) {
                char label[64];
                if (i < td_orig.axis_z.size()) {
                    std::snprintf(label, sizeof(label), "z = %g",
                                  td_orig.axis_z[i]);
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
        ImGui::TextDisabled("(%zu slices · 3D editing TBD)",
                            td_orig.slices.size());
    }

    GridStats const stats = compute_stats(td_view);
    if (stats.count > 0) {
        // Table-wide stats are noise when the user has a selection — the
        // selection-summary line below covers the same shape for the
        // chosen scope. Hide the table-wide text in that case; keep the
        // scale legend, which describes the whole-table heatmap mapping
        // and is useful regardless of selection.
        bool const show_table_stats = !state.selection.enabled;
        if (show_table_stats) {
            ImGui::TextDisabled("min %.*f  ·  max %.*f  ·  mean %.*f  ·  %zu cells",
                                precision, stats.min,
                                precision, stats.max,
                                precision, stats.mean,
                                stats.count);
        }
        // Heatmap legend — only meaningful in Grid view (Heatmap view
        // already has a vertical ColormapScale on the right via ImPlot).
        // Sampled from heatmap_color so the strip's gradient matches
        // what cells actually paint. A solid backing rect underneath
        // gives the mid-range alpha-zero region somewhere to sit —
        // otherwise the legend's middle is invisible against the
        // panel background, defeating the purpose.
        if (state.view_mode == TableViewMode::Grid
            && stats.max > stats.min) {
            constexpr float kBarW = 220.0f;
            constexpr float kBarH = 12.0f;
            constexpr int   kSegs = 64;
            if (show_table_stats) {
                ImGui::SameLine(0.0f, 24.0f);
            }
            ImGui::TextDisabled("scale:");
            ImGui::SameLine();
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.*f", precision, stats.min);
            ImGui::TextDisabled("%s", buf);
            ImGui::SameLine();
            ImVec2 const p0 = ImGui::GetCursorScreenPos();
            ImVec2 const p1 = ImVec2(p0.x + kBarW, p0.y + kBarH);
            auto * const dl = ImGui::GetWindowDrawList();
            // Backing rect: subtle dark base so the transparent middle
            // of the heatmap ramp has something to be transparent
            // against. ~10% white over the table row bg matches the
            // visual weight of a cell with no heatmap shading.
            dl->AddRectFilled(p0, p1, IM_COL32(40, 42, 48, 255));
            // N small filled rects sample the ramp evenly. 64 segments
            // is more than enough to look smooth at 220 px wide.
            for (int i = 0; i < kSegs; ++i) {
                double const t0 = static_cast<double>(i)
                                  / static_cast<double>(kSegs);
                double const t1 = static_cast<double>(i + 1)
                                  / static_cast<double>(kSegs);
                double const v_mid = stats.min
                                     + 0.5 * (t0 + t1)
                                       * (stats.max - stats.min);
                ImU32 const  col   =
                    heatmap_color(v_mid, stats.min, stats.max);
                if (col != 0u) {
                    float const x0 = p0.x + kBarW * static_cast<float>(t0);
                    float const x1 = p0.x + kBarW * static_cast<float>(t1);
                    dl->AddRectFilled(ImVec2(x0, p0.y),
                                       ImVec2(x1, p1.y), col);
                }
            }
            // Reserve the layout space the draw-list calls consumed so
            // the next widget after this lays out below correctly.
            ImGui::Dummy(ImVec2(kBarW, kBarH));
            ImGui::SameLine();
            std::snprintf(buf, sizeof buf, "%.*f", precision, stats.max);
            ImGui::TextDisabled("%s", buf);
        }
    }
    if (state.selection.enabled) {
        auto const rect = state.selection.as_rect();
        // Selection-scoped stats. Lets the user see what they're about
        // to apply +5% / Smooth / Interpolate to before they click.
        // Parallels the table-wide stats line above so the eye can
        // compare scope-to-scope at a glance.
        double      smin = std::numeric_limits<double>::infinity();
        double      smax = -std::numeric_limits<double>::infinity();
        double      ssum = 0.0;
        std::size_t scount = 0;
        for (std::size_t r = rect.r_start;
             r <= rect.r_end && r < td_view.values.size(); ++r) {
            auto const &row = td_view.values[r];
            for (std::size_t c = rect.c_start;
                 c <= rect.c_end && c < row.size(); ++c) {
                double const v = row[c];
                if (v < smin) smin = v;
                if (v > smax) smax = v;
                ssum += v;
                ++scount;
            }
        }
        if (scount > 0) {
            double const smean = ssum / static_cast<double>(scount);
            ImGui::TextDisabled(
                "selection: rows %zu:%zu × cols %zu:%zu  ·  "
                "min %.*f  ·  max %.*f  ·  mean %.*f  ·  %zu cells",
                rect.r_start, rect.r_end,
                rect.c_start, rect.c_end,
                precision, smin,
                precision, smax,
                precision, smean,
                scount);
        } else {
            // Selection rect lies outside the visible data (e.g.,
            // mid-3D-slice change with a stale selection). Fall back
            // to the previous shape so the user still sees the rect.
            ImGui::TextDisabled(
                "selection: rows %zu:%zu × cols %zu:%zu  (%zu cells)",
                rect.r_start, rect.r_end,
                rect.c_start, rect.c_end,
                state.selection.rows() * state.selection.cols());
        }
    }

    // Edit toolbar — ops act on the current selection, undo/redo on the
    // project's history. Buttons are disabled when there's no selection /
    // nothing to undo, rather than hidden, so the affordances stay visible.
    // 3D editing is gated off — the edit pipeline assumes a single 2D grid.
    bool const can_edit = state.selection.enabled && !is_3d;
    bool const can_undo = state.project->history().can_undo();
    bool const can_redo = state.project->history().can_redo();

    // Hover tooltips need to render even when the button is disabled — wrap
    // BeginDisabled with ImGuiItemFlags_AllowWhenDisabled on hover.
    auto const tip = [&](char const *body, char const *when_disabled = nullptr) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!can_edit && when_disabled != nullptr) {
                ImGui::SetTooltip("%s\n\n%s", body, when_disabled);
            } else {
                ImGui::SetTooltip("%s", body);
            }
        }
    };
    constexpr char const *kNoSelMsg = "Select cells in the grid to enable.";

    ImGui::BeginDisabled(!can_edit);
    if (ImGui::Button("+5%")) {
        apply_op(state, "+5%", [](auto &t, auto r) {
            return st::edit::percent_scale_cells(t, r, 5.0);
        });
    }
    tip("Increase each selected cell by 5% of its current value.", kNoSelMsg);
    ImGui::SameLine();
    if (ImGui::Button("-5%")) {
        apply_op(state, "-5%", [](auto &t, auto r) {
            return st::edit::percent_scale_cells(t, r, -5.0);
        });
    }
    tip("Decrease each selected cell by 5% of its current value.", kNoSelMsg);
    ImGui::SameLine();
    if (ImGui::Button("Smooth")) {
        apply_op(state, "smooth", [](auto &t, auto r) {
            return st::edit::smooth_cells(t, r, 1);
        });
    }
    tip("Replace each selected cell with the average of its neighbors.\n"
        "Stays inside the selection; useful for evening out spikes.",
        kNoSelMsg);
    ImGui::SameLine();
    if (ImGui::Button("Interpolate")) {
        apply_op(state, "interpolate", [](auto &t, auto r) {
            return st::edit::interpolate_cells(t, r);
        });
    }
    tip("Bilinear interpolation across the selection from its four corners.\n"
        "Fades edits smoothly between known anchor cells.",
        kNoSelMsg);
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 24.0f);

    ImGui::BeginDisabled(!can_undo);
    if (ImGui::Button("Undo")) {
        do_undo(state);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Undo the last edit.  (Ctrl+Z)");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo);
    if (ImGui::Button("Redo")) {
        do_redo(state);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Redo the next edit.  (Ctrl+Shift+Z or Ctrl+Y)");
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::BeginDisabled(!state.dirty);
    if (ImGui::Button("Save")) {
        save_project(state);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (state.dirty) {
            ImGui::SetTooltip(
                "Write the working ROM + edits to disk.  (Ctrl+S)");
        } else {
            ImGui::SetTooltip(
                "No unsaved edits.  (Ctrl+S)");
        }
    }
    ImGui::EndDisabled();

    // Flash button — opens the policy-gate modal. Enabled whenever there's
    // a project loaded; the modal itself handles the "nothing to flash"
    // and "blocked by policy" branches.
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.project.has_value());
    if (ImGui::Button("Flash...")) {
        state.show_flash_modal       = true;
        state.flash_confirm_checked  = false;
        state.flash_reason[0]        = '\0';
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Preview the flash plan (source -> working) and run the\n"
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

    ImGui::Separator();

    if (state.view_mode == TableViewMode::Heatmap) {
        render_table_heatmap(td_view, tbl, scal, stats);
    } else {
        render_table_grid(td_view, scal, stats, state.selection, fonts, state,
                          edited_mask);
    }

    // Escape clears the current selection when the Table panel has focus.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.selection.reset();
    }
    ImGui::End();
}

void render_status_bar(AppState &state) {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                     | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking
                     | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    if (state.project.has_value()) {
        bool const dirty = state.dirty;

        // Left cluster: project name → status chip → history position.
        ImGui::TextUnformatted(state.project->display_name().c_str());
        // Hover the name to see the on-disk path and which definition
        // pack the project is bound to. Two projects with the same id
        // (different copies, different forks) read the same in the
        // name line; the path is the unambiguous handle, and the pack
        // disambiguates ECU variant.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s\nPack: %s",
                state.project->dir().string().c_str(),
                state.project->definition().pack().id.c_str());
        }

        ImGui::SameLine();
        if (dirty) {
            chip("Unsaved edits", chip_fg_warn(), chip_bg_warn());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("In-memory edits have not been written to disk.\n"
                                  "Ctrl+S to save the .stune project.");
            }
        } else {
            chip("Clean", chip_fg_muted(), chip_bg_muted());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("All edits are saved to disk.");
            }
        }

        // Jurisdiction profile chip — disabled-muted in motorsport-only
        // (the default, silent gate), accent for any other profile so the
        // user notices when they're under a real regulatory posture. Click
        // to open a chooser. See docs/06-legal-ethics.md.
        ImGui::SameLine();
        {
            auto const  profile     = state.project->policy_profile();
            auto const  profile_str = std::string{
                st::policy::profile_name(profile)};
            bool const  is_default  =
                profile == st::policy::Profile::MotorsportOnly;
            if (is_default) {
                chip(profile_str.c_str(), chip_fg_muted(), chip_bg_muted());
            } else {
                chip(profile_str.c_str(), chip_fg_accent(), chip_bg_accent());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Active jurisdiction profile (project.toml).\n"
                    "Drives the EmissionsLinter at flash time — emissions-\n"
                    "flagged edits may require Confirm / Confirm+Reason\n"
                    "under stricter profiles. Engine-safety violations\n"
                    "always block, every profile.\n"
                    "Click to change.");
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
                ImGui::OpenPopup("##profile_chooser");
            }
            if (ImGui::BeginPopup("##profile_chooser")) {
                ImGui::TextDisabled("Jurisdiction profile (this project)");
                ImGui::Separator();
                auto pick = [&](st::policy::Profile p, char const *label,
                                char const *desc) {
                    bool const is_current        = (profile == p);
                    bool const is_saved_default =
                        (state.settings.default_policy_profile == p);
                    char row[64];
                    if (is_saved_default) {
                        std::snprintf(row, sizeof row, "%s  (default)", label);
                    } else {
                        std::snprintf(row, sizeof row, "%s", label);
                    }
                    if (ImGui::Selectable(row, is_current)) {
                        state.project->set_policy_profile(p);
                        if (auto s = state.project->save_metadata();
                            !s.has_value()) {
                            state.status_msg = "Profile save failed: "
                                + s.error().to_string();
                        } else {
                            state.status_msg = std::string{"Profile: "} + label;
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", desc);
                    }
                };
                pick(st::policy::Profile::MotorsportOnly, "motorsport-only",
                     "Silent on save and on flash. No emissions warnings.\n"
                     "Engine-safety violations still block.");
                pick(st::policy::Profile::AlbertaCa, "alberta-ca",
                     "Yellow badge on emissions-flagged edits.\n"
                     "No flash-time prompt. Engine-safety still blocks.");
                pick(st::policy::Profile::EuRoadworthy, "eu-roadworthy",
                     "Warning on save, confirmation on flash for emissions\n"
                     "edits. Engine-safety still blocks.");
                pick(st::policy::Profile::CaliforniaUs, "california-us",
                     "Confirm + free-text reason on save AND on flash for\n"
                     "emissions edits. Engine-safety still blocks.");
                ImGui::Separator();
                if (ImGui::MenuItem(
                        "Save current as default for new projects")) {
                    state.settings.default_policy_profile = profile;
                    save_settings(state.settings);
                    state.status_msg = std::string{"Default profile: "}
                        + std::string{st::policy::profile_name(profile)};
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Persist the project's current profile in\n"
                        "settings.txt as the default for future projects.\n"
                        "The CLI's `project-new` still defaults to\n"
                        "motorsport-only — this setting is GUI-only for now.");
                }
                ImGui::EndPopup();
            }
        }

        // DTCs-disabled chip — only present when the pack declares DTCs
        // AND at least one is currently disabled. Walks the pack each
        // frame (a few hundred byte reads at worst, dwarfed by the data
        // grid's per-cell scaling work). Click to focus the DTCs panel.
        {
            auto const &def = state.project->definition();
            if (!def.dtcs().empty()) {
                auto const &rom = state.project->working_rom();
                std::size_t disabled         = 0;
                std::size_t emissions_off    = 0;
                for (auto const &d : def.dtcs()) {
                    auto const *bm = def.find_dtc_bitmap(d.bitmap_id);
                    if (bm == nullptr) continue;
                    auto const en = st::is_dtc_enabled(rom, *bm, d);
                    if (en.has_value() && !*en) {
                        ++disabled;
                        if (d.emissions_relevant) ++emissions_off;
                    }
                }
                if (disabled > 0) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%zu DTC off", disabled);
                    ImGui::SameLine();
                    chip(buf, chip_fg_muted(), chip_bg_muted());
                    if (ImGui::IsItemHovered()) {
                        if (emissions_off > 0) {
                            ImGui::SetTooltip(
                                "%zu DTC(s) disabled in this working ROM\n"
                                "(%zu emissions-flagged).\n"
                                "See the DTCs panel to toggle.",
                                disabled, emissions_off);
                        } else {
                            ImGui::SetTooltip(
                                "%zu DTC(s) disabled in this working ROM.\n"
                                "See the DTCs panel to toggle.",
                                disabled);
                        }
                    }
                }
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("edits %zu / %zu",
                            state.project->history().cursor(),
                            state.project->history().size());

        // Middle cluster: transient status message. save_project sets
        // this to "Saved."; edit-op errors land here too. Previously
        // the with-project branch never showed it, so Ctrl+S
        // succeeded silently — now it gives the user feedback.
        if (!state.status_msg.empty()) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextDisabled("\xE2\x80\x94 %s", state.status_msg.c_str());
        }

        // Right cluster: source / working CRCs, right-aligned. Compute the
        // text width up front so we can place the cursor cleanly.
        char crc_buf[80];
        std::snprintf(crc_buf, sizeof(crc_buf),
                      "source 0x%08X  \xC2\xB7  working 0x%08X",
                      state.project->source_crc32_at_create(),
                      state.project->working_rom().crc32());
        float const crc_w = ImGui::CalcTextSize(crc_buf).x;
        float const right_x =
            ImGui::GetWindowContentRegionMax().x - crc_w
            - ImGui::GetStyle().FramePadding.x;
        if (right_x > ImGui::GetCursorPosX()) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_x);
            ImGui::TextDisabled("%s", crc_buf);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "CRC32 of the source ROM (immutable) and the current\n"
                    "working ROM. Any change in working bytes shifts the CRC.");
            }
        }
    } else {
        ImGui::TextDisabled("No project loaded. %s",
                            state.status_msg.empty() ? "" : state.status_msg.c_str());
    }
    ImGui::End();
}

// Render-frame callable shared between the main loop and GLFW's window-refresh
// callback. Windows enters a modal resize loop while the user drags a border;
// the main thread is blocked inside glfwPollEvents for the entire drag, so the
// only way to keep painting is to render from inside the WM_PAINT-driven
// refresh callback that GLFW dispatches during the modal loop.
std::function<void()> g_render_frame;

} // namespace

int main(int argc, char *argv[]) {
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == 0) {
        std::fprintf(stderr, "subuwutuner-gui: glfwInit failed\n");
        return 1;
    }

    // Stick to OpenGL 3.0 core-ish — the lowest target ImGui supports cleanly
    // across Win/Mac/Linux without needing extension-loader gymnastics.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow *window =
        glfwCreateWindow(1400, 880, "SubuwuTuner", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "subuwutuner-gui: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto &io = ImGui::GetIO();
    // Deliberately NOT setting ImGuiConfigFlags_NavEnableKeyboard. With
    // it on, ImGui auto-focuses the first focusable widget in each
    // window and draws a permanent nav-focus rectangle around it —
    // which reads as "one cell is highlighted and never unhighlights"
    // in the data grid. Tab-cycling through hundreds of cells is not
    // a workflow this app targets, and every actual keyboard nav we
    // care about (arrow keys in the grid, Ctrl+F focus on the filter,
    // Enter/Esc in modals, Ctrl+{O,S,Z,Y,Q}) is handled explicitly via
    // IsKeyPressed and works without the nav system being on.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    Fonts const fonts = load_fonts();
    apply_theme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    state.recents  = load_recents();
    state.settings = load_settings();
    std::string_view const arg1 = (argc >= 2) ? argv[1] : "";
    if (arg1 == "-h" || arg1 == "--help" || arg1 == "/?") {
        std::fputs("Usage: subuwutuner-gui [PROJECT.stune]\n"
                   "  Open the optional .stune project on launch. Without an "
                   "argument, the GUI starts on the welcome panel.\n",
                   stderr);
        return 0;
    }
    if (!arg1.empty()) {
        state.try_open_project(argv[1]);
    } else {
        state.status_msg = "Open a .stune project: File → Open (Ctrl+O).";
    }

    auto render_frame = [&]() {
        // The user clicked the window's close button (or any other
        // path that toggled GLFW's close flag). Route it through the
        // same confirm-action machinery as Ctrl+Q / File → Quit so an
        // accidental click on the X doesn't lose unsaved edits. The
        // GLFW flag is reset so request_action's confirm path can
        // decide whether to actually quit.
        if (glfwWindowShouldClose(window) != 0) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            request_action(state, ConfirmAction::Quit);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Global keyboard shortcuts. IsKeyChordPressed is mod-aware, so
        // Ctrl+Shift+Z and Ctrl+Z don't collide.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) {
            request_action(state, ConfirmAction::Quit);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
            request_action(state, ConfirmAction::OpenDialog);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            save_project(state);
        }
        // Ctrl+F focuses the sidebar's table filter. Only meaningful
        // when a project is open; harmless otherwise (the next
        // render_sidebar call will reset the flag without acting on
        // it).
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
            state.focus_table_filter = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
            do_redo(state);
        } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
            do_undo(state);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
            do_redo(state);
        }

        render_menubar(state);
        render_dockspace_host();
        render_sidebar(state);
        render_table_view(state, fonts);
        render_stats_panel(state);
        render_dtcs_panel(state);
        render_status_bar(state);
        render_unsaved_modal(state);
        render_flash_modal(state);

        if (state.show_imgui_demo) {
            ImGui::ShowDemoWindow(&state.show_imgui_demo);
        }

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport: render OS-level windows that ImGui has spawned for
        // panels the user tore off. The GL context juggling is the standard
        // pattern from the ImGui examples.
        if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            GLFWwindow *prev_ctx = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(prev_ctx);
        }

        // OS window title reflects current state: project name + dirty
        // marker so taskbar / alt-tab show what's open and whether
        // there are unsaved changes. Static cache avoids the glfw call
        // every frame.
        {
            static std::string last_title;
            std::string        desired;
            if (state.project.has_value()) {
                if (state.dirty) {
                    desired = "\xE2\x97\x8F ";
                }
                desired += state.project->display_name();
                desired += " \xE2\x80\x94 SubuwuTuner";
            } else {
                desired = "SubuwuTuner";
            }
            if (desired != last_title) {
                glfwSetWindowTitle(window, desired.c_str());
                last_title = std::move(desired);
            }
        }

        glfwSwapBuffers(window);
    };

    g_render_frame = render_frame;
    glfwSetWindowRefreshCallback(window, [](GLFWwindow * /*w*/) {
        if (g_render_frame) {
            g_render_frame();
        }
    });

    while (!state.user_confirmed_quit) {
        glfwPollEvents();
        render_frame();
    }
    g_render_frame = nullptr;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
