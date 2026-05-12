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

std::filesystem::path recents_config_path() {
    auto const env = [](char const *name) -> std::filesystem::path {
        auto const *v = std::getenv(name);
        return v != nullptr ? std::filesystem::path{v}
                            : std::filesystem::path{};
    };
#if defined(_WIN32)
    auto base = env("LOCALAPPDATA");
    if (base.empty()) base = env("USERPROFILE");
    if (base.empty()) base = std::filesystem::current_path();
    return base / "SubuwuTuner" / "recents.txt";
#elif defined(__APPLE__)
    auto base = env("HOME");
    if (base.empty()) base = std::filesystem::current_path();
    return base / "Library" / "Application Support" / "SubuwuTuner"
           / "recents.txt";
#else
    auto base = env("XDG_CONFIG_HOME");
    if (base.empty()) {
        auto home = env("HOME");
        if (home.empty()) home = std::filesystem::current_path();
        base = home / ".config";
    }
    return base / "subuwutuner" / "recents.txt";
#endif
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

    // Sidebar filter. Substring-matched (case-insensitive) against table
    // name + id. 128 chars is generous — table identifiers in real packs
    // top out around 40. `focus_table_filter` is the Ctrl+F handoff: set
    // by the main-loop shortcut, consumed by the sidebar's next render.
    char                                     table_filter[128]{};
    bool                                     focus_table_filter{false};

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
        ImGui::TextUnformatted("You have unsaved edits in this project.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextDisabled("Continuing without saving will discard them.");
        ImGui::Dummy(ImVec2(0.0f, 16.0f));

        constexpr float kBtnW = 160.0f;
        if (ImGui::Button("Save and continue", ImVec2(kBtnW, 0.0f))) {
            save_project(state);
            execute_action(state, state.next_action, state.next_recent);
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write the working ROM + edits to disk, "
                              "then proceed.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard changes", ImVec2(kBtnW, 0.0f))) {
            state.dirty       = false;
            execute_action(state, state.next_action, state.next_recent);
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Throw away every edit since the last save "
                              "and proceed.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(kBtnW * 0.7f, 0.0f))) {
            state.next_action = ConfirmAction::None;
            state.next_recent.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stay here. Don't open, close, or quit.");
        }
        ImGui::EndPopup();
    }
}

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
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

    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    c[ImGuiCol_NavCursor]             = accent;
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
        ImGuiID center = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.22f, &left, &center);

        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Table", center);

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

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
                request_action(state, ConfirmAction::OpenDialog);
            }
            if (ImGui::MenuItem("Save Project", "Ctrl+S", false, has_project)) {
                save_project(state);
            }
            if (ImGui::MenuItem("Close Project", nullptr, false, has_project)) {
                request_action(state, ConfirmAction::Close);
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
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, can_redo)) {
                do_redo(state);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("ImGui demo window", nullptr, &state.show_imgui_demo);
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
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void render_sidebar(AppState &state) {
    ImGui::Begin("Tables");

    if (!state.project.has_value()) {
        // Quiet, welcoming empty state — the user shouldn't have to read the
        // menu to understand how to start. Inline CTA mirrors the menu's
        // File → Open Project so the obvious affordance is right here.
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::TextWrapped("No project open yet.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (ImGui::Button("Open Project…", ImVec2(-1.0f, 0.0f))) {
            request_action(state, ConfirmAction::OpenDialog);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pick a .stune project directory.  (Ctrl+O)");
        }
        if (!state.status_msg.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::TextWrapped("%s", state.status_msg.c_str());
        }
        ImGui::End();
        return;
    }

    auto const &def = state.project->definition();
    ImGui::TextDisabled("Pack: %s", def.pack().id.c_str());

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
    // Count matches once so the header line can report "N of M".
    std::size_t matched = 0;
    for (auto const &t : def.tables()) {
        if (filter.empty()
            || icontains(t.name, filter) || icontains(t.id, filter)) {
            ++matched;
        }
    }
    if (filter.empty()) {
        ImGui::TextDisabled("%zu tables", def.tables().size());
    } else {
        ImGui::TextDisabled("%zu of %zu tables", matched,
                             def.tables().size());
    }
    ImGui::Separator();

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

    for (auto const &t : def.tables()) {
        if (!filter.empty()
            && !icontains(t.name, filter) && !icontains(t.id, filter)) {
            continue;
        }
        bool const selected = state.selected_table_id == t.id;
        // Prefer the human-readable name as the primary label. Snake-case
        // IDs are developer-facing — surface them in the tooltip instead.
        char const *label = t.name.empty() ? t.id.c_str() : t.name.c_str();
        ImGui::PushID(t.id.c_str());
        if (ImGui::Selectable(label, selected)) {
            state.select_table(t.id);
        }
        if (ImGui::IsItemHovered()) {
            // Compose the tooltip so hover gives the full identity card:
            // id, dimension, address, and any safety/emissions flags.
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
// Tuned to be readable with bright text on the dark row backgrounds — alpha
// caps at ~140/255 so cells tint rather than swamp the value underneath.
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
        a = lerp(140.0, 0.0, s);
    } else {
        double const s = (t - 0.5) * 2.0;
        r = lerp(20.0, 180.0, s);
        g = lerp(22.0, 90.0, s);
        b = lerp(26.0, 50.0, s);
        a = lerp(0.0, 140.0, s);
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
                       Fonts const &                   fonts) {
    int const  precision = scal != nullptr ? scal->precision : 0;
    auto const cols      = static_cast<int>(td.axis_x.size()) + 1;
    if (cols < 2) {
        ImGui::TextDisabled("(table has no X axis)");
        return;
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
    ImGui::TableHeadersRow();

    auto const grid_cols = td.values.empty() ? std::size_t{0} : td.values.front().size();
    char       buf[32];
    for (std::size_t r = 0; r < td.values.size(); ++r) {
        ImGui::TableNextRow();
        // Leftmost axis-Y label column: right-aligned plain text, no
        // heatmap, not clickable — it's metadata, not data.
        ImGui::TableNextColumn();
        if (!td.axis_y.empty() && r < td.axis_y.size()) {
            std::snprintf(buf, sizeof(buf), "%.*f", precision, td.axis_y[r]);
            text_right_aligned(buf);
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
            bool const is_sel = selection.contains(r, c);
            if (ImGui::Selectable(buf, is_sel,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selection.click(r, c, ImGui::GetIO().KeyShift);
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
    // Push content down to the upper third when there are no recents
    // (the original "first-run" feel). When recents exist, sit higher
    // so the list has room to breathe without the panel scrolling.
    bool const has_recents = !state.recents.empty();
    float const top_pad = has_recents ? (avail.y * 0.10f) : (avail.y * 0.22f);
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
        ImGui::SetNextItemWidth(kRowW);
        ImGui::TextDisabled("Recent projects");
        ImGui::Separator();
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
            // Subtitle: dimmed full path, smaller and aligned under the
            // row. The two-line shape comes from Button + this label
            // pair rather than a multi-line button (which ImGui doesn't
            // do well with text alignment).
            center_cursor_x(kRowW);
            if (exists) {
                ImGui::TextDisabled("%s", e.path.string().c_str());
            } else {
                ImGui::TextDisabled("%s  (missing)", e.path.string().c_str());
            }
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

    if (scal != nullptr && !scal->unit.empty()) {
        ImGui::SameLine();
        chip(scal->unit.c_str(), chip_fg_accent(), chip_bg_accent());
    }
    if (tbl != nullptr && tbl->engine_safety_critical) {
        ImGui::SameLine();
        chip("Engine safety critical", chip_fg_warn(), chip_bg_warn());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Cells in this table affect engine safety — wrong values can\n"
                "damage the engine. Make small changes, verify, and keep a\n"
                "stock backup of the working ROM before flashing.");
        }
    }
    if (tbl != nullptr && tbl->emissions_relevant) {
        ImGui::SameLine();
        chip("Emissions-relevant", chip_fg_caution(), chip_bg_caution());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Cells in this table influence the vehicle's emissions\n"
                "behavior. Jurisdiction profile (see docs/06-legal-ethics)\n"
                "governs warnings; engine-safety refusals still apply.");
        }
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
        ImGui::TextDisabled("min %.*f  ·  max %.*f  ·  mean %.*f  ·  %zu cells",
                            precision, stats.min,
                            precision, stats.max,
                            precision, stats.mean,
                            stats.count);
    }
    if (state.selection.enabled) {
        auto const rect = state.selection.as_rect();
        ImGui::TextDisabled("selection: rows %zu:%zu × cols %zu:%zu  (%zu cells)",
                            rect.r_start, rect.r_end,
                            rect.c_start, rect.c_end,
                            state.selection.rows() * state.selection.cols());
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
        render_table_grid(td_view, scal, stats, state.selection, fonts);
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
        bool const dirty =
            state.project->working_rom().crc32() != state.project->source_crc32_at_create();

        // Left cluster: project name → status chip → history position.
        ImGui::TextUnformatted(state.project->display_name().c_str());

        ImGui::SameLine();
        if (dirty) {
            chip("Unsaved edits", chip_fg_warn(), chip_bg_warn());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Working ROM differs from the source.\n"
                                  "Ctrl+S to save the .stune project.");
            }
        } else {
            chip("Clean", chip_fg_muted(), chip_bg_muted());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Working ROM matches the source — nothing to save.");
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("edits %zu / %zu",
                            state.project->history().cursor(),
                            state.project->history().size());

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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    Fonts const fonts = load_fonts();
    apply_theme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    state.recents = load_recents();
    if (argc >= 2) {
        state.try_open_project(argv[1]);
    } else {
        state.status_msg = "Open a .stune project: File → Open (Ctrl+O).";
    }

    while (!state.user_confirmed_quit) {
        glfwPollEvents();

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
        render_status_bar(state);
        render_unsaved_modal(state);

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

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
