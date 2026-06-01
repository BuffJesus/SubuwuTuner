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

#include "st/autotune.hpp"
#include "st/core/version.hpp"
#include "st/edit.hpp"
#include "st/feature.hpp"
#include "st/flash.hpp"
#include "st/log/adaptive_history.hpp"
#include "st/log/coldstart.hpp"
#include "st/log/ebcs.hpp"
#include "st/log/knock_dashboard.hpp"
#include "st/policy.hpp"
#include "st/config.hpp"
#include "st/defs/pack_registry.hpp"
#include "st/project.hpp"
#include "st/transport/factory.hpp"
#include "st/transport/mock.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/uds_trace.hpp"

#include "icon_data.hpp"

// Shared UI headers — checkpoint 1 of the main.cpp split. Struct/enum
// definitions live here; function implementations are still in this
// file until the per-modal / per-panel moves land.
#include "app_state.hpp"
#include "persistence.hpp"
#include "theme.hpp"
#include "actions.hpp"
#include "project_io.hpp"
#include "widgets/widgets.hpp"
#include "widgets/adapter_picker.hpp"
#include "modals/modals.hpp"
#include "panels/panels.hpp"

// ImGui + backends.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // DockBuilder*
#include <implot.h>
#include <ios>
#include <limits>
#include <memory>
#include <nfd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Everything below is wrapped in `namespace st::ui { ... }` for
// checkpoint 1 of the split. Header decls (in app_state.hpp etc.) are
// at st::ui scope; main.cpp's function bodies sit directly in st::ui
// so they satisfy those decls with external linkage. The only inner
// anonymous namespace kept is the one around g_current_theme (a true
// file-local cache variable). Per-file anon namespaces come back when
// each modal / panel moves to its own .cpp.

namespace st::ui {

// Forward-declared so render_* panels defined ahead of the helper
// body can use it. Definition lives near the other text helpers
// further down (text_centered_disabled, text_centered_subtle).
// Archetype is `gnu_printf` (not bare `printf`) so MinGW's MS-printf
// archetype isn't selected — see the definition's commentary block.
#if defined(__GNUC__)
[[gnu::format(gnu_printf, 1, 2)]]
#endif
void text_subtle(char const *fmt, ...);

// Forward decls for the theme-aware chip palette — defined alongside
// the chip() helper below. Forward-declared here so widget-render
// functions (which sit early in the file) can call them for inline
// status text colors. Modal/inline use is the larger consumer; the
// sidebar S/E badges were the original trigger.
ImVec4 chip_fg_warn();
ImVec4 chip_fg_caution();
ImVec4 chip_fg_ok();
ImVec4 chip_fg_danger();
ImVec4 chip_fg_info();
ImVec4 chip_fg_muted();
ImVec4 chip_bg_muted();

// Forward decl for the chip() helper itself. Defined alongside the
// chip_fg_/chip_bg_ palette below. The command palette (defined ahead
// of chip()'s definition) needs this to render the category badge on
// each result row.
void chip(char const *text, ImVec4 fg, ImVec4 bg);

// Forward decl for the centered empty-state helper — same forward-
// decl reason as the chip palette. Panels at lines ~5985+ call this.
void render_empty_state(char const *title, char const *hint);

// Centering / centered-text helpers — defined in the typography block
// near render_empty_state. Forward-declared here so modal renders
// (About modal at ~2841 calls these) can find them.
void center_cursor_x(float w);
void text_centered(char const *text, float scale);
void text_centered_subtle(char const *text);

// kSpaceXS..XL moved to widgets/widgets.hpp (inline constexpr).
// Fonts struct moved to theme.hpp.
// AppState / ToastKind forward decls dropped — full decls now in
// app_state.hpp via #include above. Forward-declaring them at this anon
// namespace scope would shadow the header types as incomplete.

// Forward decls for the primary-action accent button-color stack.
// Use in pairs around exactly one ImGui::Button() to render it as
// the modal's primary action (brand purple, matches the welcome-
// panel accent rule + status-bar profile chip + tab-selected
// overline). Modals at lines 1626+ call these.
void push_primary_button_colors();
void pop_primary_button_colors();

// Forward decl for the toast notification helper. Defined alongside
// render_toasts in the file-bottom render block. Sites that surface
// transient non-modal feedback (save success, project open OK, etc)
// call this; modal failures still go inline per
// feedback_modal_inline_errors.
void enqueue_toast(AppState &state, ToastKind kind, std::string text);

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

// RecentEntry + kRecentsCap moved to persistence.hpp.

std::filesystem::path config_dir_root() {
    auto const env = [](char const *name) -> std::filesystem::path {
        auto const *v = std::getenv(name);
        return v != nullptr ? std::filesystem::path{v} : std::filesystem::path{};
    };
#if defined(_WIN32)
    auto base = env("LOCALAPPDATA");
    if (base.empty())
        base = env("USERPROFILE");
    if (base.empty())
        base = std::filesystem::current_path();
    return base / "SubuwuTuner";
#elif defined(__APPLE__)
    auto base = env("HOME");
    if (base.empty())
        base = std::filesystem::current_path();
    return base / "Library" / "Application Support" / "SubuwuTuner";
#else
    auto base = env("XDG_CONFIG_HOME");
    if (base.empty()) {
        auto home = env("HOME");
        if (home.empty())
            home = std::filesystem::current_path();
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

// Locate the bundled fixtures/demo.stune project relative to argv[0].
// Returns nullopt if no candidate directory contains a project.toml.
// Candidate priority:
//   1. <exe-dir>/../../../fixtures/demo.stune  (CMake dev build tree:
//      build/<preset>/bin/exe → repo/fixtures)
//   2. <exe-dir>/../fixtures/demo.stune        (typical install
//      layout with bin/ and fixtures/ as siblings)
//   3. <exe-dir>/fixtures/demo.stune           (flat install layout)
//
// argv[0] on Unix may be a bare name resolved via PATH; weakly_canonical
// handles both that case and "./subuwutuner-gui" with the same call.
// On Windows it's typically a full backslash path. Either way, errors
// downgrade to nullopt — the demo button just doesn't render.
std::optional<std::filesystem::path>
resolve_demo_project_path(char const *argv0) {
    namespace fs = std::filesystem;
    if (argv0 == nullptr || argv0[0] == '\0') {
        return std::nullopt;
    }
    std::error_code ec;
    fs::path const exe = fs::weakly_canonical(fs::path{argv0}, ec);
    if (ec) {
        return std::nullopt;
    }
    fs::path const exe_dir = exe.parent_path();
    std::array<fs::path, 3> const candidates{
        exe_dir / ".." / ".." / ".." / "fixtures" / "demo.stune",
        exe_dir / ".." / "fixtures" / "demo.stune",
        exe_dir / "fixtures" / "demo.stune",
    };
    for (auto const &c : candidates) {
        std::error_code dir_ec;
        if (!fs::is_directory(c, dir_ec) || dir_ec) {
            continue;
        }
        std::error_code file_ec;
        if (!fs::exists(c / "project.toml", file_ec) || file_ec) {
            continue;
        }
        std::error_code canon_ec;
        auto canon = fs::weakly_canonical(c, canon_ec);
        return canon_ec ? c : canon;
    }
    return std::nullopt;
}

std::string iso8601_utc_now() {
    auto const now = std::chrono::system_clock::now();
    auto const t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
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
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y, &M, &D, &h, &m, &s) != 6) {
        return {};
    }
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
#if defined(_WIN32)
    std::time_t const then = ::_mkgmtime(&tm);
#else
    std::time_t const then = ::timegm(&tm);
#endif
    if (then == static_cast<std::time_t>(-1)) {
        return {};
    }
    auto const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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
    std::ifstream in{recents_config_path()};
    if (!in)
        return out;
    std::string line;
    while (std::getline(in, line) && out.size() < kRecentsCap) {
        auto const tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) {
            continue;
        }
        RecentEntry e;
        e.opened_at = line.substr(0, tab);
        e.path = std::filesystem::path{line.substr(tab + 1)};
        out.push_back(std::move(e));
    }
    return out;
}

void save_recents(std::vector<RecentEntry> const &recents) {
    auto const path = recents_config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Failure to create the directory is non-fatal — recents is best-
    // effort. The next save attempt will retry.
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    for (auto const &e : recents) {
        out << e.opened_at << '\t' << e.path.generic_string() << '\n';
    }
}

// User-preferences persistence. Stored next to recents.txt as a
// `key=value\n` plain-text file (one setting per line). New settings can
// land without breaking older builds — unknown keys are silently
// ignored on load. Currently:
//   default_policy_profile = motorsport-only|alberta-ca|eu-roadworthy|california-us
// Theme enum moved to persistence.hpp.

char const *theme_name(Theme t) noexcept {
    switch (t) {
    case Theme::Dark:
        return "dark";
    case Theme::Light:
        return "light";
    }
    return "dark";
}

std::optional<Theme> parse_theme(std::string_view s) noexcept {
    if (s == "dark")
        return Theme::Dark;
    if (s == "light")
        return Theme::Light;
    return std::nullopt;
}

// Map an autotune lint-kind enum to plain-language descriptive text
// for end-user display. The library-side `lint_kind_name` returns
// kebab-cased technical terms ("non-monotonic", "step discontinuity")
// useful for debug/CLI output but jargon-y for tuners. This wrapper
// produces a short fix-oriented phrase that explains the problem
// without requiring familiarity with the underlying enum.
[[nodiscard]] char const *pretty_lint_kind(st::autotune::LintViolationKind kind) noexcept {
    using K = st::autotune::LintViolationKind;
    switch (kind) {
    case K::NonMonotonic:
        return "neighbor cells out of expected order";
    case K::StepDiscontinuity:
        return "neighbor-cell jump exceeds smoothness threshold";
    }
    return "lint violation";
}

// AccentTriple struct moved to theme.hpp.

AccentTriple accent_for(Theme t) noexcept {
    constexpr ImVec4 accent_purple(0.55f, 0.35f, 0.85f, 1.00f);
    if (t == Theme::Light) {
        return {accent_purple, ImVec4(0.48f, 0.28f, 0.78f, 1.00f),
                ImVec4(0.40f, 0.20f, 0.70f, 1.00f)};
    }
    return {accent_purple, ImVec4(0.62f, 0.45f, 0.90f, 1.00f), ImVec4(0.70f, 0.55f, 0.95f, 1.00f)};
}

// File-local current-theme cache, updated by apply_theme() at the top
// of the function so any helper that needs theme-aware colors can read
// it without threading Theme through every call site. Chip palettes
// (chip_fg_warn etc) and badge tints use this; widget-internal ImGui
// style colors are still the source of truth for native widgets.
namespace {
Theme g_current_theme{Theme::Dark};
}

Theme current_theme() noexcept {
    return g_current_theme;
}

// True when the active theme renders against a light surface — useful
// for picking foreground colors that need to flip dark to stay legible.
bool theme_is_light() noexcept {
    return g_current_theme == Theme::Light;
}

// Settings struct moved to persistence.hpp.

Settings load_settings() {
    Settings s;
    std::ifstream in{settings_config_path()};
    if (!in)
        return s;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto const eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string_view const key{line.data(), eq};
        std::string_view const val{line.data() + eq + 1, line.size() - eq - 1};
        if (key == "default_policy_profile") {
            if (auto p = st::policy::parse_profile(val); p.has_value()) {
                s.default_policy_profile = *p;
            }
        } else if (key == "theme") {
            if (auto t = parse_theme(val); t.has_value()) {
                s.theme = *t;
            }
        }
    }
    return s;
}

void save_settings(Settings const &s) {
    auto const path = settings_config_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    out << "default_policy_profile=" << st::policy::profile_name(s.default_policy_profile) << '\n';
    out << "theme=" << theme_name(s.theme) << '\n';
}

// Move `path` to the front of `recents`, deduplicating by canonical
// path comparison and capping the list at `kRecentsCap`. Idempotent.
void push_recent(std::vector<RecentEntry> &recents, std::filesystem::path const &path) {
    std::error_code ec;
    auto const canon = std::filesystem::weakly_canonical(path, ec);
    auto const compare_to = canon.empty() ? path : canon;
    // Remove any existing entry pointing at the same canonical path.
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                 [&](RecentEntry const &e) {
                                     std::error_code ec2;
                                     auto const ec_path =
                                         std::filesystem::weakly_canonical(e.path, ec2);
                                     return (ec_path.empty() ? e.path : ec_path) == compare_to;
                                 }),
                  recents.end());
    // Insert at the front.
    RecentEntry e;
    e.opened_at = iso8601_utc_now();
    e.path = compare_to;
    recents.insert(recents.begin(), std::move(e));
    if (recents.size() > kRecentsCap)
        recents.resize(kRecentsCap);
}

// Selection, TableViewMode, WorkspaceMode, ConfirmAction, ToastKind,
// Toast, AdapterPickerState moved to app_state.hpp.

// Adapter picker helpers (render_adapter_picker / adapter_picker_to_spec /
// adapter_is_trace_mode) stay file-local for now — their bodies use ImGui +
// transport-factory details. Header is widgets/adapter_picker.hpp.
[[nodiscard]] bool adapter_is_trace_mode(AdapterPickerState const &s) noexcept {
    return s.kind_idx == 3;
}

// Render the adapter-picker sub-form into the current ImGui window/popup.
// Returns true iff the form is filled in enough to enable a "go" button
// (kind selected + the matching path field non-empty). Intended to be
// called inside a modal/window started by the caller.
[[nodiscard]] bool render_adapter_picker(AdapterPickerState &s) {
    char const *const labels[] = {"J2534", "OBDX", "Native", "Trace (test)"};
    ImGui::Combo("Adapter", &s.kind_idx, labels, IM_ARRAYSIZE(labels));
    if (s.kind_idx == 0) {
        ImGui::InputText("Vendor DLL path", s.dll_path, sizeof s.dll_path);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Path to your J2534 vendor DLL\n"
                              "(e.g. op20pt32.dll, MongoosePro_GM.dll).");
        }
        return s.dll_path[0] != '\0';
    }
    if (s.kind_idx == 3) {
        ImGui::InputText("Trace file (.uds)", s.trace_path, sizeof s.trace_path);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Path to a UDS trace file ('> req' / '< resp' pairs).\n"
                              "Lets you smoke-test the flow without an adapter — feeds\n"
                              "the canned exchanges into a MockTransport. Use this for\n"
                              "pre-OBDX testing or for replaying a recorded session.");
        }
        return s.trace_path[0] != '\0';
    }
    ImGui::InputText("Device path", s.device_path, sizeof s.device_path);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("USB CDC port for the adapter.\n"
                          "Windows: COM5, COM6 etc.   Linux: /dev/ttyACM0.");
    }
    return s.device_path[0] != '\0';
}

// Convert an AdapterPickerState to a TransportSpec ready for
// `st::transport::open_transport`. Pure function; no UI side effects.
// PRECONDITION: !adapter_is_trace_mode(s). Caller must branch on
// adapter_is_trace_mode first; trace mode does not go through the
// factory.
[[nodiscard]] st::transport::TransportSpec
adapter_picker_to_spec(AdapterPickerState const &s) {
    st::transport::TransportSpec spec;
    switch (s.kind_idx) {
    case 0:
        spec.kind = st::transport::Kind::J2534;
        break;
    case 2:
        spec.kind = st::transport::Kind::Native;
        break;
    default:
        spec.kind = st::transport::Kind::Obdx;
        break;
    }
    spec.dll_path = s.dll_path;
    spec.device_path = s.device_path;
    return spec;
}

// AppState moved to app_state.hpp.
//
// The three methods that have non-trivial bodies (try_open_project,
// select_table, close_project) are declared in the header and defined
// out-of-class here so the bodies can call file-local helpers like
// enqueue_toast / push_recent / save_recents that still live in main.cpp.

void AppState::try_open_project(std::filesystem::path const &path) {
    auto r = st::Project::open(path);
    if (!r.has_value()) {
        auto const err = "Failed to open " + path.string() + ": " + r.error().to_string();
        status_msg = err;
        enqueue_toast(*this, ToastKind::Danger, err);
        project.reset();
        selected_table_id.clear();
        current_table_data.reset();
        selection.reset();
        last_save_iso.reset();
        return;
    }
    project = std::move(*r);
    status_msg.clear();
    selected_table_id.clear();
    current_table_data.reset();
    selection.reset();
    dirty = false;
    last_save_iso.reset();
    enqueue_toast(*this, ToastKind::Success,
                  std::string{"Loaded "} + path.filename().string());
    push_recent(recents, path);
    save_recents(recents);
    auto const &tables = project->definition().tables();
    if (!tables.empty()) {
        select_table(tables.front().id);
    }
}

void AppState::select_table(std::string const &id) {
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

void AppState::close_project() {
    project.reset();
    selected_table_id.clear();
    current_table_data.reset();
    selection.reset();
    selected_z = 0;
    status_msg.clear();
    dirty = false;
    last_save_iso.reset();
}

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
        // Failure: keep status_msg for persistent visibility AND fire
        // a danger toast for the immediate "this just happened" beat.
        // The user might be staring at the save button when this fires;
        // the toast confirms they triggered something.
        auto const err = "Save failed: " + s.error().to_string();
        state.status_msg = err;
        enqueue_toast(state, ToastKind::Danger, err);
        return;
    }
    // Success: status bar already shows "Saved Nm ago" + last_save_iso
    // tooltip, so the persistent status_msg is redundant. Toast does the
    // transient confirmation and disappears on its own — no stale
    // "Saved." sticking around for minutes after.
    state.status_msg.clear();
    enqueue_toast(state, ToastKind::Success, "Saved.");
    state.dirty = false;
    state.last_save_iso = iso8601_utc_now();
}

// Writes the currently-selected table to `path` as a CSV in the same
// format `subuwutuner-cli project-export-csv` emits (identity headers
// + `row,col,value` rows). When `diff_only` is true, emits only cells
// whose working value differs from the source — the share-a-tune-diff
// shape. Returns nullopt on success or an error message for the
// status bar.
std::optional<std::string>
write_current_table_csv(AppState const &state, std::filesystem::path const &path, bool diff_only) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    if (state.selected_table_id.empty()) {
        return std::string{"No table selected."};
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        return "Table '" + state.selected_table_id + "' not found in pack.";
    }
    auto const working_td =
        state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!working_td.has_value()) {
        return "read working: " + working_td.error().to_string();
    }
    std::optional<st::Definition::TableData> source_td;
    if (diff_only) {
        auto s = state.project->definition().read_table_values(state.project->source_rom(), *table);
        if (!s.has_value()) {
            return "read source: " + s.error().to_string();
        }
        source_td = std::move(*s);
    }
    auto const *scaling = state.project->definition().find_scaling(table->scaling);
    int const prec = scaling != nullptr ? scaling->precision : 6;

    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        return "cannot open " + path.string();
    }
    out << "# pack_id = \"" << state.project->definition().pack().id << "\"\n";
    out << "# table   = \"" << table->id << "\"\n";
    out << "row,col,value\n";
    char buf[64];
    for (std::size_t r = 0; r < working_td->values.size(); ++r) {
        for (std::size_t c = 0; c < working_td->values[r].size(); ++c) {
            double const v = working_td->values[r][c];
            if (diff_only) {
                if (r >= source_td->values.size() || c >= source_td->values[r].size())
                    continue;
                if (v == source_td->values[r][c])
                    continue;
            }
            std::snprintf(buf, sizeof buf, "%zu,%zu,%.*f\n", r, c, prec, v);
            out << buf;
        }
    }
    if (!out) {
        return "write failed: " + path.string();
    }
    // Success — caller composes the status message.
    return std::nullopt;
}

// Convenience: NFD-driven save dialog wrapper. Default filename is
// "<table_id>[.diff].csv" so the file falls under the table being
// exported on disk without the user having to think about it.
void export_current_table_csv_dialog(AppState &state, bool diff_only) {
    if (!state.project.has_value() || state.selected_table_id.empty()) {
        state.status_msg = "Select a table first.";
        return;
    }
    std::string default_name = state.selected_table_id;
    if (diff_only)
        default_name += ".diff";
    default_name += ".csv";

    nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::SaveDialog(out_path, filters, 1, nullptr, default_name.c_str());
    if (r == NFD_CANCEL) {
        return;
    }
    if (r == NFD_ERROR) {
        state.status_msg = std::string{"Save dialog error: "} + NFD::GetError();
        return;
    }
    std::filesystem::path const path{out_path.get()};
    if (auto err = write_current_table_csv(state, path, diff_only); err.has_value()) {
        state.status_msg = "Export failed: " + *err;
        return;
    }
    state.status_msg = "Exported " + state.selected_table_id + (diff_only ? " (diff) " : " ") +
                       "to " + path.filename().string() + ".";
}

// Applies a previously-parsed CSV cell list to the currently-selected
// table as a single bulk edit recorded through edit::History. Used by
// both the import-preview modal's Apply button and any non-interactive
// caller. Returns nullopt on success or an error message.
std::optional<std::string> apply_parsed_csv_edits(AppState &state,
                                                  std::string const &target_table_id,
                                                  st::EditCsvParseResult const &parsed) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    if (parsed.cells.empty()) {
        return std::string{"Nothing to apply."};
    }
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!td.has_value()) {
        return "read working: " + td.error().to_string();
    }
    // Bounding rect over all touched cells — same shape the CLI records.
    std::size_t r_min = parsed.cells[0].row, r_max = parsed.cells[0].row;
    std::size_t c_min = parsed.cells[0].col, c_max = parsed.cells[0].col;
    for (auto const &e : parsed.cells) {
        r_min = std::min(r_min, e.row);
        r_max = std::max(r_max, e.row);
        c_min = std::min(c_min, e.col);
        c_max = std::max(c_max, e.col);
    }
    st::edit::Rect const rect{r_min, r_max, c_min, c_max};
    auto before = st::edit::snapshot(*td, rect);
    if (!before.has_value()) {
        return "snapshot before: " + before.error().to_string();
    }
    for (auto const &e : parsed.cells)
        td->values[e.row][e.col] = e.value;
    auto after = st::edit::snapshot(*td, rect);
    if (!after.has_value()) {
        return "snapshot after: " + after.error().to_string();
    }
    if (auto wb = state.project->definition().write_table_values(state.project->working_rom(),
                                                                 *table, *td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[64];
    std::snprintf(descbuf, sizeof descbuf, "csv import (%zu cell%s)", parsed.cells.size(),
                  parsed.cells.size() == 1 ? "" : "s");
    state.project->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                          std::move(*after), std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(*td);
    }
    state.dirty = true;

    std::string status = "Imported " + std::to_string(parsed.cells.size()) + " cell" +
                         (parsed.cells.size() == 1 ? "" : "s") + " into " + table->id + ".";
    if (!parsed.warnings.empty()) {
        status += "  Warning: " + parsed.warnings.front().message;
        if (parsed.warnings.size() > 1) {
            status += "  (+" + std::to_string(parsed.warnings.size() - 1) + " more)";
        }
    }
    state.status_msg = std::move(status);
    return std::nullopt;
}

// NFD-driven open dialog wrapper for CSV import. Parses the file
// through st::parse_edit_csv and, on success, stages the parsed cells
// in AppState and opens the preview modal — the apply happens only
// after the user explicitly confirms (matching the CLI's --dry-run
// safety net for shared / untrusted CSVs).
void import_csv_into_current_table_dialog(AppState &state) {
    if (!state.project.has_value() || state.selected_table_id.empty()) {
        state.status_msg = "Select a table first.";
        return;
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        state.status_msg = "Table not in pack.";
        return;
    }
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!td.has_value()) {
        state.status_msg = "Import failed: read working: " + td.error().to_string();
        return;
    }
    std::size_t const rows = td->values.size();
    std::size_t const cols = rows > 0 ? td->values[0].size() : 0;
    if (rows == 0 || cols == 0) {
        state.status_msg = "Import failed: cannot import into a "
                           "zero-dimension table.";
        return;
    }

    nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
    NFD::UniquePathU8 out_path;
    nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
    if (r == NFD_CANCEL) {
        return;
    }
    if (r == NFD_ERROR) {
        state.status_msg = std::string{"Open dialog error: "} + NFD::GetError();
        return;
    }
    std::filesystem::path const path{out_path.get()};

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        state.status_msg = "Import failed: cannot open " + path.string();
        return;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string const text = std::move(buf).str();

    st::EditCsvParseOptions opts;
    opts.expected_pack_id = state.project->definition().pack().id;
    opts.expected_table_id = table->id;
    opts.table_rows = rows;
    opts.table_cols = cols;
    auto parsed = st::parse_edit_csv(text, opts);
    if (!parsed.has_value()) {
        state.status_msg = "Import failed: " + parsed.error().to_string();
        return;
    }
    if (parsed->cells.empty()) {
        state.status_msg = "Import: no edit rows parsed; nothing to do.";
        return;
    }

    // Stash the parse + the current values for the modal preview.
    state.csv_import_parsed = std::move(*parsed);
    state.csv_import_table_id = table->id;
    state.csv_import_source_path = path;
    state.csv_import_before_values = std::move(*td);
    state.show_csv_import_modal = true;
}


// Undo/redo share the same restore-and-writeback shape; only the snapshot
// side and the rollback direction differ. `forward = false` for undo,
// `forward = true` for redo. Dispatches on payload kind — TableEdit goes
// through the definition's read_table_values / write_table_values seam,
// ByteEdit writes raw bytes directly (no table layer needed).
void apply_history_step(AppState &state, st::edit::Edit const &edit, bool forward) {
    auto const rollback_cursor = [&] {
        if (forward) {
            (void)state.project->history().undo();
        } else {
            (void)state.project->history().redo();
        }
    };

    if (auto const *te = edit.as_table(); te != nullptr) {
        auto const *tbl = state.project->definition().find_table(te->table_id);
        if (tbl == nullptr) {
            state.status_msg = "history: table not in pack: " + te->table_id;
            rollback_cursor();
            return;
        }

        auto td = state.project->definition().read_table_values(state.project->working_rom(), *tbl);
        if (!td.has_value()) {
            state.status_msg = "history re-read: " + td.error().to_string();
            rollback_cursor();
            return;
        }

        auto const &snap = forward ? te->after : te->before;
        if (auto s = st::edit::restore(*td, snap); !s.has_value()) {
            state.status_msg = "history restore: " + s.error().to_string();
            rollback_cursor();
            return;
        }

        auto wb =
            state.project->definition().write_table_values(state.project->working_rom(), *tbl, *td);
        if (!wb.has_value()) {
            state.status_msg = "history writeback: " + wb.error().to_string();
            rollback_cursor();
            return;
        }

        if (te->table_id == state.selected_table_id) {
            state.current_table_data = std::move(*td);
        }
    } else if (auto const *be = edit.as_byte(); be != nullptr) {
        auto &rom = state.project->working_rom();
        for (auto const &c : be->changes) {
            auto const v = forward ? c.after : c.before;
            if (auto s = rom.write_u8(c.address, v); !s.has_value()) {
                char buf[80];
                std::snprintf(buf, sizeof buf, "history byte writeback @0x%zX: ", c.address);
                state.status_msg = std::string{buf} + s.error().to_string();
                rollback_cursor();
                return;
            }
        }
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
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    char const *clip = ImGui::GetClipboardText();
    if (clip == nullptr || *clip == '\0')
        return;
    auto grid = parse_tsv(std::string_view{clip});
    if (grid.empty() || grid.front().empty())
        return;

    auto &td = *state.current_table_data;
    std::size_t const cur_r = state.selection.r_cursor;
    std::size_t const cur_c = state.selection.c_cursor;
    if (cur_r >= td.values.size() || td.values[cur_r].empty())
        return;

    // Clip the paste rect to actual table bounds (Excel-style truncate,
    // not wrap).
    std::size_t const grid_rows = grid.size();
    std::size_t grid_cols = 0;
    for (auto const &row : grid) {
        if (row.size() > grid_cols)
            grid_cols = row.size();
    }
    std::size_t const r1 = std::min<std::size_t>(cur_r + grid_rows - 1, td.values.size() - 1);
    std::size_t const c1 =
        std::min<std::size_t>(cur_c + grid_cols - 1, td.values[cur_r].size() - 1);

    // Snap selection to the paste destination so apply_op picks the
    // right rect.
    state.selection.r_anchor = cur_r;
    state.selection.r_cursor = r1;
    state.selection.c_anchor = cur_c;
    state.selection.c_cursor = c1;

    apply_op(state, "paste", [&grid, cur_r, cur_c](auto &t, auto rect) -> st::Status {
        for (std::size_t dr = 0;
             dr < grid.size() && cur_r + dr < t.values.size() && cur_r + dr <= rect.r_end; ++dr) {
            auto &tt_row = t.values[cur_r + dr];
            auto const &g_row = grid[dr];
            for (std::size_t dc = 0;
                 dc < g_row.size() && cur_c + dc < tt_row.size() && cur_c + dc <= rect.c_end;
                 ++dc) {
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
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl == nullptr) {
        state.status_msg = "Reset: table missing from pack";
        return;
    }
    // Read the source ROM through the same scaling pipeline as the
    // working ROM — so what we copy back is the exact value a fresh
    // open of the source would show, not raw bytes.
    auto src_td = state.project->definition().read_table_values(state.project->source_rom(), *tbl);
    if (!src_td.has_value()) {
        state.status_msg = "Reset: read source: " + src_td.error().to_string();
        return;
    }
    auto const rect = state.selection.as_rect();
    apply_op(state, "reset to source", [&src = *src_td, rect](auto &t, auto r) -> st::Status {
        for (std::size_t row = r.r_start;
             row <= r.r_end && row < t.values.size() && row < src.values.size(); ++row) {
            for (std::size_t col = r.c_start;
                 col <= r.c_end && col < t.values[row].size() && col < src.values[row].size();
                 ++col) {
                t.values[row][col] = src.values[row][col];
            }
        }
        (void)rect; // rect == r when apply_op fires
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

void execute_action(AppState &state, ConfirmAction action, std::filesystem::path const &path) {
    switch (action) {
    case ConfirmAction::None:
        break;
    case ConfirmAction::OpenDialog:
        open_project_dialog(state);
        break;
    case ConfirmAction::OpenRecent:
        state.try_open_project(path);
        break;
    case ConfirmAction::NewProject:
        state.show_new_project_modal = true;
        break;
    case ConfirmAction::Close:
        state.close_project();
        break;
    case ConfirmAction::Quit:
        state.user_confirmed_quit = true;
        break;
    }
}

void request_action(AppState &state, ConfirmAction action, std::filesystem::path path) {
    if (action == ConfirmAction::None)
        return;
    bool const need_confirm = state.dirty && state.project.has_value();
    if (need_confirm) {
        state.next_action = action;
        state.next_recent = std::move(path);
        state.show_unsaved_modal = true;
    } else {
        execute_action(state, action, path);
    }
}

// render_unsaved_modal moved to modals/unsaved.cpp (along with
// modal_save_label, modal_discard_label, modal_subtitle as file-local).
// render_csv_import_modal moved to modals/csv_import.cpp.
// render_maf_autotune_modal moved to modals/autotune_maf.cpp (along
// with run_maf_autotune_preview + apply_maf_autotune_proposal helpers).
// render_kp_autotune_modal moved to modals/autotune_knock.cpp (along
// with run_knock_pull_preview + apply_knock_pull_proposal helpers).
// render_flash_modal moved to modals/flash.cpp (along with PendingFlash
// + build_pending_flash as file-local).
// render_shortcuts_modal moved to modals/shortcuts.cpp (along with
// ShortcutRow, ShortcutGroup, shortcuts_reference as file-local).
// render_about_modal moved to modals/about.cpp.
// render_new_project_modal moved to modals/new_project.cpp.

void glfw_error_callback(int err, char const *desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

// Format the rect of `td` as TSV (rows on lines, cells tab-separated)
// and put it on the system clipboard via ImGui's clipboard helper.
// Format matches Excel/Sheets clipboard convention so pasted values
// round-trip cleanly to a spreadsheet for batch analysis.
void copy_rect_to_clipboard(st::Definition::TableData const &td, st::edit::Rect const &rect,
                            int precision) {
    std::string out;
    char buf[32];
    for (std::size_t r = rect.r_start; r <= rect.r_end && r < td.values.size(); ++r) {
        bool first = true;
        for (std::size_t c = rect.c_start; c <= rect.c_end && c < td.values[r].size(); ++c) {
            if (!first)
                out.push_back('\t');
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
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
        return;
    }
    int precision = 0;
    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl != nullptr) {
        auto const *scal = state.project->definition().find_scaling(tbl->scaling);
        if (scal != nullptr) {
            precision = scal->precision;
        }
    }
    copy_rect_to_clipboard(*state.current_table_data, state.selection.as_rect(), precision);
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
        while (line_end < text.size() && text[line_end] != '\n' && text[line_end] != '\r') {
            ++line_end;
        }
        std::string_view line{text.data() + i, line_end - i};
        std::vector<double> row;
        std::size_t cs = 0;
        while (cs <= line.size()) {
            std::size_t ce = cs;
            while (ce < line.size() && line[ce] != '\t')
                ++ce;
            std::string_view cell{line.data() + cs, ce - cs};
            // Trim whitespace at both ends.
            while (!cell.empty() && std::isspace(static_cast<unsigned char>(cell.front()))) {
                cell.remove_prefix(1);
            }
            while (!cell.empty() && std::isspace(static_cast<unsigned char>(cell.back()))) {
                cell.remove_suffix(1);
            }
            double v = 0.0;
            if (!cell.empty()) {
                std::string tmp{cell};
                (void)std::sscanf(tmp.c_str(), "%lf", &v);
                // sscanf failure leaves v at 0; tolerate so a stray
                // empty cell doesn't fail the whole paste.
            }
            row.push_back(v);
            if (ce >= line.size())
                break;
            cs = ce + 1;
        }
        if (!row.empty())
            grid.push_back(std::move(row));
        // Advance past the newline (handle both CRLF and LF).
        if (line_end < text.size()) {
            if (text[line_end] == '\r' && line_end + 1 < text.size() &&
                text[line_end + 1] == '\n') {
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
    if (needle.empty())
        return true;
    if (hay.size() < needle.size())
        return false;
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
        if (match)
            return true;
    }
    return false;
}

// Probe a few candidate paths and load the first one that exists. Returns
// nullptr if none was loadable, in which case ImGui's default font is used.
ImFont *load_first_existing(std::initializer_list<char const *> candidates, float size_px) {
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
    f.ui = load_first_existing(
        {
            "assets/fonts/Inter-Regular.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        },
        15.0f);

    // Mono — for grids, hex dumps, log output where alignment matters.
    f.mono = load_first_existing(
        {
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
// Shape + sizing settings — identical between themes. Only the palette
// differs; pulling these out keeps apply_theme() short and avoids
// re-stating the layout twice.
void apply_style_shape(ImGuiStyle &s) {
    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(8.0f, 5.0f);
    s.CellPadding = ImVec2(6.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    s.IndentSpacing = 20.0f;
    s.ScrollbarSize = 14.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.DockingSeparatorSize = 1.0f;
}

void apply_palette_dark(ImGuiStyle &s) {
    auto const [accent, accent_hover, accent_active] = accent_for(Theme::Dark);

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.16f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.45f, 0.71f, 0.45f);

    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.30f, 0.35f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.34f, 0.39f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.38f, 0.42f, 0.49f, 1.00f);

    c[ImGuiCol_CheckMark] = accent_active;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_active;

    c[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accent_hover;
    c[ImGuiCol_SeparatorActive] = accent_active;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ResizeGripHovered] = accent_hover;
    c[ImGuiCol_ResizeGripActive] = accent_active;

    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered] = accent_hover;
    c[ImGuiCol_TabSelected] = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);

    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    c[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = accent_hover;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accent_hover;

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.23f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.17f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    c[ImGuiCol_NavCursor] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

void apply_palette_light(ImGuiStyle &s) {
    // Same purple accent as the dark theme — keeps brand identity
    // consistent across both modes. Hover/active go DARKER (toward
    // saturated indigo) on the light background so they don't wash
    // out, opposite of the dark-theme convention.
    auto const [accent, accent_hover, accent_active] = accent_for(Theme::Light);

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.93f, 0.94f, 0.96f, 0.98f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_Text] = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.42f, 0.62f, 0.83f, 0.45f);

    c[ImGuiCol_FrameBg] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.78f, 0.81f, 0.86f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.74f, 0.76f, 0.80f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.66f, 0.69f, 0.73f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.58f, 0.62f, 0.67f, 1.00f);

    c[ImGuiCol_CheckMark] = accent_active;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_active;

    c[ImGuiCol_Button] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accent_hover;
    c[ImGuiCol_SeparatorActive] = accent_active;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_ResizeGripHovered] = accent_hover;
    c[ImGuiCol_ResizeGripActive] = accent_active;

    c[ImGuiCol_Tab] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0.91f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);

    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.96f, 0.97f, 0.98f, 1.00f);

    c[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.42f, 0.46f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = accent_hover;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accent_hover;

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.84f, 0.87f, 0.92f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.78f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.00f, 0.00f, 0.00f, 0.04f);

    c[ImGuiCol_NavCursor] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.20f, 0.20f, 0.20f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

// NavCursor was previously `accent`, which made it render as a bright
// outline around whichever widget last received focus. Combined with
// viewports/docking, this manifested as "one cell is highlighted and
// never unhighlights" in the data grid — even after disabling
// NavEnableKeyboard, the cursor color was still being applied wherever
// the nav system happened to land. We don't ship any explicit
// nav-focus indicator, so both palettes zero out NavCursor (above).
//
// With viewports enabled, OS-level windows render with their own
// alpha; force fully-opaque WindowBg so detached panels don't show
// through to the desktop.
void apply_theme(Theme t) {
    // Cache for chip helpers (chip_fg_warn etc) that need theme-aware
    // colors without threading Theme through every call site. ImGui is
    // single-threaded so no race.
    g_current_theme = t;

    auto &s = ImGui::GetStyle();
    apply_style_shape(s);
    if (t == Theme::Light)
        apply_palette_light(s);
    else
        apply_palette_dark(s);
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        s.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
}

// ===========================================================================
// Read-ROM-from-car modal
// ===========================================================================
//
// Renders the Tools → Read ROM from Car flow. State machine drives the body:
// Idle (form) → Running (progress) → Done (save dialog) / Failed (error) /
// Cancelled (status note). All long-running work runs on `state.read_rom_worker`;
// the modal just polls atomics + status enum on every frame.
//
// ----- v1.1 Write-ROM plan (NOT IMPLEMENTED — design notes only) -----
//
// The flash side of this same UX is meaningfully more dangerous than the
// read side, and the existing render_flash_modal already covers most of
// it for file-based flashing. The "Write ROM to Car" GUI button would be
// a thin wrapper that:
//
//   1. Routes through the same render_flash_modal policy gate (engine-
//      safety hard-stop + emissions confirmation + tuner-reason field).
//   2. Adds a "Pick adapter" sub-form identical to this read modal's
//      (transport kind + device/DLL path) — could literally share the
//      ImGui code via a render_adapter_picker(state) helper.
//   3. Adds a brick-protection confirmation (docs/05 §4): battery > 12.0V
//      via a UDS PID poll, ignition state check, etc.
//   4. Background-threads Flasher::execute(plan), with the same atomic
//      progress + cancel pattern this read modal uses. Flasher::execute
//      already emits a structured FlashReport per step — surface that
//      as a per-sector ledger in the modal body.
//   5. Verify pass (the existing post-erase-write loop in execute()
//      already does this). Failure path: keep partial-flash report
//      visible + offer "Resume from journal" if applicable.
//
// Scope to defer:
//   - Resume-from-journal UI is its own modal/dialog.
//   - Vehicle-state precondition checks (battery, ignition, transmission
//      in N/P) need new transport calls + a "preflight checks passed"
//      panel. Hardware-gated — wire after OBDX arrives.
// render_settings_modal moved to modals/settings.cpp.
// render_def_registry_modal moved to modals/def_registry.cpp.
// render_read_rom_modal moved to modals/read_rom.cpp.
// open_command_palette + render_command_palette moved to panels/command_palette.cpp
// (PaletteCommand / PaletteCommandKind / build_palette_commands /
// dispatch_palette_command are file-local there).

// render_menubar moved to panels/menubar.cpp.
// render_sidebar moved to panels/sidebar.cpp.
// render_workspace_rail + render_dockspace_host + apply_workspace_mode
// + build_workspace_layout moved to panels/workspace.cpp (g_request_dock_reset
// is now file-local there; request_layout_reset is exposed via panels.hpp).

// Center the current cursor's X for a piece of content of width `w` within
// the panel's current content region.
void center_cursor_x(float w) {
    float const avail = ImGui::GetContentRegionAvail().x;
    if (w < avail) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
    }
}

// Centered single-line text. Scale > 1.0 temporarily enlarges the font.
void text_centered(char const *text, float scale) {
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

// `TextDisabled` is the ImGui-canonical "this is inactive" color
// (~0.5 alpha, distinctly grey). It's the right choice for a
// disabled menu item or a button that won't fire. It's the WRONG
// choice for a subtitle or a piece of metadata — both want a
// muted-but-not-disabled look so the eye can still distinguish
// "kind of dim" (subtitle) from "actually inactive" (disabled).
// `text_subtle` fills that gap at ~0.65 alpha of the normal text
// color, regardless of theme. printf-style format string mirrors
// ImGui::Text / ImGui::TextDisabled so call sites can move over
// 1:1.
#if defined(__GNUC__)
// gnu_printf (not bare `printf`) accepts %zu / %ll etc.; bare
// `printf` on MinGW maps to the MS-printf check which rejects
// those conversions. ImGui::Text + TextDisabled don't carry a
// format attribute at all, so existing call sites with %zu
// already work — we annotate ours for the catch-mismatched-types
// benefit + match the gnu_printf semantics those sites assume.
[[gnu::format(gnu_printf, 1, 2)]]
#endif
void text_subtle(char const *fmt, ...) {
    auto const c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.x, c.y, c.z, c.w * 0.65f));
    va_list args;
    va_start(args, fmt);
    // `fmt` is the format-attribute-checked parameter of this function;
    // the static check at the call site has already validated it. Apple
    // Clang flags the forward into ImGui::TextV anyway, so suppress
    // -Wformat-nonliteral just for this delegation.
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    ImGui::TextV(fmt, args);
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif
    va_end(args);
    ImGui::PopStyleColor();
}

void text_centered_subtle(char const *text) {
    center_cursor_x(ImGui::CalcTextSize(text).x);
    text_subtle("%s", text);
}

// Centered empty-state for panels that have no data to show. Title is
// regular weight (reads as a heading); hint is subtle (reads as a
// caption). Top padding lifts the cluster off the panel's top edge so
// it doesn't read as a terse one-liner stuck against the title bar.
// Use for "no project loaded", "select a table first", "pack declares
// no DTCs" — wherever a panel needs to acknowledge a void rather than
// look broken.
void render_empty_state(char const *title, char const *hint) {
    // Adaptive top padding — 24px floor for short panels, 10% of
    // available height on tall ones so the cluster doesn't pin to
    // the top edge with a sea of void below. Empirically the 10%
    // anchor lands the cluster at a comfortable upper-third on
    // 600-800px panels without feeling lost in 2000px maximized
    // viewports.
    float const avail_h = ImGui::GetContentRegionAvail().y;
    float const top_pad = std::max(24.0f, avail_h * 0.10f);
    ImGui::Dummy(ImVec2(0.0f, top_pad));
    text_centered(title, 1.0f);
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_centered_subtle(hint);
}

// Push the brand-accent triple onto ImGui's button color stack. Call
// in pairs around exactly one ImGui::Button to render it as the
// primary action — same purple the welcome panel accent rule + the
// status-bar profile chip + tab-selected overline use. Reads from
// current_theme() so dark/light renders the right contrast variant.
void push_primary_button_colors() {
    auto const a = accent_for(current_theme());
    ImGui::PushStyleColor(ImGuiCol_Button, a.base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, a.active);
}

void pop_primary_button_colors() {
    ImGui::PopStyleColor(3);
}

// Small framed "tag" used to highlight a per-table attribute (unit, safety
// flag, …) without it competing with the title. Looks like a button but
// stays purely visual: the return value is ignored and the hover/active
// states match the resting state.
void chip(char const *text, ImVec4 fg, ImVec4 bg) {
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 2.0f));
    (void)ImGui::SmallButton(text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

// Common chip palettes — kept centrally so future flags pick from a small,
// coherent set rather than each call site rolling its own RGB. Each pair
// (fg, bg) is theme-aware: dark variants are pale-fg on dark-tint-bg, light
// variants flip to dark-fg on pale-tint-bg so contrast holds on white. The
// alpha on bg composites against WindowBg, so the chips read as tinted
// surfaces rather than slabs.
//
// Accent uses the brand purple from accent_for() — same purple as
// ButtonActive / HeaderActive / TabSelectedOverline / the welcome-panel
// accent rule, so when an "active" chip lights up it matches the rest of
// the active-element language.
ImVec4 chip_fg_accent() {
    return theme_is_light() ? ImVec4(0.32f, 0.18f, 0.55f, 1.0f)
                            : ImVec4(0.92f, 0.84f, 1.00f, 1.0f);
}
ImVec4 chip_bg_accent() {
    return theme_is_light() ? ImVec4(0.85f, 0.78f, 0.96f, 0.80f)
                            : ImVec4(0.32f, 0.20f, 0.50f, 0.55f);
}
ImVec4 chip_fg_warn() {
    return theme_is_light() ? ImVec4(0.55f, 0.32f, 0.05f, 1.0f)
                            : ImVec4(1.00f, 0.86f, 0.55f, 1.0f);
}
ImVec4 chip_bg_warn() {
    return theme_is_light() ? ImVec4(1.00f, 0.90f, 0.65f, 0.78f)
                            : ImVec4(0.42f, 0.30f, 0.08f, 0.60f);
}
ImVec4 chip_fg_caution() {
    return theme_is_light() ? ImVec4(0.46f, 0.40f, 0.05f, 1.0f)
                            : chip_fg_caution();
}
ImVec4 chip_bg_caution() {
    return theme_is_light() ? ImVec4(1.00f, 0.96f, 0.70f, 0.75f)
                            : ImVec4(0.34f, 0.32f, 0.08f, 0.55f);
}
ImVec4 chip_fg_muted() {
    return theme_is_light() ? ImVec4(0.32f, 0.36f, 0.42f, 1.0f)
                            : ImVec4(0.78f, 0.80f, 0.82f, 1.0f);
}
ImVec4 chip_bg_muted() {
    return theme_is_light() ? ImVec4(0.82f, 0.85f, 0.90f, 0.75f)
                            : ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
}
ImVec4 chip_fg_ok() {
    return theme_is_light() ? ImVec4(0.10f, 0.40f, 0.15f, 1.0f)
                            : ImVec4(0.70f, 0.94f, 0.72f, 1.0f);
}
ImVec4 chip_bg_ok() {
    return theme_is_light() ? ImVec4(0.75f, 0.93f, 0.78f, 0.75f)
                            : ImVec4(0.14f, 0.34f, 0.18f, 0.55f);
}
ImVec4 chip_fg_danger() {
    return theme_is_light() ? ImVec4(0.58f, 0.10f, 0.10f, 1.0f)
                            : ImVec4(1.00f, 0.75f, 0.72f, 1.0f);
}
ImVec4 chip_bg_danger() {
    return theme_is_light() ? ImVec4(1.00f, 0.82f, 0.80f, 0.80f)
                            : ImVec4(0.46f, 0.18f, 0.18f, 0.60f);
}
// Info — neutral / informational accent in the blue band. Distinct
// from chip_accent (brand purple) so info-tier indicators don't read
// as "active selection". Used for non-error/non-warn callouts like
// chunk indicators, "preview" labels, etc.
ImVec4 chip_fg_info() {
    return theme_is_light() ? ImVec4(0.10f, 0.32f, 0.62f, 1.0f)
                            : ImVec4(0.55f, 0.82f, 1.00f, 1.0f);
}
ImVec4 chip_bg_info() {
    return theme_is_light() ? ImVec4(0.78f, 0.88f, 1.00f, 0.75f)
                            : ImVec4(0.14f, 0.26f, 0.42f, 0.55f);
}

// Small "Preview" chip rendered inside a panel header. Replaces the
// older "(Preview)" suffix on menu labels + window titles, which read
// as "this is broken" rather than "API may shift." Info-band palette
// (low-alpha blue) keeps it visually quiet next to the panel title
// while still signaling status at a glance. Call right after the
// panel's title text on the same line.
void preview_pill() { chip("Preview", chip_fg_info(), chip_bg_info()); }

// render_welcome_panel moved to panels/welcome.cpp.
// render_stats_panel moved to panels/stats.cpp.
// render_knock_dashboard_panel moved to panels/knock_dashboard.cpp.
// render_adaptive_history_panel moved to panels/adaptive_history.cpp.
// render_coldstart_panel moved to panels/coldstart.cpp.
// render_ebcs_panel moved to panels/ebcs.cpp.
// render_dtcs_panel moved to panels/dtcs.cpp.
// render_history_panel moved to panels/history.cpp.
// render_features_designer moved to panels/features_designer.cpp.
// render_table_view moved to panels/table_view.cpp (along with
// GridStats, compute_stats, heatmap_color, text_right_aligned,
// render_table_heatmap, render_table_grid as file-local helpers).


// Status-bar TTL pass. Called once per frame before render_status_bar
// to fade out stale transient messages. Detects "new message" via a
// shadow string + per-message timestamp; after ~5s of the same
// content, clears it. Call sites just write to state.status_msg — no
// per-callsite TTL bookkeeping. Edge cases:
//  - Two consecutive identical messages (e.g. "Saved." twice) only
//    reset the timer on STRING change. That's the right trade-off:
//    rapid duplicate writes are usually the same event, and the
//    alternative (timer reset on every frame the string is set) would
//    pin the message forever.
//  - Time source is ImGui::GetTime() (wall-clock seconds since app
//    start, monotonic). No allocations, no calls outside ImGui.

// Render-frame callable shared between the main loop and GLFW's window-refresh
// callback. Windows enters a modal resize loop while the user drags a border;
// the main thread is blocked inside glfwPollEvents for the entire drag, so the
// only way to keep painting is to render from inside the WM_PAINT-driven
// refresh callback that GLFW dispatches during the modal loop.
//
// Lifted to outer st::ui scope so main() (at global scope) can address it
// after the anon namespace closes.
std::function<void()> g_render_frame;

} // namespace st::ui

int main(int argc, char *argv[]) {
    using namespace st::ui;
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == 0) {
        std::fprintf(stderr, "subuwutuner-gui: glfwInit failed\n");
        return 1;
    }

    // Stick to OpenGL 3.0 core-ish — the lowest target ImGui supports cleanly
    // across Win/Mac/Linux without needing extension-loader gymnastics.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow *window = glfwCreateWindow(1400, 880, "SubuwuTuner", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "subuwutuner-gui: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Set the window title-bar icon from the compiled-in RGBA blob
    // (icon_data.hpp, regenerated by scripts/embed_icon.py from
    // assets/icon.png). GLFW takes ownership of nothing — pixels stay
    // in our static .rodata segment. The Windows Explorer / taskbar
    // EXE icon is set separately via subuwutuner.rc + windres in
    // src/ui/CMakeLists.txt; both originate from the same PNG.
    {
        GLFWimage icon_image{};
        icon_image.width = st::ui::icon::kWidth;
        icon_image.height = st::ui::icon::kHeight;
        icon_image.pixels = const_cast<unsigned char *>(st::ui::icon::kRgba);
        glfwSetWindowIcon(window, 1, &icon_image);
    }

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
    // Force a tab bar on every docked window — even alone-in-node ones.
    // Without this, ImGui auto-hides the tab on single-window nodes and
    // there's no visible drag handle, so the panel can't be undocked or
    // moved by dragging once it's settled.
    io.ConfigDockingAlwaysTabBar = true;

    Fonts const fonts = load_fonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // RAII bracket for nfd's per-process state. Must outlive any dialog.
    NFD::Guard nfd_guard;

    AppState state;
    state.recents = load_recents();
    state.settings = load_settings();
    // Best-effort lookup of the bundled fixtures/demo.stune. Welcome
    // panel renders a "Try the demo project" button when set; absent
    // in installs that didn't ship the demo (the call returns
    // nullopt and the button just doesn't render).
    state.demo_project_path = resolve_demo_project_path(argc >= 1 ? argv[0] : nullptr);
    // Apply the persisted theme before any user-visible frame renders.
    apply_theme(state.settings.theme);
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
        // Ctrl+K opens the command palette. Highest-leverage discoverability
        // affordance — every menu item, every panel toggle, every recent
        // project, every table in the loaded pack is searchable from one
        // input. open_command_palette resets buffer + selection so each
        // open lands on a clean state.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K)) {
            open_command_palette(state);
        }

        tick_status_msg(state);
        render_menubar(state);
        // Workspace rail before the dockspace — its window position is
        // viewport-anchored and doesn't depend on dockspace layout, but
        // rendering it first keeps Z order intuitive when both happen
        // to overlap during the GLFW window-refresh callback path.
        render_workspace_rail(state);
        render_dockspace_host(state);
        render_sidebar(state);
        render_table_view(state, fonts);
        render_stats_panel(state);
        render_dtcs_panel(state);
        render_history_panel(state);
        render_knock_dashboard_panel(state);
        render_adaptive_history_panel(state);
        render_coldstart_panel(state);
        render_ebcs_panel(state);
        render_features_designer(state);
        render_status_bar(state);
        // Toasts last so they layer over panels + the status bar's
        // window (each toast is its own undecorated viewport-anchored
        // window). Render order doesn't matter for modals — those
        // dim the background regardless.
        render_toasts(state);
        render_unsaved_modal(state);
        render_csv_import_modal(state);
        render_new_project_modal(state);
        render_maf_autotune_modal(state);
        render_kp_autotune_modal(state);
        render_flash_modal(state);
        render_read_rom_modal(state);
        render_def_registry_modal(state);
        render_settings_modal(state);
        render_shortcuts_modal(state);
        render_about_modal(state);
        // Command palette rendered last so it stacks above every other
        // modal — Ctrl+K is meant as a global escape hatch.
        render_command_palette(state);

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
            std::string desired;
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

    // If a Read-ROM-from-Car worker is still running (user closed the GUI
    // mid-read instead of clicking Cancel), signal it to abort and join
    // before the AppState destructor runs. Without this the std::thread
    // destructor would terminate() on a joinable thread.
    if (state.read_rom_worker.joinable()) {
        if (state.read_rom_cancel)
            state.read_rom_cancel->store(true, std::memory_order_release);
        state.read_rom_worker.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
