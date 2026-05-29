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

namespace {

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
inline ImVec4 chip_fg_warn();
inline ImVec4 chip_fg_caution();
inline ImVec4 chip_fg_ok();
inline ImVec4 chip_fg_danger();
inline ImVec4 chip_fg_info();

// Forward decl for the centered empty-state helper — same forward-
// decl reason as the chip palette. Panels at lines ~5985+ call this.
void render_empty_state(char const *title, char const *hint);

// Centering / centered-text helpers — defined in the typography block
// near render_empty_state. Forward-declared here so modal renders
// (About modal at ~2841 calls these) can find them.
void center_cursor_x(float w);
void text_centered(char const *text, float scale);
void text_centered_subtle(char const *text);

// Vertical-rhythm scale. Use these in `ImGui::Dummy({0, kSpaceX})`
// instead of raw pixel values so the rhythm stays consistent and a
// single edit can re-tune the whole app's whitespace. Most callsites
// pick S (default row gap) or M (between paragraph-equivalents);
// L / XL anchor major section breaks and welcome-style breathing
// room.
//
// Off-grid values (2 / 8 / 12 / 14 / 18 / 28 px) stay as raw literals
// at their callsites — those are per-callsite visual tuning, not steps
// on this rhythm scale. Don't "fix" them to the nearest canonical
// without checking the rendered result.
constexpr float kSpaceXS = 4.0f;
constexpr float kSpaceS  = 6.0f;
constexpr float kSpaceM  = 10.0f;
constexpr float kSpaceL  = 16.0f;
constexpr float kSpaceXL = 24.0f;

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
struct AppState;
enum class ToastKind : std::uint8_t;
void enqueue_toast(AppState &state, ToastKind kind, std::string text);

struct Fonts {
    ImFont *ui = nullptr;   // Sans for UI chrome (menus, labels, panels)
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
    std::string opened_at; // ISO 8601 UTC, e.g. "2026-05-12T15:30:00Z"
    std::filesystem::path path;
};

constexpr std::size_t kRecentsCap = 8;

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
enum class Theme { Dark, Light };

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

// Accent triple — the three shades used everywhere the GUI emphasises
// a primary action (modal "Create" / "Apply" buttons, ButtonActive,
// HeaderActive, SliderGrabActive, etc.). Per-theme so contrast with
// the surrounding palette stays right (hover/active go LIGHTER on
// dark backgrounds, DARKER on light ones).
struct AccentTriple {
    ImVec4 base;
    ImVec4 hover;
    ImVec4 active;
};

inline AccentTriple accent_for(Theme t) noexcept {
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

inline Theme current_theme() noexcept {
    return g_current_theme;
}

// True when the active theme renders against a light surface — useful
// for picking foreground colors that need to flip dark to stay legible.
inline bool theme_is_light() noexcept {
    return g_current_theme == Theme::Light;
}

struct Settings {
    st::policy::Profile default_policy_profile{st::policy::Profile::MotorsportOnly};
    Theme theme{Theme::Dark};
};

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

// Anchor + cursor selection model. Click sets both; shift-click moves only
// the cursor — the cell rect runs between anchor and cursor inclusively.
// `enabled` distinguishes "nothing selected" from "single cell at (0,0)".
struct Selection {
    bool enabled{false};
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
            enabled = true;
        }
    }

    void reset() noexcept {
        enabled = false;
    }
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
    NewProject,
    Close,
    Quit,
};

// Transient on-screen feedback — replaces the older "set status_msg
// and let it linger forever" pattern for non-modal success / info
// callouts. Toasts stack bottom-right above the status bar, auto-
// expire after kToastLifetime, fade in the last kToastFadeWindow.
// Modal failures still go inline per the modal-inline-errors rule
// (memory feedback_modal_inline_errors); toasts are for surfaces
// the user isn't already looking at.
enum class ToastKind : std::uint8_t { Info, Success, Warn, Danger };

struct Toast {
    std::string text;
    ToastKind kind;
    std::chrono::steady_clock::time_point expires_at;
};

// ===========================================================================
// Adapter picker (shared between Read-ROM and future Write-ROM flows)
// ===========================================================================
//
// The Read-ROM modal needs a "pick your USB adapter" sub-form (transport
// kind dropdown + device path or DLL path field). The future Write-ROM
// modal needs an identical form. Factoring this into a self-contained
// state + render helper so both call sites share the same UX + parsing
// + spec-construction code path.
struct AdapterPickerState {
    int kind_idx{1};               // 0=J2534, 1=OBDX, 2=Native, 3=Trace (test)
    char device_path[256]{"COM5"}; // OBDX/Native; J2534/Trace hide this field
    char dll_path[1024]{};         // J2534 vendor DLL; others hide this
    char trace_path[1024]{};       // Trace (test) file path; others hide
};

// Returns true if `s` selects the trace-replay test mode (no real
// adapter — MockTransport + parse_uds_trace). Read-ROM and (future)
// Write-ROM modals branch on this to construct a MockTransport
// directly instead of calling open_transport.
[[nodiscard]] inline bool adapter_is_trace_mode(AdapterPickerState const &s) noexcept {
    return s.kind_idx == 3;
}

// Render the adapter-picker sub-form into the current ImGui window/popup.
// Returns true iff the form is filled in enough to enable a "go" button
// (kind selected + the matching path field non-empty). Intended to be
// called inside a modal/window started by the caller.
[[nodiscard]] inline bool render_adapter_picker(AdapterPickerState &s) {
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
[[nodiscard]] inline st::transport::TransportSpec
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

struct AppState {
    std::optional<st::Project> project;
    // Transient status line shown in the status bar's middle cluster.
    // Set whenever an action wants to surface a confirmation or a non-
    // fatal error ("Saved.", "Open dialog error: …"). Auto-clears
    // after ~5 s so stale messages don't sit indefinitely — the
    // tick_status_msg() pass in the render loop manages this; call
    // sites just write the string.
    std::string status_msg;
    // Change-detection shadow + first-seen timestamp for the auto-
    // clear pass. Touched only by tick_status_msg(). Internal — no
    // call site reads or writes these directly.
    std::string status_msg_prev;
    double status_msg_seen_at{0.0};
    std::string selected_table_id;
    std::optional<st::Definition::TableData> current_table_data;
    Selection selection;
    TableViewMode view_mode{TableViewMode::Grid};
    std::size_t selected_z{0};
    bool show_imgui_demo{false};
    bool show_shortcuts_modal{false};
    bool show_about_modal{false};

    // ISO 8601 UTC timestamp of the most recent save_project() success in
    // this session. Drives the "Saved 3 minutes ago" status-bar reading;
    // empty until the first save, which falls back to a plain "Clean"
    // chip (the project on disk is still saved — it just wasn't saved
    // by this session).
    std::optional<std::string> last_save_iso;

    // Path to the bundled fixtures/demo.stune project, resolved at
    // startup from argv[0] (best-effort, depends on the install
    // layout — dev tree, packaged, etc). Welcome panel renders a
    // "Try the demo project" button when this is set; absent in
    // installs that didn't ship the demo. See resolve_demo_project_path.
    std::optional<std::filesystem::path> demo_project_path;

    // Stacked transient feedback (Saved., Loaded foo.stune, Apply
    // failed: …). Rendered by render_toasts at the bottom-right
    // above the status bar; auto-expire. Bounded so a burst of
    // notifications doesn't tile the screen.
    std::vector<Toast> toasts;
    // Phase 5 custom-features designer. Hidden behind View → Debug.
    // Graph data model lives in st::feature; the wiring fields below
    // are transient editor state (only meaningful while the user is
    // mid-drag on a pin) and reset on completion or escape.
    bool show_features_designer{false};
    st::feature::Graph features_graph;
    bool features_wiring_active{false};
    st::feature::NodeId features_wiring_from_node{0};
    st::feature::PinId features_wiring_from_pin{0};
    // Set true when Esc / right-click cancels a wire; blocks the
    // pin-drag handler from spawning a fresh wire while the mouse
    // is still held over the source pin. Cleared when the mouse
    // button is released.
    bool features_wiring_blocked{false};
    // Last connect-attempt error. Surfaced under the canvas when a
    // wire is rejected (type mismatch, fan-in, etc.); cleared on the
    // next successful connect or when a drag starts.
    std::string features_wire_error;
    // Currently-selected nodes on the designer canvas. Plain click
    // replaces with {id}; Shift+click toggles a node in the set;
    // box-select via left-drag from empty canvas replaces with every
    // node whose body intersects the rubber-band rectangle. Cleared
    // on click of empty canvas, on graph clear/load, and when any
    // selected node is removed.
    std::vector<st::feature::NodeId> features_selected_nodes;
    // Rubber-band selection state. While `band_active` is true the
    // user is dragging out a rectangle from `band_start` (screen
    // coords) — selection is recomputed each frame from the
    // rectangle until release.
    bool features_band_active{false};
    ImVec2 features_band_start{0.0f, 0.0f};
    // Transient buffer for the pin-default-value editor opened from
    // the pin context menu. Pre-filled with the pin's current
    // default_value (or 0) when the popup is opened. The popup
    // commits on Apply / clears on Clear via pending_default below.
    float features_pin_edit_buf{0.0f};
    // Currently-selected edge on the designer canvas. Mutually
    // exclusive with the selected node — selecting either clears
    // the other. Cleared on graph clear/load, when the edge is
    // removed, or when either endpoint node is removed.
    std::optional<st::feature::Edge> features_selected_edge;
    // Edge captured by the most recent right-click, used only as a
    // reference for the edge context-menu popup. Independent of
    // `features_selected_edge` — dismissing the menu without acting
    // should leave the selection state unchanged.
    std::optional<st::feature::Edge> features_context_edge;
    // Canvas pan + zoom. `view_offset` translates graph-space
    // coords to screen-space (post-canvas-origin), `view_scale`
    // multiplies them. Defaults are identity. Middle-mouse drag
    // updates the offset; the mouse wheel updates the scale and
    // re-anchors the offset around the cursor.
    ImVec2 features_view_offset{0.0f, 0.0f};
    float features_view_scale{1.0f};
    // Loaded once at startup, persisted on every successful open. See
    // recents_config_path() for the on-disk location.
    std::vector<RecentEntry> recents;
    Settings settings;

    // Sidebar filter. Substring-matched (case-insensitive) against table
    // name + id. 128 chars is generous — table identifiers in real packs
    // top out around 40. `focus_table_filter` is the Ctrl+F handoff: set
    // by the main-loop shortcut, consumed by the sidebar's next render.
    char table_filter[128]{};
    bool focus_table_filter{false};

    // DTC-panel filter buffer. Same shape as table_filter; matched against
    // DTC code (P0401) or name.
    char dtc_filter[128]{};

    // Visibility toggles for the secondary panels. The Sidebar and Table
    // panels are always-on (closing them would orphan the user); these
    // are as-needed and are exposed via View menu checkboxes + the
    // standard dock tab-close X.
    bool show_stats_panel{true};
    bool show_dtcs_panel{true};
    bool show_history_panel{true};
    // Per-cylinder knock dashboard — v1.x feature, docs/05 §11. Hidden by
    // default (analysis tool, not always-on). State is fully self-contained:
    // a CSV log + column-name mapping + a cached snapshot. None of it
    // depends on having an open project.
    bool show_knock_dashboard_panel{false};
    char knock_log_path[1024]{};
    std::string knock_load_error;
    char knock_rpm_col[64]{"rpm"};
    char knock_load_col[64]{"load"};
    char knock_flkc_cols[6][64]{"flkc1", "flkc2", "flkc3", "flkc4", "flkc5", "flkc6"};
    char knock_fbkc_cols[6][64]{"fbkc1", "fbkc2", "fbkc3", "fbkc4", "fbkc5", "fbkc6"};
    int knock_cylinder_count{4};
    float knock_window_seconds{10.0f};
    float knock_sample_rate_hz{20.0f};
    float knock_min_rpm{1500.0f};
    float knock_min_load{1.5f};
    bool knock_gate_enabled{true};
    std::optional<st::log::knock::KnockSnapshot> knock_snapshot;
    std::string knock_compute_msg;
    // Adaptive-learning history panel — docs/05 §11 play 1. Same shape as
    // knock_*: CSV + column mapping + cached snapshot, fully self-contained.
    bool show_adaptive_history_panel{false};
    char ah_log_path[1024]{};
    std::string ah_load_error;
    char ah_ts_col[64]{"ts"};
    char ah_ltft_col[64]{"ltft"};
    char ah_dam_col[64]{"dam"};
    char ah_iac_col[64]{"iac"};
    float ah_bucket_seconds{86400.0f};
    int ah_ts_unit{0}; // 0=s,1=ms,2=us,3=rows
    int ah_min_samples_per_bucket{0};
    std::optional<st::log::adaptive::HistorySnapshot> ah_snapshot;
    std::string ah_compute_msg;
    // Cold-start analysis panel — docs/05 §11 play 3.
    bool show_coldstart_panel{false};
    char cs_log_path[1024]{};
    std::string cs_load_error;
    char cs_ts_col[64]{"ts"};
    char cs_ect_col[64]{"ect"};
    char cs_iat_col[64]{"iat"};
    char cs_rpm_col[64]{"rpm"};
    char cs_obs_col[64]{"obs"};
    char cs_cmd_col[64]{"cmd"};
    float cs_cold_threshold_c{55.0f};
    float cs_ect_bin_width_c{5.0f};
    int cs_min_samples_per_bin{2};
    int cs_ts_unit{0};
    // Target curve as a free-form text buffer; parsed on Compute.
    // Example: "0:0.82,20:0.88,40:0.95,55:1.00"
    char cs_target_curve[256]{"0:0.82,20:0.88,40:0.95,55:1.00"};
    std::optional<st::log::coldstart::ColdStartSnapshot> cs_snapshot;
    std::vector<std::pair<double, double>> cs_target_curve_parsed;
    std::string cs_compute_msg;
    // EBCS PID assistant panel — docs/05 §11 play 4.
    bool show_ebcs_panel{false};
    char ebcs_log_path[1024]{};
    std::string ebcs_load_error;
    char ebcs_ts_col[64]{"ts"};
    char ebcs_target_col[64]{"target_boost"};
    char ebcs_actual_col[64]{"actual_boost"};
    char ebcs_wgdc_col[64]{"wgdc"};
    char ebcs_throttle_col[64]{"throttle"};
    char ebcs_rpm_col[64]{"rpm"};
    float ebcs_throttle_step_pct{30.0f};
    float ebcs_target_step{2.0f};
    float ebcs_max_event_duration{6.0f};
    float ebcs_overshoot_warn_pct{15.0f};
    int ebcs_ts_unit{0};
    std::optional<st::log::ebcs::BoostSnapshot> ebcs_snapshot;
    std::string ebcs_compute_msg;

    // History-panel filter buffer. Substring-matched (case-insensitive)
    // against the edit's table_id; mirrors the CLI's project-history
    // --table flag at the GUI surface.
    char history_filter[128]{};

    // Inline cell-value editor state. Active iff `editing_cell` is true;
    // the cell being edited is identified by selection.r_cursor /
    // selection.c_cursor (these don't change while editing — the
    // arrow-key nav block reads `editing_cell` and skips its movement
    // logic). `editor_just_opened` is a one-frame handoff so the new
    // InputText gets SetKeyboardFocusHere on its first render.
    bool editing_cell{false};
    bool editor_just_opened{false};
    char edit_buf[64]{};

    // Unsaved-changes tracking. `dirty` is conservative: any edit flips
    // it true, save flips it false. An undo-back-to-clean leaves dirty
    // true, which is harmless because the resulting save is a no-op
    // against an already-correct file. Switching projects via the
    // modal's Discard option also clears dirty.
    bool dirty{false};
    ConfirmAction next_action{ConfirmAction::None};
    std::filesystem::path next_recent{};
    bool show_unsaved_modal{false};
    // Set when the user has confirmed (or there was nothing to confirm)
    // that they want to quit. Main loop reads this AFTER rendering each
    // frame and breaks when true.
    bool user_confirmed_quit{false};

    // Flash-flow modal state. `show_flash_modal` opens the popup on the
    // next frame; the rest are buffers for the confirm / reason UI bound
    // to the active project's profile. Fixed-size char buffer keeps the
    // ImGui call free of the `imgui_stdlib` std::string helper.
    bool show_flash_modal{false};
    bool flash_confirm_checked{false};
    char flash_reason[512]{};

    // CSV import preview modal. Populated by the Import dialog after a
    // successful parse; the modal renders a before->after preview and
    // applies the edit only on explicit user confirmation. Mirrors the
    // CLI's `project-edit-csv --dry-run` safety net for share-able CSVs.
    bool show_csv_import_modal{false};
    st::EditCsvParseResult csv_import_parsed;
    std::string csv_import_table_id;
    std::filesystem::path csv_import_source_path;
    // Working-ROM values at parse time, kept so the preview can show
    // "before -> after" without re-reading per frame.
    std::optional<st::Definition::TableData> csv_import_before_values;
    // Inline apply error. Set when "Apply edits" fails (e.g. the ROM
    // changed under us, an out-of-range cell snuck through, the table
    // disappeared from the def). Rendered above the buttons inside
    // the modal so the user sees the cause without losing the parsed
    // preview state — keeps the modal "non-intimidating" instead of
    // bouncing them out to the status bar.
    std::string csv_import_apply_error;

    // New-project modal. GUI front for `subuwutuner-cli project-new`.
    // Three path fields (source ROM, def pack folder OR single-file
    // pack, target project dir) and an optional display name. NFD
    // pickers populate the fields; Create runs Project::create +
    // try_open_project.
    bool show_new_project_modal{false};
    char np_source_path[1024]{};
    char np_def_path[1024]{};
    char np_dir_path[1024]{};
    char np_display_name[256]{};
    // Inline ROM/def match check. Recomputed only when either path
    // changes (cached_* mirrors the last-checked paths). Status drives
    // the colored line under the Definition row; load failure also
    // disables Create.
    enum class NpMatchStatus { None, Match, NoMatch, LoadFailed };
    NpMatchStatus np_match_status{NpMatchStatus::None};
    std::string np_match_message;
    std::string np_cached_source_path;
    std::string np_cached_def_path;
    // Surface Project::create's error inline — the bottom status bar
    // is hidden behind the modal, so the user otherwise sees nothing.
    std::string np_create_error;

    // MAF autotune modal. GUI front for `subuwutuner-cli
    // project-autotune-maf`. Inputs (table id, log path, tuning
    // params) live alongside the cached preview result so the modal
    // can show a per-cell ledger between Preview and Apply.
    bool show_maf_autotune_modal{false};
    char maf_at_table_id[128]{};
    char maf_at_log_path[1024]{};
    float maf_at_gain{0.5f};
    float maf_at_max_delta_pct{0.08f};
    int maf_at_min_samples{50};
    bool maf_at_apply_smooth{true};
    bool maf_at_require_open_loop{false};
    // Preview output. Populated by the Preview button; consumed by
    // the ledger display and by Apply. Cleared on Cancel or Apply.
    std::optional<st::autotune::MafTuneResult> maf_at_result;
    std::vector<st::autotune::LintViolation> maf_at_lints;
    std::optional<st::Definition::TableData> maf_at_table_data;
    std::string maf_at_status_msg;

    // Knock-pull autotune modal. GUI front for `subuwutuner-cli
    // project-autotune-knock-pull`. Mirrors the MAF modal's lifecycle:
    // table-id + log path + kernel params → Preview → 2D ledger →
    // Apply commits the proposal as a single undoable edit.
    bool show_kp_autotune_modal{false};
    char kp_at_table_id[128]{};
    char kp_at_log_path[1024]{};
    // Which of the pack's two table axes is RPM. Default 'y' matches
    // the common Subaru convention (axis_x = load, axis_y = engine
    // speed); the combo lets the user flip it for packs with the
    // opposite orientation. Backed by an int because ImGui::Combo wants
    // one — 0 = 'y', 1 = 'x'.
    int kp_at_rpm_axis_kind{0};
    float kp_at_trigger_degrees{1.5f};
    float kp_at_pull_step_degrees{0.75f};
    int kp_at_min_samples{30};
    bool kp_at_enable_add_back{false};
    float kp_at_add_step_degrees{0.5f};
    int kp_at_add_back_min_clean{50};
    float kp_at_clean_threshold{0.05f};
    std::optional<st::autotune::KnockPullResult> kp_at_result;
    std::vector<st::autotune::LintViolation> kp_at_lints;
    std::optional<st::Definition::TableData> kp_at_table_data;
    // Mirrors the rpm/load axis values used at preview time so the
    // ledger can label rows + columns without re-deriving the axis
    // orientation from the pack on each frame.
    std::vector<double> kp_at_rpm_axis_values;
    std::vector<double> kp_at_load_axis_values;
    std::string kp_at_status_msg;

    // ===== Read-ROM-from-car modal =====
    //
    // Pre-staged for the OBDX Pro VX adapter (ETA May 22-25 2026). The
    // CLI's `rom-pull` command works end-to-end against the MockTransport
    // today; this modal is the GUI front-end so the tuner workflow becomes
    //   Tools → Read ROM from Car → wait → save .bin → File → New project
    // instead of "open a terminal and type a long command".
    //
    // Lifecycle:
    //
    //   show_read_rom_modal=true on next frame  →  Idle
    //   user clicks Read                        →  Running (worker thread)
    //   worker progresses                       →  bytes_done / total_bytes
    //   worker finishes / errors / cancelled    →  Done / Failed / Cancelled
    //                                              (main joins the thread)
    //   user saves .bin via NFD                 →  back to Idle, popup closes
    //
    // Threading: the worker calls Flasher::read_full_rom on a std::thread;
    // progress is reported via an atomic counter the main loop reads each
    // frame; cancel is an atomic flag the main loop sets on click. Main
    // never blocks on the worker except at join time (status transition).
    bool show_read_rom_modal{false};
    enum class ReadRomState : std::uint8_t {
        Idle,      // form entry, no worker thread
        Running,   // worker reading; progress + cancel atomics live
        Done,      // worker finished OK; bytes_result has the dump
        Failed,    // worker errored; error_msg set
        Cancelled, // user-initiated abort; nothing to save
    };
    ReadRomState read_rom_state{ReadRomState::Idle};
    // Adapter form inputs (shared shape with the future write-rom flow —
    // see render_adapter_picker / adapter_picker_to_spec helpers).
    AdapterPickerState read_rom_adapter{};
    char read_rom_base_addr_hex[32]{"0x0"};
    char read_rom_size_hex[32]{"0x200000"}; // 2 MB default — VA/VB FA-DIT ROM size
    int read_rom_max_chunk{0x100};
    // 0 = SSM (VA WRX + every CAN-era Subaru that hasn't moved to UDS).
    // 1 = UDS (VB WRX + newer ISO 14229 ECUs). Default SSM because v1.0
    // targets the VA. Wrong choice on a real car = silent timeout
    // because the ECU drops the unknown SID without responding.
    int read_rom_protocol{0};
    // When set, the worker enables obdx::set_trace_enabled() for the
    // duration of the read, so every TX/RX frame is hex-dumped to
    // stderr. Default on while we're still bringing up real-hardware
    // I/O; users can flip it off when reads are reliably succeeding.
    bool read_rom_verbose{true};
    // When set, the worker performs UDS SecurityAccess (request seed,
    // compute key via Flasher::security_key_fn_, send key) BEFORE the
    // RMBA loop. Real Subaru ECUs reject RMBA with NRC 0x33
    // (securityAccessDenied) until SA succeeds. Defaults ON for real-
    // hardware UDS reads; the default key function is the stub which
    // returns NotImplemented, so the user has to either plug in a key
    // function or leave this off (and accept that RMBA will fail).
    bool read_rom_authenticate{true};
    // Worker state. The atomics live in shared_ptr storage so the worker
    // can outlive a destruct of AppState without crashing (defensive — in
    // practice we always join before tearing down).
    std::shared_ptr<std::atomic<std::uint32_t>> read_rom_bytes_done;
    std::shared_ptr<std::atomic<std::uint32_t>> read_rom_total_bytes;
    std::shared_ptr<std::atomic<bool>> read_rom_cancel;
    // Result handoff. Set by the worker before transitioning to Done/Failed.
    std::vector<std::uint8_t> read_rom_bytes_result;
    std::string read_rom_error_msg;
    // Change-detection shadow for the stderr error-mirror pass — see
    // tick_status_msg(). Internal; no call site reads or writes this.
    std::string read_rom_error_msg_prev;
    // The worker thread itself. Joined on state transition into a terminal
    // status; .joinable() is the canonical "is the thread alive" check.
    std::thread read_rom_worker;
    // Read start time for the status line ("elapsed: 4m13s").
    std::chrono::steady_clock::time_point read_rom_start_time;

    void try_open_project(std::filesystem::path const &path) {
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
        // New project = clean state. last_save_iso resets too — any
        // prior "Saved 3m ago" reading was for the previous project.
        dirty = false;
        last_save_iso.reset();
        // Success toast — the user just clicked "Open Project" or a
        // recent and the silent project-appears-in-the-grid was easy
        // to miss. Filename keeps it specific to which project loaded.
        enqueue_toast(*this, ToastKind::Success,
                      std::string{"Loaded "} + path.filename().string());
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
        last_save_iso.reset();
    }

    // --- Pack registry browser ---------------------------------------------
    // Tools -> Browse Definitions opens a modal that scans a directory via
    // st::defs::PackRegistry and lists every pack found (loose top-level,
    // per-platform loose, or zipped). Read-only for v1 — useful for
    // verifying the new <platform>.zip overlay loader (commit 571ca22)
    // sees the same packs as the loose-only walk.
    // --- Settings (Tools -> Settings...) ---------------------------------
    // Persists to %APPDATA%\SubuwuTuner\config.toml (Windows) /
    // $XDG_CONFIG_HOME/SubuwuTuner/config.toml (POSIX) via st::config.
    // Implements docs/25 §9 — Phase 2 step 6.
    bool show_settings_modal{false};
    char settings_def_root_input[1024]{};
    char settings_rom_dump_root_input[1024]{};
    bool settings_loaded_once{false};
    std::string settings_status_msg;
    ImVec4 settings_status_color{1.0f, 1.0f, 1.0f, 1.0f};

    bool show_def_registry_modal{false};
    char def_registry_root_input[1024]{"definitions"};
    // Cached scan result. Rebuilt when the user clicks Scan.
    bool def_registry_scanned{false};
    std::vector<std::string> def_registry_warnings;
    struct DefRegistryRow {
        std::string id;
        std::string platform;
        std::string source_label;
        std::string display_name;
    };
    std::vector<DefRegistryRow> def_registry_rows;
    char def_registry_filter[128]{};
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

// Snapshot, mutate, snapshot, writeback, record. If the writeback fails we
// restore the in-memory TableData so the rendered grid stays consistent with
// the ROM bytes — better than silently diverging.
template<typename Op>
void apply_op(AppState &state, std::string label, Op &&op) {
    if (!state.project.has_value() || !state.current_table_data.has_value() ||
        !state.selection.enabled) {
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
        (void)st::edit::restore(td, *before);
        state.status_msg = label + ": snapshot: " + after.error().to_string();
        return;
    }

    auto const *tbl = state.project->definition().find_table(state.selected_table_id);
    if (tbl == nullptr) {
        (void)st::edit::restore(td, *before);
        state.status_msg = label + ": table missing from pack";
        return;
    }

    auto wb =
        state.project->definition().write_table_values(state.project->working_rom(), *tbl, td);
    if (!wb.has_value()) {
        (void)st::edit::restore(td, *before);
        state.status_msg = label + ": writeback: " + wb.error().to_string();
        return;
    }

    state.project->history().record(st::edit::Edit::table(
        state.selected_table_id, std::move(*before), std::move(*after), std::move(label)));
    state.status_msg.clear();
    state.dirty = true;
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

void request_action(AppState &state, ConfirmAction action, std::filesystem::path path = {}) {
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

// Action-specific labels for the unsaved-changes modal. Keeping these
// pure verbs scoped to the modal so they don't accidentally read like
// general "open / close / quit" handlers elsewhere.
char const *modal_save_label(ConfirmAction a) noexcept {
    switch (a) {
    case ConfirmAction::OpenDialog:
    case ConfirmAction::OpenRecent:
        return "Save and open";
    case ConfirmAction::NewProject:
        return "Save and create new";
    case ConfirmAction::Close:
        return "Save and close";
    case ConfirmAction::Quit:
        return "Save and quit";
    case ConfirmAction::None:
        break;
    }
    return "Save and continue";
}

char const *modal_discard_label(ConfirmAction a) noexcept {
    switch (a) {
    case ConfirmAction::OpenDialog:
    case ConfirmAction::OpenRecent:
        return "Discard and open";
    case ConfirmAction::NewProject:
        return "Discard and create new";
    case ConfirmAction::Close:
        return "Discard and close";
    case ConfirmAction::Quit:
        return "Discard and quit";
    case ConfirmAction::None:
        break;
    }
    return "Discard changes";
}

char const *modal_subtitle(ConfirmAction a) noexcept {
    switch (a) {
    case ConfirmAction::OpenDialog:
    case ConfirmAction::OpenRecent:
        return "Opening another project will replace this one.";
    case ConfirmAction::NewProject:
        return "Creating a new project will replace this one.";
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
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes##unsaved", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ConfirmAction const what = state.next_action;

        ImGui::TextUnformatted("You have unsaved edits in this project.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        text_subtle("%s", modal_subtitle(what));
        ImGui::Dummy(ImVec2(0.0f, kSpaceL));

        // Keyboard shortcuts: Enter = the safe default (Save).
        // Esc = the safe undo (Cancel). Destructive Discard
        // requires an explicit click — no accelerator on purpose.
        bool const want_save = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
        bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);

        constexpr float kBtnW = 180.0f;
        // Save is the default action (Enter) and the safe path — give it
        // accent fill so the eye lands on it first.
        push_primary_button_colors();
        bool const save_clicked = ImGui::Button(modal_save_label(what), ImVec2(kBtnW, 0.0f));
        pop_primary_button_colors();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write the working ROM + edits to disk, "
                              "then proceed.  (Enter)");
        }
        ImGui::SameLine();
        bool const discard_clicked = ImGui::Button(modal_discard_label(what), ImVec2(kBtnW, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Throw away every edit since the last save "
                              "and proceed.");
        }
        ImGui::SameLine();
        bool const cancel_clicked = ImGui::Button("Cancel", ImVec2(kBtnW * 0.7f, 0.0f));
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
            state.dirty = false;
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
    st::flash::FlashPlan plan;
    st::flash::PolicyDecision decision;
    std::size_t total_bytes;
};

std::optional<PendingFlash> build_pending_flash(AppState const &state) {
    if (!state.project.has_value())
        return std::nullopt;
    auto const &proj = *state.project;
    if (proj.source_rom().size() != proj.working_rom().size())
        return std::nullopt;

    constexpr std::uint32_t kSectorSize = 0x1000;
    constexpr std::uint32_t kBaseAddress = 0;
    auto const sectors = st::flash::Flasher::compute_delta(
        proj.source_rom().data(), proj.working_rom().data(), kSectorSize, kBaseAddress);
    if (sectors.empty())
        return std::nullopt;

    PendingFlash pf;
    pf.total_bytes = 0;
    pf.plan.writes.reserve(sectors.size());
    for (auto const &s : sectors) {
        st::flash::SectorWrite sw;
        sw.sector = s;
        std::size_t const off = static_cast<std::size_t>(s.address - kBaseAddress);
        sw.data.assign(proj.working_rom().data().begin() + static_cast<std::ptrdiff_t>(off),
                       proj.working_rom().data().begin() +
                           static_cast<std::ptrdiff_t>(off + s.length));
        pf.total_bytes += s.length;
        pf.plan.writes.push_back(std::move(sw));
    }
    pf.decision = st::flash::evaluate_plan_policy(pf.plan, proj.definition(),
                                                  proj.source_rom().data(), proj.policy_profile());
    return pf;
}

// Preview modal for CSV import. Renders the parsed cell list as a
// before->after diff and only applies the edit on explicit user
// confirmation — the GUI equivalent of `project-edit-csv --dry-run`.
void render_csv_import_modal(AppState &state) {
    if (state.show_csv_import_modal) {
        ImGui::OpenPopup("Import CSV##csv_import_modal");
        state.show_csv_import_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 320.0f), ImVec2(900.0f, 700.0f));
    if (!ImGui::BeginPopupModal("Import CSV##csv_import_modal", nullptr,
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

// Runs the MAF auto-tune pipeline against the staged log path + tuning
// options and stashes the result + lints in AppState for the modal
// preview. Returns nullopt on success or an error message.
std::optional<std::string> run_maf_autotune_preview(AppState &state) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    std::string const target_table_id{state.maf_at_table_id};
    if (target_table_id.empty()) {
        return std::string{"Pick a target table."};
    }
    std::filesystem::path const log_path{state.maf_at_log_path};
    if (log_path.empty()) {
        return std::string{"Pick a CSV log."};
    }

    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    if (table->dimensions != 1) {
        return "Table '" + target_table_id +
               "' has dimensions=" + std::to_string(table->dimensions) +
               "; MAF scaling must be 1D.";
    }
    if (!table->axis_x.has_value() || table->axis_x->empty()) {
        return "Table '" + target_table_id + "' has no axis_x.";
    }
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
    if (!td.has_value()) {
        return "read table: " + td.error().to_string();
    }
    if (td->values.empty() || td->values[0].empty()) {
        return "Table is empty.";
    }
    auto const &axis_values = td->axis_x;
    std::vector<double> current(td->values[0].begin(), td->values[0].end());
    if (axis_values.size() != current.size()) {
        return "Axis length doesn't match current row length.";
    }

    // Slurp the log.
    std::ifstream in{log_path, std::ios::binary};
    if (!in) {
        return "cannot open log: " + log_path.string();
    }
    std::stringstream buf;
    buf << in.rdbuf();
    auto samples = st::autotune::read_maf_samples_csv(buf.str());
    if (!samples.has_value()) {
        return "log parse: " + samples.error().to_string();
    }

    st::autotune::MafTuneOptions opts;
    opts.gain = static_cast<double>(state.maf_at_gain);
    opts.max_delta_pct = static_cast<double>(state.maf_at_max_delta_pct);
    opts.min_samples_per_cell = static_cast<std::size_t>(std::max(0, state.maf_at_min_samples));
    opts.require_open_loop = state.maf_at_require_open_loop;

    auto result = st::autotune::tune_maf(axis_values, current, *samples, opts);
    if (!result.has_value()) {
        return "tune_maf: " + result.error().to_string();
    }
    if (state.maf_at_apply_smooth) {
        *result = st::autotune::smooth_proposals(*result, opts.max_delta_pct);
    }
    auto lints = st::autotune::lint_maf_proposal(axis_values, current, *result);

    // Stash for the modal preview + Apply.
    state.maf_at_result = std::move(*result);
    state.maf_at_lints = std::move(lints);
    state.maf_at_table_data = std::move(*td);
    return std::nullopt;
}

// Commits the cached MAF auto-tune proposal as a single edit::History
// entry on the target table. Mirrors the CLI's --apply branch.
std::optional<std::string> apply_maf_autotune_proposal(AppState &state) {
    if (!state.project.has_value()) {
        return std::string{"No project loaded."};
    }
    if (!state.maf_at_result.has_value() || !state.maf_at_table_data.has_value()) {
        return std::string{"Run Preview first."};
    }
    std::string const target_table_id{state.maf_at_table_id};
    auto const *table = state.project->definition().find_table(target_table_id);
    if (table == nullptr) {
        return "Table '" + target_table_id + "' not found in pack.";
    }
    auto td = *state.maf_at_table_data;
    auto const &result = *state.maf_at_result;

    if (result.cells.empty()) {
        // Defensive: an empty proposal would underflow `size() - 1` on
        // the rect below. Upstream guards prevent this in practice; a
        // future refactor of `tune_maf` might not.
        return std::string{"Empty proposal — nothing to apply."};
    }
    st::edit::Rect const rect{0, 0, 0, result.cells.size() - 1};
    auto before = st::edit::snapshot(td, rect);
    if (!before.has_value()) {
        return "snapshot before: " + before.error().to_string();
    }
    std::size_t modified = 0;
    for (auto const &c : result.cells) {
        if (c.proposed_value != c.current_value)
            ++modified;
        td.values[0][c.cell_index] = c.proposed_value;
    }
    auto after = st::edit::snapshot(td, rect);
    if (!after.has_value()) {
        return "snapshot after: " + after.error().to_string();
    }
    if (auto wb = state.project->definition().write_table_values(state.project->working_rom(),
                                                                 *table, td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[64];
    std::snprintf(descbuf, sizeof descbuf, "autotune maf (%zu cell%s)", modified,
                  modified == 1 ? "" : "s");
    state.project->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                          std::move(*after), std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(td);
    }
    state.dirty = true;
    state.status_msg = "Autotune MAF applied: " + std::to_string(modified) + " cell" +
                       (modified == 1 ? "" : "s") + " changed on " + table->id + ".";
    return std::nullopt;
}

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
    auto td = state.project->definition().read_table_values(state.project->working_rom(), *table);
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
    if (auto wb = state.project->definition().write_table_values(state.project->working_rom(),
                                                                 *table, td);
        !wb.has_value()) {
        return "writeback: " + wb.error().to_string();
    }
    char descbuf[80];
    std::snprintf(descbuf, sizeof descbuf, "autotune knock-pull (%zu pulled%s%s)", pulled,
                  added > 0 ? ", " : "",
                  added > 0 ? (std::to_string(added) + " added-back").c_str() : "");
    state.project->history().record(st::edit::Edit::table(table->id, std::move(*before),
                                                          std::move(*after), std::string{descbuf}));
    if (table->id == state.selected_table_id) {
        state.current_table_data = std::move(td);
    }
    state.dirty = true;
    state.status_msg = "Autotune knock-pull applied: " + std::to_string(pulled) + " pulled" +
                       (added > 0 ? (", " + std::to_string(added) + " added-back") : "") + " on " +
                       table->id + ".";
    return std::nullopt;
}

// Knock-pull autotune modal. GUI parity with `subuwutuner-cli
// project-autotune-knock-pull`: pick a target 2D table, pick a knock
// CSV, configure trigger / step / min-samples (+ optional add-back),
// Preview to see the proposed delta grid + lints, Apply to commit as a
// single edit::History entry.
void render_kp_autotune_modal(AppState &state) {
    if (state.show_kp_autotune_modal) {
        ImGui::OpenPopup("Autotune knock pull##kp_autotune_modal");
        state.show_kp_autotune_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(680.0f, 420.0f), ImVec2(1200.0f, 900.0f));
    if (!ImGui::BeginPopupModal("Autotune knock pull##kp_autotune_modal", nullptr,
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

// MAF autotune modal. GUI parity with `subuwutuner-cli
// project-autotune-maf`: pick a target table, pick a log CSV, set
// tuning params, Preview to see the proposed per-cell deltas + lint
// findings, Apply to commit as a single edit::History entry.
void render_maf_autotune_modal(AppState &state) {
    if (state.show_maf_autotune_modal) {
        ImGui::OpenPopup("Autotune MAF##maf_autotune_modal");
        state.show_maf_autotune_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 380.0f), ImVec2(1100.0f, 800.0f));
    if (!ImGui::BeginPopupModal("Autotune MAF##maf_autotune_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // ---- Inputs ------------------------------------------------------
    ImGui::TextUnformatted("Target table");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##maf_at_table", "snake_case id of the 1D MAF scaling table",
                             state.maf_at_table_id, sizeof state.maf_at_table_id);

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextUnformatted("Log CSV");
    {
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w - 8.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText("##maf_at_log", state.maf_at_log_path, sizeof state.maf_at_log_path,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Browse…##maf_at_log")) {
            nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
            if (r == NFD_OKAY) {
                std::snprintf(state.maf_at_log_path, sizeof state.maf_at_log_path, "%s", out.get());
                // Path changed — drop any stale preview.
                state.maf_at_result.reset();
                state.maf_at_lints.clear();
                state.maf_at_table_data.reset();
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::SeparatorText("Tuning");

    ImGui::PushItemWidth(180.0f);
    ImGui::SliderFloat("gain", &state.maf_at_gain, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fraction of the observed error to apply per\n"
                          "pass. 0.5 is a safe default; 1.0 commits the\n"
                          "full observed delta. Lower = more passes,\n"
                          "less risk per pass.");
    }
    ImGui::SliderFloat("max delta", &state.maf_at_max_delta_pct, 0.0f, 0.50f, "±%.0f%%",
                       ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Per-cell clamp on the proposed change. Caps\n"
                          "runaway moves on noisy or outlier-rich logs.");
    }
    ImGui::DragInt("min samples / cell", &state.maf_at_min_samples, 1.0f, 1, 10000);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cells with fewer than this many samples after\n"
                          "data-quality gates stay at their current value\n"
                          "and report with confidence = 0.");
    }
    ImGui::PopItemWidth();

    ImGui::Checkbox("smooth pass (neighbor weight 0.25)", &state.maf_at_apply_smooth);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Confidence-weighted neighbor smoothing per\n"
                          "docs/12 §MAF auto-tune. Disabled = raw per-cell\n"
                          "proposals; smooth re-clamps to ±max-delta.");
    }
    ImGui::Checkbox("require open-loop fueling", &state.maf_at_require_open_loop);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drop samples taken in closed-loop. Useful if\n"
                          "your log straddles both modes and you want to\n"
                          "tune from WOT pulls only.");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    ImGui::Spacing();

    bool const have_inputs = state.maf_at_table_id[0] != '\0' && state.maf_at_log_path[0] != '\0';
    bool run_clicked = false;
    {
        ImGui::BeginDisabled(!have_inputs);
        run_clicked = ImGui::Button("Run preview", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!have_inputs) {
                ImGui::SetTooltip("Pick a target table id and a log CSV first.");
            } else {
                ImGui::SetTooltip("Parse the log + run tune_maf + smooth + "
                                  "lint.  Nothing written yet.");
            }
        }
    }
    if (run_clicked) {
        if (auto err = run_maf_autotune_preview(state); err.has_value()) {
            state.maf_at_status_msg = *err;
            state.maf_at_result.reset();
            state.maf_at_lints.clear();
            state.maf_at_table_data.reset();
        } else {
            state.maf_at_status_msg.clear();
        }
    }

    if (!state.maf_at_status_msg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(chip_fg_danger(), "Preview failed: %s",
                           state.maf_at_status_msg.c_str());
    }

    // ---- Result -----------------------------------------------------
    if (state.maf_at_result.has_value()) {
        auto const &result = *state.maf_at_result;
        ImGui::Spacing();
        ImGui::SeparatorText("Preview");
        ImGui::Text("Samples: %zu (after gates: %zu)", result.total_samples,
                    result.samples_after_gates);

        std::size_t modified = 0;
        std::size_t underpowered = 0;
        for (auto const &c : result.cells) {
            if (c.confidence == 0.0)
                ++underpowered;
            else if (c.proposed_value != c.current_value)
                ++modified;
        }
        ImGui::Text("Modified: %zu / %zu cells, underpowered: %zu", modified, result.cells.size(),
                    underpowered);

        // Per-cell ledger.
        if (ImGui::BeginTable("##maf_at_ledger", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 240.0f))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
            ImGui::TableSetupColumn("axis", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("current", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("proposed", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("samples", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("conf", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            auto const &axis = state.maf_at_table_data->axis_x;
            for (auto const &c : result.cells) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", c.cell_index);
                ImGui::TableSetColumnIndex(1);
                if (c.cell_index < axis.size()) {
                    ImGui::Text("%.3f", axis[c.cell_index]);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.4f", c.current_value);

                ImGui::TableSetColumnIndex(3);
                ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                if (c.confidence == 0.0) {
                    color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                } else if (c.proposed_value > c.current_value) {
                    color = chip_fg_ok();
                } else if (c.proposed_value < c.current_value) {
                    color = chip_fg_danger();
                }
                ImGui::TextColored(color, "%.4f", c.proposed_value);

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%zu", c.samples_used);
                ImGui::TableSetColumnIndex(5);
                if (c.confidence == 0.0) {
                    ImGui::TextDisabled("-");
                } else {
                    ImGui::Text("%.2f", c.confidence);
                }
            }
            ImGui::EndTable();
        }

        // Lint findings, if any.
        if (!state.maf_at_lints.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(chip_fg_warn(),
                               "Lint findings (%zu):", state.maf_at_lints.size());
            for (auto const &v : state.maf_at_lints) {
                ImGui::BulletText("cells %zu..%zu — %s\n    %s", v.cell_index, v.cell_index + 1,
                                  pretty_lint_kind(v.kind), v.message.c_str());
            }
        }
    }

    // ---- Footer ------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool const have_preview = state.maf_at_result.has_value();
    bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
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
        state.maf_at_result.reset();
        state.maf_at_lints.clear();
        state.maf_at_table_data.reset();
        state.maf_at_status_msg.clear();
    };

    if ((apply_clicked && have_preview) || want_apply) {
        if (auto err = apply_maf_autotune_proposal(state); err.has_value()) {
            state.maf_at_status_msg = *err;
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

// Static help reference. Lists every binding the GUI listens for,
// grouped by context. Single source of truth — when a new shortcut
// lands, add a row here.
struct ShortcutRow {
    char const *binding;
    char const *description;
};
struct ShortcutGroup {
    char const *heading;
    std::vector<ShortcutRow> rows;
};

std::vector<ShortcutGroup> const &shortcuts_reference() {
    static std::vector<ShortcutGroup> const groups = {
        {"Global",
         {
             {"Ctrl+O", "Open project…"},
             {"Ctrl+S", "Save project"},
             {"Ctrl+Q", "Quit (with unsaved-changes guard)"},
             {"Ctrl+F", "Focus the sidebar table filter"},
             {"Ctrl+Z", "Undo last edit"},
             {"Ctrl+Shift+Z", "Redo (alt: Ctrl+Y)"},
         }},
        {"Table grid",
         {
             {"Arrows", "Move cursor cell"},
             {"Shift+Arrows", "Extend selection from cursor"},
             {"Click", "Select cell"},
             {"Shift+Click", "Extend selection to clicked cell"},
             {"F2", "Start editing the cursor cell"},
             {"Enter", "Commit value (or Ctrl+Enter to "
                       "fill every selected cell)"},
             {"Esc", "Cancel the in-cell editor"},
             {"Ctrl+C", "Copy selection as tab-separated "
                        "values"},
             {"Ctrl+V", "Paste tab-separated values at "
                        "cursor"},
         }},
        {"Modals",
         {
             {"Enter", "Primary action (Save / Apply / "
                       "Create — whatever is the "
                       "highlighted button)"},
             {"Esc", "Close / cancel without applying"},
         }},
        {"Designer canvas",
         {
             {"Click", "Select a node or edge"},
             {"Shift+Click", "Toggle a node in the selection"},
             {"Click (empty)", "Clear selection"},
             {"Drag (empty)", "Box-select nodes inside the "
                              "rectangle (Shift to add)"},
             {"Delete", "Remove the selected nodes / edge"},
             {"Drag body", "Move the selected nodes as a group "
                           "(snaps to grid on release)"},
             {"Drag pin → pin", "Wire two pins"},
             {"Middle-drag", "Pan the canvas"},
             {"Mouse wheel", "Zoom in/out (anchored to cursor)"},
             {"Reset view", "Toolbar button — reset pan + zoom"},
             {"Esc / Right-click", "Cancel an in-progress wire"},
             {"Right-click", "Context menu (delete node, "
                             "disconnect edge / pin)"},
         }},
    };
    return groups;
}

// Help → Keyboard shortcuts reference. Read-only, scrollable, two
// columns. Esc closes.
void render_shortcuts_modal(AppState &state) {
    if (state.show_shortcuts_modal) {
        ImGui::OpenPopup("Keyboard shortcuts##shortcuts_modal");
        state.show_shortcuts_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 380.0f), ImVec2(900.0f, 720.0f));
    if (!ImGui::BeginPopupModal("Keyboard shortcuts##shortcuts_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    text_subtle("All bindings the GUI listens for, grouped by "
                "context.");
    ImGui::Spacing();

    for (auto const &group : shortcuts_reference()) {
        ImGui::SeparatorText(group.heading);
        if (ImGui::BeginTable(group.heading, 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("description", ImGuiTableColumnFlags_WidthStretch);
            for (auto const &row : group.rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.binding);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.description);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();
    bool const close_clicked = ImGui::Button("Close", ImVec2(120.0f, 0.0f));
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void render_about_modal(AppState &state) {
    if (state.show_about_modal) {
        ImGui::OpenPopup("About SubuwuTuner##about_modal");
        state.show_about_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(440.0f, 0.0f), ImVec2(680.0f, 600.0f));
    if (!ImGui::BeginPopupModal("About SubuwuTuner##about_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // Title cluster — name + version + brand accent rule, mirrors the
    // welcome panel's typography so the "this is SubuwuTuner" voice is
    // consistent wherever it appears.
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_centered("SubuwuTuner", 1.6f);
    {
        constexpr float kRuleW = 48.0f;
        constexpr float kRuleH = 2.0f;
        center_cursor_x(kRuleW);
        ImVec2 const p = ImGui::GetCursorScreenPos();
        auto const a = accent_for(current_theme());
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImVec2(p.x + kRuleW, p.y + kRuleH),
            ImGui::GetColorU32(a.base), kRuleH * 0.5f);
        ImGui::Dummy(ImVec2(kRuleW, kRuleH + 4.0f));
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "v%.*s",
                      static_cast<int>(st::Version::string().size()),
                      st::Version::string().data());
        text_centered_subtle(buf);
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_centered_subtle(
        "A free, open-source Subaru ECU tuning suite in modern C++23.");

    ImGui::Dummy(ImVec2(0.0f, kSpaceL));
    ImGui::Separator();
    ImGui::Spacing();

    // Project facts — license, repo, build info. Two-column layout
    // (label / value) gives the eye a stable left rail to scan.
    if (ImGui::BeginTable("##about_facts", 2,
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        auto const row = [](char const *label, char const *value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            text_subtle("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value);
        };

        row("License",   "Apache-2.0 (see LICENSE)");
        row("Source",    "https://github.com/BuffJesus/SubuwuTuner");
        row("Built",     __DATE__ " " __TIME__);
#if defined(__clang__)
        row("Compiler",  "Clang " __clang_version__);
#elif defined(__GNUC__)
        {
            char gcc[32];
            std::snprintf(gcc, sizeof gcc, "GCC %d.%d.%d",
                          __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
            row("Compiler", gcc);
        }
#elif defined(_MSC_VER)
        {
            char msvc[32];
            std::snprintf(msvc, sizeof msvc, "MSVC %d", _MSC_VER);
            row("Compiler", msvc);
        }
#else
        row("Compiler",  "unknown");
#endif
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_subtle("Bundled libraries");
    ImGui::BulletText("Dear ImGui — Omar Cornut + contributors (MIT)");
    ImGui::BulletText("ImPlot — Evan Pezent + contributors (MIT)");
    ImGui::BulletText("GLFW — Marcus Geelnard + Camilla Lowy (zlib/libpng)");
    ImGui::BulletText("nativefiledialog-extended — btzy + Frogtoss Games (zlib)");
    ImGui::BulletText("tomlplusplus — Mark Gillard (MIT)");
    ImGui::BulletText("Catch2 — Catch Org + contributors (BSL-1.0)");
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
    text_subtle("Full text of each license: THIRD_PARTY_NOTICES.md");

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Spacing();

    bool const close_clicked = ImGui::Button("Close", ImVec2(120.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dismiss.  (Esc)");
    }
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// New-project modal. GUI parity with `subuwutuner-cli project-new`:
// three path inputs (source ROM, def pack folder, target dir) plus an
// optional display name. Each Browse… button fires NFD and rewrites
// the matching buffer. Create calls Project::create and, on success,
// hands the new directory off to try_open_project so the caller lands
// in the open project immediately.
void render_new_project_modal(AppState &state) {
    if (state.show_new_project_modal) {
        ImGui::OpenPopup("New project##new_project_modal");
        state.show_new_project_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 280.0f), ImVec2(900.0f, 600.0f));
    if (!ImGui::BeginPopupModal("New project##new_project_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // One row = label, text-input that fills the horizontal space, and
    // a Browse... button. ImGui::Begin/EndDisabled wraps the text input
    // to keep it read-only — the canonical-path constraint is "use
    // Browse..."; typing in random paths invites mistakes.
    auto const path_row = [](char const *label, char *buf, std::size_t buf_size, char const *btn_id,
                             char const *tooltip) -> bool {
        ImGui::TextUnformatted(label);
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w - 8.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText((std::string{"##"} + btn_id).c_str(), buf, buf_size,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        bool const clicked =
            ImGui::Button((std::string{"Browse…##"} + btn_id).c_str(), ImVec2(btn_w, 0.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return clicked;
    };

    if (path_row("Source ROM", state.np_source_path, sizeof state.np_source_path, "src",
                 "Pick the stock ROM dump (.bin). Copied into the\n"
                 "project as source.bin and never modified.")) {
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::OpenDialog(out);
        if (r == NFD_OKAY) {
            std::snprintf(state.np_source_path, sizeof state.np_source_path, "%s", out.get());
        } else if (r == NFD_ERROR) {
            state.status_msg = std::string{"Source dialog error: "} + NFD::GetError();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // Definition-pack row: custom because it needs two pickers
    // (multi-file directory layout vs. single-file pack).
    {
        ImGui::TextUnformatted("Definition pack");
        float const avail = ImGui::GetContentRegionAvail().x;
        float const btn_w = 96.0f;
        float const input_w = std::max(120.0f, avail - btn_w * 2.0f - 16.0f);
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText("##def_path", state.np_def_path, sizeof state.np_def_path,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Folder…##def_folder", ImVec2(btn_w, 0.0f))) {
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::PickFolder(out);
            if (r == NFD_OKAY) {
                std::snprintf(state.np_def_path, sizeof state.np_def_path, "%s", out.get());
            } else if (r == NFD_ERROR) {
                state.status_msg = std::string{"Def dialog error: "} + NFD::GetError();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pick a directory laid out as a multi-file\n"
                              "pack (must contain pack.toml).");
        }
        ImGui::SameLine();
        if (ImGui::Button("File…##def_file", ImVec2(btn_w, 0.0f))) {
            nfdu8filteritem_t filters[1] = {{"TOML pack", "toml"}};
            NFD::UniquePathU8 out;
            nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
            if (r == NFD_OKAY) {
                std::snprintf(state.np_def_path, sizeof state.np_def_path, "%s", out.get());
            } else if (r == NFD_ERROR) {
                state.status_msg = std::string{"Def dialog error: "} + NFD::GetError();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pick a single-file TOML pack\n"
                              "(one ROM/CID per file).");
        }
        // First-run hint. The single most common new-user question
        // ("where do I get a pack?") deserves a visible answer in
        // the modal, not a buried doc reference. Kept compact +
        // muted (text_subtle, not TextDisabled — this is a hint,
        // not an inactive control) so it doesn't compete with the
        // inputs.
        text_subtle("Don't have a pack? Try the bundled "
                    "fixtures/demo-pack/ to explore the UI, or see "
                    "docs/11-definition-format.md to author one.");
    }

    // Inline ROM/def match check. Recompute only when either path has
    // changed since the last run — both loaders touch disk, and the
    // modal would otherwise re-load every frame.
    if (state.np_source_path[0] != '\0' && state.np_def_path[0] != '\0') {
        std::string const cur_source = state.np_source_path;
        std::string const cur_def = state.np_def_path;
        if (cur_source != state.np_cached_source_path || cur_def != state.np_cached_def_path) {
            state.np_cached_source_path = cur_source;
            state.np_cached_def_path = cur_def;
            // Path changed — last create attempt's error is stale.
            state.np_create_error.clear();
            auto rom_r = st::Rom::from_file(cur_source);
            if (!rom_r.has_value()) {
                state.np_match_status = AppState::NpMatchStatus::LoadFailed;
                state.np_match_message = "ROM load failed: " + rom_r.error().to_string();
            } else {
                auto def_r = st::Definition::from_file(cur_def);
                if (!def_r.has_value()) {
                    state.np_match_status = AppState::NpMatchStatus::LoadFailed;
                    state.np_match_message = "Pack load failed: " + def_r.error().to_string();
                } else {
                    auto const name = def_r->matches(*rom_r);
                    if (name.has_value()) {
                        state.np_match_status = AppState::NpMatchStatus::Match;
                        state.np_match_message = "Matches \"" + *name + "\"";
                    } else {
                        state.np_match_status = AppState::NpMatchStatus::NoMatch;
                        state.np_match_message = "No matching identification "
                                                 "in this pack — Create still "
                                                 "allowed.";
                    }
                }
            }
        }
    } else {
        state.np_match_status = AppState::NpMatchStatus::None;
        state.np_match_message.clear();
        state.np_cached_source_path.clear();
        state.np_cached_def_path.clear();
    }
    if (state.np_match_status != AppState::NpMatchStatus::None) {
        ImVec4 color{1, 1, 1, 1};
        char const *prefix = "";
        switch (state.np_match_status) {
        case AppState::NpMatchStatus::Match:
            color = ImVec4(0.40f, 0.82f, 0.45f, 1.0f);
            prefix = "\xE2\x9C\x93 "; // ✓
            break;
        case AppState::NpMatchStatus::NoMatch:
            color = ImVec4(0.95f, 0.78f, 0.30f, 1.0f);
            prefix = "\xE2\x9A\xA0 "; // ⚠
            break;
        case AppState::NpMatchStatus::LoadFailed:
            color = chip_fg_danger();
            prefix = "\xE2\x9C\x97 "; // ✗
            break;
        case AppState::NpMatchStatus::None:
            break;
        }
        ImGui::TextColored(color, "%s%s", prefix, state.np_match_message.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    if (path_row("Project directory", state.np_dir_path, sizeof state.np_dir_path, "dir",
                 "Pick the empty target directory. project.toml,\n"
                 "source.bin, and working.bin land here.")) {
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::PickFolder(out);
        if (r == NFD_OKAY) {
            std::snprintf(state.np_dir_path, sizeof state.np_dir_path, "%s", out.get());
        } else if (r == NFD_ERROR) {
            state.status_msg = std::string{"Dir dialog error: "} + NFD::GetError();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    ImGui::TextUnformatted("Display name (optional)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##np_name", "Defaults to the project directory's basename.",
                             state.np_display_name, sizeof state.np_display_name);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Inline create error from a prior failed click, rendered ABOVE
    // the buttons so retries don't have to chase the eye to the
    // bottom of the modal. Kept compact (one red line) — detailed
    // diagnostics go in tooltips on the offending field.
    if (!state.np_create_error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "\xE2\x9C\x97 Create failed: %s",
                           state.np_create_error.c_str());
        ImGui::Spacing();
    }

    bool const have_source = state.np_source_path[0] != '\0';
    bool const have_def = state.np_def_path[0] != '\0';
    bool const have_dir = state.np_dir_path[0] != '\0';
    bool const load_failed = state.np_match_status == AppState::NpMatchStatus::LoadFailed;
    bool const can_create = have_source && have_def && have_dir && !load_failed;

    bool const want_cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);
    bool create_clicked = false;
    {
        push_primary_button_colors();
        ImGui::BeginDisabled(!can_create);
        create_clicked = ImGui::Button("Create", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        pop_primary_button_colors();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!have_source || !have_def || !have_dir) {
                ImGui::SetTooltip("Source ROM, definition pack, and "
                                  "project directory are all required.");
            } else if (load_failed) {
                ImGui::SetTooltip("Cannot create: ROM or pack failed to "
                                  "load. See message above.");
            } else {
                ImGui::SetTooltip("Create the project and open it.");
            }
        }
    }
    ImGui::SameLine();
    bool const cancel_clicked = ImGui::Button("Cancel", ImVec2(110.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Close without creating.  (Esc)");
    }

    auto const reset_fields = [&]() {
        state.np_source_path[0] = '\0';
        state.np_def_path[0] = '\0';
        state.np_dir_path[0] = '\0';
        state.np_display_name[0] = '\0';
        state.np_match_status = AppState::NpMatchStatus::None;
        state.np_match_message.clear();
        state.np_cached_source_path.clear();
        state.np_cached_def_path.clear();
        state.np_create_error.clear();
    };

    if (create_clicked && can_create) {
        std::filesystem::path const dir{state.np_dir_path};
        std::string name{state.np_display_name};
        if (name.empty()) {
            name = dir.filename().string();
            if (name.empty())
                name = "Untitled";
        }
        auto r = st::Project::create(dir, std::filesystem::path{state.np_source_path},
                                     std::filesystem::path{state.np_def_path}, name);
        if (!r.has_value()) {
            // Inline error only — status_msg would compete with the
            // modal's own surface. The error renders above the
            // buttons on next frame.
            state.np_create_error = r.error().to_string();
        } else {
            reset_fields();
            ImGui::CloseCurrentPopup();
            // try_open_project handles status_msg + recents update.
            state.try_open_project(dir);
        }
    } else if (cancel_clicked || want_cancel) {
        reset_fields();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void render_flash_modal(AppState &state) {
    if (state.show_flash_modal) {
        ImGui::OpenPopup("Flash...##flash_modal");
        state.show_flash_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Flash...##flash_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto const pending = build_pending_flash(state);

    if (!state.project.has_value()) {
        ImGui::TextUnformatted("No project loaded.");
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    if (!pending.has_value()) {
        ImGui::TextUnformatted("Working ROM matches source — nothing to flash.");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    auto const &d = pending->decision;
    auto const profile = state.project->policy_profile();
    auto const pname = std::string{st::policy::profile_name(profile)};
    using A = st::policy::Action;

    // Header: plan stats.
    ImGui::Text("Sectors: %zu   Bytes: %zu   Profile: %s", pending->plan.writes.size(),
                pending->total_bytes, pname.c_str());
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    // Engine-safety is a hard refusal across every profile.
    if (!d.engine_safety_tables.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("REFUSED: engine-safety-critical tables in plan");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        for (auto const &id : d.engine_safety_tables) {
            ImGui::BulletText("%s", id.c_str());
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        text_subtle("Engine-safety violations block in every profile (docs/06).");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    } else {
        text_subtle("No emissions-flagged tables touched.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    }

    // Action-specific UI: silent / confirm / confirm+reason.
    bool ready_to_send = true;
    switch (d.overall_action) {
    case A::Silent:
    case A::Badge:
        // The profile demands no user interaction — just go.
        text_subtle("Profile '%s' raises no gate for this plan.", pname.c_str());
        break;
    case A::Warn:
        text_subtle("Profile '%s' would flag this on save; no flash-time gate.", pname.c_str());
        break;
    case A::Confirm:
        ImGui::Checkbox("I confirm flashing these emissions edits", &state.flash_confirm_checked);
        ready_to_send = state.flash_confirm_checked;
        break;
    case A::ConfirmWithReason:
        ImGui::Checkbox("I confirm flashing these emissions edits", &state.flash_confirm_checked);
        ImGui::TextUnformatted("Reason (required):");
        ImGui::InputTextMultiline("##flash_reason", state.flash_reason, sizeof state.flash_reason,
                                  ImVec2(-FLT_MIN, 60.0f));
        ready_to_send = state.flash_confirm_checked && state.flash_reason[0] != '\0';
        break;
    case A::Block:
        // Distinct from the engine-safety branch above: profile-level
        // Block, e.g. a hypothetical future strict profile.
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("REFUSED by policy.");
        ImGui::PopStyleColor();
        ready_to_send = false;
        break;
    }

    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // Buttons. "Send to ECU" is intentionally never enabled in this build:
    // the GUI doesn't have a transport binding yet. The dry-run "Verify"
    // button completes the gate workflow without contacting hardware.
    //
    // Verify gets accent treatment + 160 wide to match the primary-action
    // convention across every other modal. No Enter shortcut on purpose:
    // this modal sits adjacent to the "actually flash bytes" workflow
    // (CLI today, in-app later) so accidental Enter-on-focus would feel
    // unsafe even though Verify itself sends nothing.
    bool verify_clicked = false;
    {
        push_primary_button_colors();
        ImGui::BeginDisabled(!ready_to_send);
        verify_clicked = ImGui::Button("Verify policy", ImVec2(160.0f, 0.0f));
        ImGui::EndDisabled();
        pop_primary_button_colors();
    }
    if (verify_clicked) {
        state.status_msg = "Flash plan cleared policy gate (" + pname + ").";
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (ready_to_send) {
            ImGui::SetTooltip("Acknowledge that the plan cleared the policy gate.\n"
                              "No bytes are sent to any ECU.");
        } else {
            ImGui::SetTooltip("Tick the confirm box (and fill the reason) first.");
        }
    }
    // "Send to ECU" stays hidden until a real transport binding lands
    // (Phase 3). Greying out a permanently-disabled CTA with the
    // explanation buried in a hover tooltip reads as "the app is
    // broken." Replacing it with an explicit inline note (below)
    // tells the user the same fact without the dead-button surface.
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("ECU send is CLI-only in this build (no in-app transport "
                        "binding yet). After Verify, drive the flash from:");
    ImGui::Indent();
    ImGui::TextDisabled("subuwutuner-cli project-flash <dir> --trace …");
    ImGui::Unindent();

    ImGui::EndPopup();
}

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
// Renders Tools -> Browse Definitions. A thin read-only window over
// st::defs::PackRegistry: pick a directory, see every pack the registry
// would surface (loose top-level, per-platform loose, zipped). Future
// commits wire this into the New-Project pack selector so the user can
// pick a pack-by-id instead of a file path.
// Tools -> Settings... — Phase 2 step 6 (docs/25 §9).
// Atomic save semantics handled by st::config::Config::save() (tmp +
// rename). After a successful save we reload PackRegistry so the new
// definitions_root takes effect without restarting the GUI.
void render_settings_modal(AppState &state) {
    if (state.show_settings_modal) {
        ImGui::OpenPopup("Settings##settings_modal");
        state.show_settings_modal = false;
        // Refresh from disk every time the modal opens so concurrent
        // edits via the CLI `config set` subcommand don't get clobbered.
        auto cfg_r = st::config::Config::load();
        if (cfg_r.has_value()) {
            std::snprintf(state.settings_def_root_input,
                          sizeof state.settings_def_root_input, "%s",
                          cfg_r->paths().definitions_root.string().c_str());
            std::snprintf(state.settings_rom_dump_root_input,
                          sizeof state.settings_rom_dump_root_input, "%s",
                          cfg_r->paths().rom_dump_root.string().c_str());
            state.settings_status_msg.clear();
        } else {
            state.settings_status_msg =
                "Load failed: " + cfg_r.error().to_string();
            state.settings_status_color = chip_fg_danger();
        }
        state.settings_loaded_once = true;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Settings##settings_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("Config file:");
    ImGui::SameLine();
    auto const cfg_path = st::config::default_config_path().string();
    ImGui::TextDisabled("%s", cfg_path.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s%s",
            cfg_path.c_str(),
            std::filesystem::exists(st::config::default_config_path())
                ? ""
                : "\n(not present — defaults in use until first Save)");
    }
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));

    float const avail = ImGui::GetContentRegionAvail().x;
    float const btn_w = 96.0f;
    float const input_w = std::max(120.0f, avail - btn_w - 16.0f);

    ImGui::TextUnformatted("Definitions root");
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputText("##settings_def_root", state.settings_def_root_input,
                     sizeof state.settings_def_root_input);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##settings_def_pick", ImVec2(btn_w, 0.0f))) {
        NFD::UniquePathU8 out;
        if (NFD::PickFolder(out) == NFD_OKAY) {
            std::snprintf(state.settings_def_root_input,
                          sizeof state.settings_def_root_input, "%s",
                          out.get());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Where PackRegistry looks for <platform>.zip archives and\n"
            "loose <id>.toml overrides. See docs/17 and docs/25.");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    ImGui::TextUnformatted("ROM dump root");
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputText("##settings_rom_root", state.settings_rom_dump_root_input,
                     sizeof state.settings_rom_dump_root_input);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##settings_rom_pick", ImVec2(btn_w, 0.0f))) {
        NFD::UniquePathU8 out;
        if (NFD::PickFolder(out) == NFD_OKAY) {
            std::snprintf(state.settings_rom_dump_root_input,
                          sizeof state.settings_rom_dump_root_input, "%s",
                          out.get());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Default destination for rom-pull captures when no\n"
            "--output is given. Must be writable by the running user\n"
            "(not Program Files).");
    }

    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

    // Button-row convention (matches unsaved_modal, csv_import_modal,
    // autotune modals, new_project_modal):
    //   primary: 160 wide, accent-filled, Enter shortcut, tooltip
    //   secondary/destructive: ~140 wide, no accent, tooltip
    //   cancel/close: ~110 wide, Esc shortcut, tooltip
    bool const want_save = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
    bool const want_close = ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false);

    bool save_clicked = false;
    {
        push_primary_button_colors();
        save_clicked = ImGui::Button("Save", ImVec2(160.0f, 0.0f));
        pop_primary_button_colors();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write definitions_root and rom_dump_root\n"
                              "to settings.toml.  (Enter)");
        }
    }
    if (save_clicked || want_save) {
        auto cfg_r = st::config::Config::load();
        if (!cfg_r.has_value()) {
            state.settings_status_msg =
                "Load failed: " + cfg_r.error().to_string();
            state.settings_status_color = chip_fg_danger();
        } else {
            cfg_r->paths().definitions_root = state.settings_def_root_input;
            cfg_r->paths().rom_dump_root = state.settings_rom_dump_root_input;
            auto s = cfg_r->save();
            if (s.has_value()) {
                state.settings_status_msg =
                    "Saved to " + cfg_r->source_path().string();
                state.settings_status_color = chip_fg_ok();
                // Invalidate the registry modal's cached scan so the
                // next open re-walks against the new definitions_root.
                state.def_registry_scanned = false;
                state.def_registry_rows.clear();
                state.def_registry_warnings.clear();
            } else {
                state.settings_status_msg =
                    "Save failed: " + s.error().to_string();
                state.settings_status_color = chip_fg_danger();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore Defaults", ImVec2(140.0f, 0.0f))) {
        auto defs = st::config::Config::defaults();
        std::snprintf(state.settings_def_root_input,
                      sizeof state.settings_def_root_input, "%s",
                      defs.paths().definitions_root.string().c_str());
        std::snprintf(state.settings_rom_dump_root_input,
                      sizeof state.settings_rom_dump_root_input, "%s",
                      defs.paths().rom_dump_root.string().c_str());
        state.settings_status_msg =
            "Defaults restored in form (click Save to persist).";
        state.settings_status_color = chip_fg_warn();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Populate the form with built-in defaults.\n"
                          "Click Save to persist them.");
    }
    ImGui::SameLine();
    bool const close_clicked = ImGui::Button("Close", ImVec2(110.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Close without persisting any pending changes.  (Esc)");
    }
    if (close_clicked || want_close) {
        ImGui::CloseCurrentPopup();
    }

    if (!state.settings_status_msg.empty()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        ImGui::PushStyleColor(ImGuiCol_Text, state.settings_status_color);
        ImGui::TextWrapped("%s", state.settings_status_msg.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndPopup();
}

void render_def_registry_modal(AppState &state) {
    if (state.show_def_registry_modal) {
        ImGui::OpenPopup("Browse Definitions##def_registry_modal");
        state.show_def_registry_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700.0f, 520.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Browse Definitions##def_registry_modal",
                                nullptr, ImGuiWindowFlags_None)) {
        return;
    }

    ImGui::TextUnformatted("Definitions root:");
    float const avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(avail - 200.0f);
    ImGui::InputText("##def_reg_root", state.def_registry_root_input,
                     sizeof state.def_registry_root_input);
    ImGui::SameLine();
    if (ImGui::Button("Folder...##def_reg_pick", ImVec2(80.0f, 0.0f))) {
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::PickFolder(out);
        if (r == NFD_OKAY) {
            std::snprintf(state.def_registry_root_input,
                          sizeof state.def_registry_root_input, "%s", out.get());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan##def_reg_scan", ImVec2(100.0f, 0.0f))) {
        state.def_registry_rows.clear();
        state.def_registry_warnings.clear();
        state.def_registry_scanned = true;
        auto reg_r = st::defs::PackRegistry::from_directory(
            std::filesystem::path{state.def_registry_root_input});
        if (!reg_r.has_value()) {
            state.status_msg = "Registry scan failed: " + reg_r.error().to_string();
        } else {
            for (auto const &w : reg_r->warnings()) {
                state.def_registry_warnings.push_back(w);
            }
            for (auto const &entry : reg_r->list_packs()) {
                AppState::DefRegistryRow row;
                row.id = entry.id;
                row.platform = entry.platform.empty() ? "—" : entry.platform;
                switch (entry.source) {
                case st::defs::PackEntry::Source::LooseTopLevel:
                    row.source_label = "loose-top";
                    break;
                case st::defs::PackEntry::Source::LoosePlatform:
                    row.source_label = "loose";
                    break;
                case st::defs::PackEntry::Source::Zipped:
                    row.source_label = "zipped";
                    break;
                }
                // Try to load + read the display name. Best-effort — if
                // parsing fails (corrupt or in-progress write), the row
                // still shows up with display_name = "(load failed)".
                auto def_r = reg_r->load_pack(entry.id);
                row.display_name = def_r.has_value()
                                       ? def_r->pack().display_name
                                       : std::string{"(load failed)"};
                state.def_registry_rows.push_back(std::move(row));
            }
            std::sort(state.def_registry_rows.begin(),
                      state.def_registry_rows.end(),
                      [](auto const &a, auto const &b) { return a.id < b.id; });
        }
    }

    if (state.def_registry_scanned) {
        ImGui::Separator();
        ImGui::Text("Found %zu pack(s)%s", state.def_registry_rows.size(),
                    state.def_registry_warnings.empty()
                        ? ""
                        : "  (warnings present)");
        if (!state.def_registry_warnings.empty()) {
            if (ImGui::TreeNode("warnings##def_reg_warn")) {
                for (auto const &w : state.def_registry_warnings) {
                    ImGui::TextWrapped("%s", w.c_str());
                }
                ImGui::TreePop();
            }
        }

        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##def_reg_filter", "filter by id or platform...",
                                 state.def_registry_filter,
                                 sizeof state.def_registry_filter);
        std::string filter_lc{state.def_registry_filter};
        std::transform(filter_lc.begin(), filter_lc.end(), filter_lc.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });

        ImGui::BeginChild("##def_reg_list", ImVec2(0.0f, -32.0f), true);
        if (ImGui::BeginTable("def_reg_table", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Id");
            ImGui::TableSetupColumn("Platform");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Display Name");
            ImGui::TableHeadersRow();
            for (auto const &row : state.def_registry_rows) {
                if (!filter_lc.empty()) {
                    auto contains = [&](std::string_view s) {
                        std::string lower{s};
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) {
                                           return static_cast<char>(std::tolower(c));
                                       });
                        return lower.find(filter_lc) != std::string::npos;
                    };
                    if (!contains(row.id) && !contains(row.platform)) {
                        continue;
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.id.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.platform.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(row.source_label.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(row.display_name.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    } else {
        ImGui::Separator();
        ImGui::TextDisabled("Click Scan to list packs at the chosen root.");
        ImGui::Dummy(ImVec2(0, 16));
        ImGui::TextWrapped(
            "The registry walks one level deep:\n"
            "  - <root>/<id>.toml         (loose, top-level)\n"
            "  - <root>/<platform>/<id>.toml  (loose, per-platform)\n"
            "  - <root>/<platform>.zip    (zipped archives)\n"
            "Loose files override zipped entries on id collision.");
    }

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void render_read_rom_modal(AppState &state) {
    if (state.show_read_rom_modal) {
        ImGui::OpenPopup("Read ROM from Car##read_rom_modal");
        state.show_read_rom_modal = false;
    }
    ImVec2 const center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // FIX (was: SetNextWindowSize + AlwaysAutoResize). The combination
    // of AlwaysAutoResize + TextWrapped inside the modal created a
    // shrink loop — each frame the auto-resize measured the wrapped
    // text's content width (smaller than the natural width), shrank
    // the window, which forced TextWrapped to wrap tighter on the
    // next frame, repeat. Visible as the modal walking right-to-left
    // until it pinned at minimum widget width.
    //
    // SizeConstraints with a 560 width floor prevents the shrink
    // (TextWrapped's wrap width is bounded below) while letting
    // height grow with content. Matches the pattern render_about_modal
    // and render_shortcuts_modal already use successfully.
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 240.0f),
                                        ImVec2(560.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("Read ROM from Car##read_rom_modal", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // ----- transition: Running → Done/Failed/Cancelled -----
    // Worker writes the terminal status, we join here so the std::thread
    // doesn't sit zombified across frames. Join is cheap once the worker
    // has already finished its loop.
    if (state.read_rom_worker.joinable() &&
        state.read_rom_state != AppState::ReadRomState::Running) {
        state.read_rom_worker.join();
    }

    auto const elapsed_str = [&] {
        auto const dur = std::chrono::steady_clock::now() - state.read_rom_start_time;
        auto const sec = std::chrono::duration_cast<std::chrono::seconds>(dur).count();
        long long mm = sec / 60, ss = sec % 60;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lldm %02llds", mm, ss);
        return std::string{buf};
    };

    // --------- Idle: form entry ---------
    if (state.read_rom_state == AppState::ReadRomState::Idle) {
        ImGui::TextUnformatted("Pulls a ROM dump from the connected ECU via the");
        ImGui::TextUnformatted("OBDX/J2534/native adapter. Read-only — no ECU writes.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_caution());
        ImGui::TextWrapped("Before clicking Read: ignition must be in ACC or RUN "
                           "(engine off is fine), nothing else (e.g. COBB AccessPort) "
                           "plugged into the OBD-II port, OBDX in the port. ECU is "
                           "asleep with key out — it won't answer.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));

        bool const adapter_ready = render_adapter_picker(state.read_rom_adapter);
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::InputText("Base address (hex)", state.read_rom_base_addr_hex,
                         sizeof state.read_rom_base_addr_hex);
        ImGui::InputText("Size (hex)", state.read_rom_size_hex, sizeof state.read_rom_size_hex);
        ImGui::Combo("Protocol", &state.read_rom_protocol,
                     "SSM2 (pre-2016 K-Line + early CAN)\0"
                     "UDS / SSM4 (2016+ WRX, FA20DIT, FA24)\0\0");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Wire-level protocol used to talk to the ECU.\n"
                "\n"
                "SSM2: Subaru Select Monitor 2 — A8 ReadByAddress.\n"
                "  Used by older EJ Subarus on K-Line and the early\n"
                "  CAN-era cars (≤ 2015 model year). Maps to the\n"
                "  dealer's SSMIII software in Subaru parlance.\n"
                "\n"
                "UDS / SSM4: ISO 14229 — SID 0x23 ReadMemoryByAddress.\n"
                "  Used by 2016+ Subarus (VA WRX 2016-2021, VB WRX\n"
                "  2022+, FA20DIT, FA24, Hitachi-built ECUs). Maps\n"
                "  to the dealer's SSM4 software, which is documented\n"
                "  as 'Generic OBDII of the Subaru Select Monitor 4'\n"
                "  in Subaru's own technician reference.\n"
                "\n"
                "Wrong choice = silent timeout (ECU drops the unknown\n"
                "SID without responding). For a 2015 VA WRX, try both:\n"
                "Subaru transitioned dealer software at MY 2016.");
        }
        ImGui::InputInt("Max chunk size (bytes)", &state.read_rom_max_chunk, 16, 64);
        if (state.read_rom_max_chunk < 16)
            state.read_rom_max_chunk = 16;
        if (state.read_rom_max_chunk > 0x1000)
            state.read_rom_max_chunk = 0x1000;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Bytes per request.\n"
                              "SSM: caps at 80 internally (single-frame limit);\n"
                              "     larger values are silently clamped.\n"
                              "UDS: 256 is the safe Subaru SH-2A default;\n"
                              "     some adapters tolerate up to 4096.");
        }
        ImGui::Checkbox("Verbose console output", &state.read_rom_verbose);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When on, every TX/RX frame is hex-dumped to the\n"
                              "spawned console as `[trace][obdx-tx]` / `[obdx-rx]`.\n"
                              "Useful while debugging silent timeouts — you can\n"
                              "see whether bytes are leaving the adapter and\n"
                              "whether the ECU is responding at all.");
        }
        ImGui::Checkbox("Authenticate (UDS SecurityAccess)", &state.read_rom_authenticate);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When on, performs UDS SID 0x27 seed/key exchange before\n"
                "the read loop. Real Subaru ECUs reject ReadMemoryByAddress\n"
                "with NRC 0x33 (securityAccessDenied) until SA succeeds.\n"
                "\n"
                "Shipped default (2026-05-24+): real Gen-A.2 L1 algorithm\n"
                "(SH7058 / SSMCAN1, ~2008-2017 Subarus) recovered analyst-\n"
                "side from the plaintext flash. The plug-in seam still\n"
                "exists — see src/ecu/include/st/ecu/security_key.hpp to\n"
                "swap in Gen-B (RH850/AES) or a vendor-supplied algorithm.\n"
                "\n"
                "SSM protocol path does not consult this checkbox — SSM\n"
                "does not gate ReadByAddress behind seed/key.");
        }

        // Pre-run errors (typically a parse failure from a previous click).
        if (!state.read_rom_error_msg.empty()) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
            ImGui::TextWrapped("%s", state.read_rom_error_msg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Dummy(ImVec2(0.0f, kSpaceM));

        bool const start_disabled = !adapter_ready;
        ImGui::BeginDisabled(start_disabled);
        if (ImGui::Button("Read", ImVec2(120.0f, 0.0f))) {
            // ----- parse form inputs into runtime values -----
            auto parse_hex_u32 = [](char const *s, std::uint32_t &out) {
                if (s == nullptr || s[0] == '\0')
                    return false;
                char *end = nullptr;
                unsigned long v = std::strtoul(s, &end, 0); // base=0 picks 0x or decimal
                if (end == s || *end != '\0')
                    return false;
                if (v > 0xFFFFFFFFul)
                    return false;
                out = static_cast<std::uint32_t>(v);
                return true;
            };
            std::uint32_t base_addr = 0, size = 0;
            if (!parse_hex_u32(state.read_rom_base_addr_hex, base_addr)) {
                state.read_rom_error_msg = "Base address: parse failed (use 0xNNN or decimal).";
            } else if (!parse_hex_u32(state.read_rom_size_hex, size)) {
                state.read_rom_error_msg = "Size: parse failed (use 0xNNN or decimal).";
            } else if (size == 0) {
                state.read_rom_error_msg = "Size must be > 0.";
            } else {
                bool const trace_mode = adapter_is_trace_mode(state.read_rom_adapter);
                // Real-transport branch builds a spec for open_transport;
                // trace branch carries the file path (worker loads it).
                st::transport::TransportSpec spec =
                    trace_mode ? st::transport::TransportSpec{}
                               : adapter_picker_to_spec(state.read_rom_adapter);
                std::string trace_path_str =
                    trace_mode ? std::string{state.read_rom_adapter.trace_path} : std::string{};

                state.read_rom_error_msg.clear();
                state.read_rom_bytes_result.clear();
                state.read_rom_bytes_done = std::make_shared<std::atomic<std::uint32_t>>(0);
                state.read_rom_total_bytes = std::make_shared<std::atomic<std::uint32_t>>(size);
                state.read_rom_cancel = std::make_shared<std::atomic<bool>>(false);
                state.read_rom_state = AppState::ReadRomState::Running;
                state.read_rom_start_time = std::chrono::steady_clock::now();

                AppState *st_ptr = &state;
                int const max_chunk = state.read_rom_max_chunk;
                int const protocol = state.read_rom_protocol;
                bool const verbose = state.read_rom_verbose;
                bool const authenticate = state.read_rom_authenticate;
                auto bytes_done_sp = state.read_rom_bytes_done;
                auto total_sp = state.read_rom_total_bytes;
                auto cancel_sp = state.read_rom_cancel;

                state.read_rom_worker = std::thread([st_ptr, spec, trace_mode, trace_path_str,
                                                     base_addr, size, max_chunk, protocol, verbose,
                                                     authenticate, bytes_done_sp, total_sp,
                                                     cancel_sp]() mutable {
                    // Owning storage. Trace-mode keeps a stack MockTransport;
                    // real-mode owns the unique_ptr from the factory.
                    st::transport::MockTransport mock;
                    std::unique_ptr<st::transport::ITransport> owned;
                    st::transport::ITransport *chosen = nullptr;

                    // Flip the process-wide OBDX hex-trace toggle for the
                    // duration of the worker; restore it on every exit
                    // path via the RAII guard below so a backgrounded
                    // datalog can't inherit a stuck-on trace flag.
                    bool const previous_trace = st::transport::obdx::trace_enabled();
                    st::transport::obdx::set_trace_enabled(verbose);
                    struct TraceGuard {
                        bool restore_to;
                        ~TraceGuard() { st::transport::obdx::set_trace_enabled(restore_to); }
                    } const trace_guard{previous_trace};

                    if (verbose) {
                        char const *const proto_name = protocol == 0 ? "SSM" : "UDS";
                        std::fprintf(stderr,
                                     "[trace][read-rom] starting %s read: "
                                     "base=0x%08X size=0x%X max_chunk=%d trace_mode=%d\n",
                                     proto_name, base_addr, size, max_chunk,
                                     trace_mode ? 1 : 0);
                        std::fflush(stderr);
                    }

                    if (trace_mode) {
                        std::vector<st::transport::UdsTracePair> pairs;
                        std::string err;
                        if (!st::transport::parse_uds_trace(std::filesystem::path{trace_path_str},
                                                            pairs, err)) {
                            st_ptr->read_rom_error_msg = std::move(err);
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        if (auto s = mock.open({}); !s.has_value()) {
                            st_ptr->read_rom_error_msg =
                                "MockTransport open failed: " + std::string{s.error().message()};
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        for (auto &p : pairs) {
                            mock.expect_send_recv(std::move(p.request), std::move(p.response));
                        }
                        chosen = &mock;
                    } else {
                        auto transport_r = st::transport::open_transport(spec);
                        if (!transport_r.has_value()) {
                            st_ptr->read_rom_error_msg = "Adapter open failed: " +
                                                         std::string{transport_r.error().message()};
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        // The OBDX/Native paths are fully wired via Win32
                        // serial (CreateFileA on \\.\COMx) — a bad device
                        // path surfaces a real Win32 error. What's
                        // empirically unverified is the OBDX DVI codec +
                        // UDS handshake against real adapter firmware.
                        auto open_r = (*transport_r)->open({});
                        if (!open_r.has_value()) {
                            st_ptr->read_rom_error_msg = "Adapter link open failed: " +
                                                         std::string{open_r.error().message()};
                            st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                            return;
                        }
                        owned = std::move(*transport_r);
                        chosen = owned.get();
                    }

                    // SSM-on-CAN strips the K-Line wrapper (per
                    // docs/13-transport.md). Real-hardware SSM mode opens
                    // the adapter on HS CAN (LinkConfig default
                    // CanIso15765), so the ECU expects bare `A8 00 + addrs`
                    // payloads. Trace mode replays K-Line-shaped fixtures
                    // — keep KLine framing there so existing trace files
                    // round-trip unchanged.
                    auto const ssm_framing = (!trace_mode && protocol == 0)
                                                 ? st::ecu::ssm::Framing::IsoTp
                                                 : st::ecu::ssm::Framing::KLine;
                    st::flash::Flasher flasher{*chosen, ssm_framing};
                    auto const progress_cb =
                        [bytes_done_sp, total_sp](st::flash::Flasher::ReadProgress p) {
                            bytes_done_sp->store(p.bytes_done, std::memory_order_release);
                            total_sp->store(p.total_bytes, std::memory_order_release);
                        };
                    auto result = protocol == 0
                                      ? flasher.read_full_rom_ssm(
                                            base_addr, size, static_cast<std::uint32_t>(max_chunk),
                                            std::chrono::milliseconds{1500}, progress_cb,
                                            cancel_sp.get())
                                      : flasher.read_full_rom(
                                            base_addr, size, static_cast<std::uint32_t>(max_chunk),
                                            std::chrono::milliseconds{1500}, progress_cb,
                                            cancel_sp.get(),
                                            /*enter_diagnostic_session=*/!trace_mode,
                                            /*authenticate=*/authenticate && !trace_mode);
                    if (!result.has_value()) {
                        if (result.error().code() == st::ErrorCode::Cancelled) {
                            if (verbose) {
                                std::fprintf(stderr,
                                             "[trace][read-rom] cancelled by user\n");
                                std::fflush(stderr);
                            }
                            st_ptr->read_rom_state = AppState::ReadRomState::Cancelled;
                            return;
                        }
                        if (verbose) {
                            std::fprintf(stderr, "[trace][read-rom] FAILED: %s\n",
                                         std::string{result.error().message()}.c_str());
                            std::fflush(stderr);
                        }
                        st_ptr->read_rom_error_msg = std::string{result.error().message()};
                        st_ptr->read_rom_state = AppState::ReadRomState::Failed;
                        return;
                    }
                    if (verbose) {
                        std::fprintf(stderr,
                                     "[trace][read-rom] done: %zu bytes received\n",
                                     result->size());
                        std::fflush(stderr);
                    }
                    st_ptr->read_rom_bytes_result = std::move(*result);
                    st_ptr->read_rom_state = AppState::ReadRomState::Done;
                });
            }
        }
        ImGui::EndDisabled();
        if (start_disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            char const *const which = state.read_rom_adapter.kind_idx == 0   ? "vendor DLL path"
                                      : state.read_rom_adapter.kind_idx == 3 ? "trace file path"
                                                                             : "device path";
            ImGui::SetTooltip("Fill in the %s field above.", which);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.read_rom_error_msg.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    // --------- Running: progress bar + cancel ---------
    if (state.read_rom_state == AppState::ReadRomState::Running) {
        std::uint32_t const done = state.read_rom_bytes_done->load(std::memory_order_acquire);
        std::uint32_t const total = state.read_rom_total_bytes->load(std::memory_order_acquire);
        float const frac = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "0x%X / 0x%X  (%.1f%%)", done, total,
                      static_cast<double>(100.0f * frac));
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        ImGui::Text("Elapsed: %s", elapsed_str().c_str());
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        text_subtle("Reading is slow (10-30 KB/s typical on SSM).");
        text_subtle("A 2 MB ROM usually takes 4-15 minutes.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
        if (ImGui::Button("Cancel read", ImVec2(160.0f, 0.0f))) {
            state.read_rom_cancel->store(true, std::memory_order_release);
        }
        ImGui::EndPopup();
        return;
    }

    // --------- Done: save .bin + offer to open as project ---------
    if (state.read_rom_state == AppState::ReadRomState::Done) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_ok());
        ImGui::TextUnformatted("Read complete.");
        ImGui::PopStyleColor();
        ImGui::Text("Got %zu bytes in %s.", state.read_rom_bytes_result.size(),
                    elapsed_str().c_str());
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));

        if (ImGui::Button("Save .bin...", ImVec2(160.0f, 0.0f))) {
            NFD::UniquePath out_path;
            nfdfilteritem_t const filters[] = {{"ROM image", "bin,hex"}};
            // Suggest a name from base+size for traceability. Buffer sized
            // for the literal prefix + two hex strings up to 31 chars each
            // (the actual struct field widths) — GCC -Wformat-truncation
            // computes the worst-case as ~72 B, so 128 is comfortable.
            char default_name[128];
            std::snprintf(default_name, sizeof default_name, "rom_%s_%s.bin",
                          state.read_rom_base_addr_hex, state.read_rom_size_hex);
            nfdresult_t const r = NFD::SaveDialog(out_path, filters, 1, nullptr, default_name);
            if (r == NFD_OKAY) {
                std::filesystem::path target{out_path.get()};
                std::ofstream fh{target, std::ios::binary};
                if (!fh) {
                    state.read_rom_error_msg =
                        "Failed to open output file for write: " + target.string();
                } else {
                    fh.write(reinterpret_cast<char const *>(state.read_rom_bytes_result.data()),
                             static_cast<std::streamsize>(state.read_rom_bytes_result.size()));
                    if (!fh) {
                        state.read_rom_error_msg = "Write failed (disk full?).";
                    } else {
                        state.status_msg = "Saved " + target.string() +
                                           ". Use File \xE2\x86\x92 New Project\xE2\x80\xA6 to start tuning.";
                        // Reset to Idle and close the popup.
                        state.read_rom_bytes_result.clear();
                        state.read_rom_state = AppState::ReadRomState::Idle;
                        state.read_rom_error_msg.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
            } else if (r == NFD_ERROR) {
                state.read_rom_error_msg = std::string{"Save dialog error: "} + NFD::GetError();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(120.0f, 0.0f))) {
            state.read_rom_bytes_result.clear();
            state.read_rom_state = AppState::ReadRomState::Idle;
            ImGui::CloseCurrentPopup();
        }
        if (!state.read_rom_error_msg.empty()) {
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
            ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
            ImGui::TextWrapped("%s", state.read_rom_error_msg.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
        return;
    }

    // --------- Failed / Cancelled: surface + back-to-Idle ---------
    if (state.read_rom_state == AppState::ReadRomState::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextUnformatted("Read failed.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        ImGui::TextWrapped("%s", state.read_rom_error_msg.c_str());
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    } else if (state.read_rom_state == AppState::ReadRomState::Cancelled) {
        ImGui::TextUnformatted("Read cancelled.");
        ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    }
    if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.read_rom_state = AppState::ReadRomState::Idle;
        state.read_rom_error_msg.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

constexpr float kStatusBarHeight = 26.0f;

// One transparent host window covering the work area (minus our manual status
// bar) hosts the dockspace. ImGuiDockNodeFlags_PassthruCentralNode keeps the
// empty central area transparent so the GL clear color shows through.
void render_dockspace_host() {
    auto const *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - kStatusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    auto const flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##dockspace_host", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID const id = ImGui::GetID("MainDockSpace");

    // On first ever run (no imgui.ini), set up a default split: Tables on the
    // left, Table view central. Once the user has a saved layout it's
    // respected automatically.
    if (ImGui::DockBuilderGetNode(id) == nullptr) {
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);

        ImGuiID left = 0;
        ImGuiID middle = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.22f, &left, &middle);
        ImGuiID right = 0;
        ImGuiID center = 0;
        ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, 0.25f, &right, &center);

        ImGui::DockBuilderDockWindow("Tables", left);
        ImGui::DockBuilderDockWindow("Table", center);
        ImGui::DockBuilderDockWindow("Stats", right);
        ImGui::DockBuilderDockWindow("History", right);

        ImGui::DockBuilderFinish(id);
    }

    ImGui::DockSpace(id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

void render_menubar(AppState &state) {
    bool const has_project = state.project.has_value();
    bool const can_undo = has_project && state.project->history().can_undo();
    bool const can_redo = has_project && state.project->history().can_redo();

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
            if (ImGui::MenuItem("New Project\xE2\x80\xA6")) {
                request_action(state, ConfirmAction::NewProject);
            }
            if (ImGui::MenuItem("Open Project\xE2\x80\xA6", "Ctrl+O")) {
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
            // CSV import/export — same `# pack_id` / `# table` /
            // `row,col,value` format as project-export-csv / -edit-csv,
            // and same parser, so the two surfaces round-trip with each
            // other.
            bool const can_csv = has_project && !state.selected_table_id.empty();
            if (ImGui::MenuItem("Import CSV into Table\xE2\x80\xA6", nullptr, false, can_csv)) {
                import_csv_into_current_table_dialog(state);
            }
            if (!can_csv) {
                disabled_tip("Select a table first.\n"
                             "Imports a row,col,value CSV as a single bulk edit\n"
                             "(undoable via Ctrl+Z).");
            }
            if (ImGui::MenuItem("Export Table as CSV\xE2\x80\xA6", nullptr, false, can_csv)) {
                export_current_table_csv_dialog(state, /*diff_only=*/false);
            }
            if (!can_csv) {
                disabled_tip("Select a table first.");
            }
            if (ImGui::MenuItem("Export Table Edits as CSV\xE2\x80\xA6", nullptr, false, can_csv)) {
                export_current_table_csv_dialog(state, /*diff_only=*/true);
            }
            if (!can_csv) {
                disabled_tip("Select a table first.\n"
                             "Emits only cells changed from the source — "
                             "share-able tune diff.");
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
            if (ImGui::MenuItem("Reset to Source", nullptr, false, has_selection)) {
                reset_selection_to_source(state);
            }
            if (has_project && !has_selection) {
                disabled_tip("Reverts the selected cells to their source-ROM\n"
                             "values (undoable).");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Autotune MAF\xE2\x80\xA6", nullptr, false, has_project)) {
                // Default the target table to whatever the user has open
                // — saves a step when they're already looking at the MAF
                // scaling. They can still type a different id.
                std::snprintf(state.maf_at_table_id, sizeof state.maf_at_table_id, "%s",
                              state.selected_table_id.c_str());
                state.maf_at_status_msg.clear();
                state.maf_at_result.reset();
                state.maf_at_lints.clear();
                state.maf_at_table_data.reset();
                state.show_maf_autotune_modal = true;
            }
            if (!has_project) {
                disabled_tip("Open a project first.\n"
                             "Runs the docs/12 MAF auto-tune kernel against\n"
                             "a CSV datalog and applies the proposal as a\n"
                             "single undoable edit.");
            }
            if (ImGui::MenuItem("Autotune Knock Pull\xE2\x80\xA6", nullptr, false, has_project)) {
                std::snprintf(state.kp_at_table_id, sizeof state.kp_at_table_id, "%s",
                              state.selected_table_id.c_str());
                state.kp_at_status_msg.clear();
                state.kp_at_result.reset();
                state.kp_at_lints.clear();
                state.kp_at_table_data.reset();
                state.kp_at_rpm_axis_values.clear();
                state.kp_at_load_axis_values.clear();
                state.show_kp_autotune_modal = true;
            }
            if (!has_project) {
                disabled_tip("Open a project first.\n"
                             "Runs the docs/12 knock-based ignition pull\n"
                             "kernel against a CSV datalog and applies the\n"
                             "2D proposal as a single undoable edit.");
            }
            ImGui::EndMenu();
        }
        if (!has_project && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            // Edit menu itself is disabled (BeginMenu second arg = false).
            ImGui::SetTooltip("No project open — open one to enable editing.");
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Read ROM from Car\xE2\x80\xA6")) {
                // Open the modal in Idle state. Leftover bytes_result from
                // a previous successful run get cleared so the modal opens
                // on the form, not on the post-read save dialog.
                state.read_rom_state = AppState::ReadRomState::Idle;
                state.read_rom_error_msg.clear();
                state.read_rom_bytes_result.clear();
                state.show_read_rom_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Dump the ECU's current calibration via the connected\n"
                                  "USB adapter (OBDX / J2534 / Native). Read-only — no\n"
                                  "ECU writes. Saves to a .bin you can then open as a\n"
                                  "new project via File \xE2\x86\x92 New Project\xE2\x80\xA6");
            }
            // Future: "Write ROM to Car..." (see plan comment above
            // render_read_rom_modal). Wired after OBDX adapter validation
            // + battery / ignition preflight checks land.
            ImGui::Separator();
            if (ImGui::MenuItem("Browse Definitions\xE2\x80\xA6")) {
                state.show_def_registry_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Walk a definitions directory via PackRegistry and\n"
                    "list every pack found (loose + zipped). Useful\n"
                    "for verifying a ship-time install layout where\n"
                    "<platform>.zip archives replace loose .toml files.");
            }
            if (ImGui::MenuItem("Settings\xE2\x80\xA6")) {
                state.show_settings_modal = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Edit the runtime config that PackRegistry and\n"
                    "rom-pull consult for default paths. Lives at\n"
                    "%%APPDATA%%\\SubuwuTuner\\config.toml. Equivalent\n"
                    "to running `subuwutuner-cli config set` from a\n"
                    "shell. See docs/25 for precedence rules.");
            }
            ImGui::EndMenu();
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
            // Panel visibility. Sidebar + Table are always-on (primary
            // navigation); Stats and DTCs are secondary panels the user
            // may want hidden when working in the table grid full-screen.
            ImGui::MenuItem("Stats Panel", nullptr, &state.show_stats_panel);
            ImGui::MenuItem("DTCs Panel", nullptr, &state.show_dtcs_panel);
            ImGui::MenuItem("History Panel", nullptr, &state.show_history_panel);
            ImGui::MenuItem("Knock Dashboard (Preview)", nullptr,
                            &state.show_knock_dashboard_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Per-cylinder knock dashboard. Load a CSV datalog,\n"
                                  "map RPM / load / per-cyl FLKC + FBKC columns,\n"
                                  "compute a windowed snapshot, view strip charts.\n"
                                  "See docs/05-improvements.md §11.");
            }
            ImGui::MenuItem("Adaptive History (Preview)", nullptr,
                            &state.show_adaptive_history_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Long-cycle LTFT / DAM / idle-adapt drift charts.\n"
                                  "Load a CSV with timestamps + per-signal columns,\n"
                                  "bucket by time (day default), view drift slope.\n"
                                  "See docs/05-improvements.md §11.");
            }
            ImGui::MenuItem("Cold-Start Analysis (Preview)", nullptr, &state.show_coldstart_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Phase-classify + ECT-bin a cold-start datalog.\n"
                                  "Compares observed lambda to a user-defined target\n"
                                  "curve at each ECT bucket. See docs/05 §11.");
            }
            ImGui::MenuItem("EBCS PID Assistant (Preview)", nullptr, &state.show_ebcs_panel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Detect tip-in events in a boost log; characterize each\n"
                                  "step response (rise / overshoot / settling); produce\n"
                                  "heuristic Kp/Ki/Kd gain-adjustment suggestions.\n"
                                  "Advisory only — verify on a dyno. See docs/05 §11.");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Theme")) {
                bool const is_dark = state.settings.theme == Theme::Dark;
                bool const is_light = state.settings.theme == Theme::Light;
                if (ImGui::MenuItem("Dark", nullptr, is_dark) && !is_dark) {
                    state.settings.theme = Theme::Dark;
                    apply_theme(Theme::Dark);
                    save_settings(state.settings);
                }
                if (ImGui::MenuItem("Light", nullptr, is_light) && !is_light) {
                    state.settings.theme = Theme::Light;
                    apply_theme(Theme::Light);
                    save_settings(state.settings);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // Custom features designer — Phase 5 user-facing surface.
            // Promoted out of Debug to top-level so users actually
            // discover it; the audit flagged that hiding it next to
            // the ImGui demo made it read as a developer escape hatch.
            ImGui::MenuItem("Custom Features Designer (Preview)", nullptr,
                            &state.show_features_designer);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Phase 5 node-graph editor for authoring custom\n"
                                  "features (rev limiters, flat-foot shift, etc.).\n"
                                  "Graph + IR + SH-2A codegen are implemented;\n"
                                  "wire-up to flash lands in Phase 5 patch insertion.");
            }
            ImGui::Separator();
            // Dev-only escape hatch. Tucked one level deeper so the
            // ImGui example isn't a peer of the user-facing view modes.
            if (ImGui::BeginMenu("Debug")) {
                ImGui::MenuItem("ImGui demo window", nullptr, &state.show_imgui_demo);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("SubuwuTuner %.*s", static_cast<int>(st::Version::string().size()),
                        st::Version::string().data());
            ImGui::Separator();
            if (ImGui::MenuItem("Keyboard Shortcuts\xE2\x80\xA6")) {
                state.show_shortcuts_modal = true;
            }
            if (ImGui::MenuItem("About SubuwuTuner\xE2\x80\xA6")) {
                state.show_about_modal = true;
            }
            ImGui::Separator();
            text_subtle("Getting started");
            ImGui::BulletText(
                "File \xE2\x86\x92 Open Project\xE2\x80\xA6 (Ctrl+O) to pick a .stune directory.");
            ImGui::BulletText("Or pass one on the command line: subuwutuner-gui my.stune");
            ImGui::BulletText(
                "No project of your own? Open the bundled fixtures/demo.stune/ to "
                "explore the UI.");
            ImGui::Separator();
            text_subtle("Editing");
            ImGui::BulletText("Click cells to select; Shift-click to extend.");
            ImGui::BulletText("Arrow keys move the cursor; Shift+arrows extend.");
            ImGui::BulletText(
                "F2 or double-click a cell to type a new value.  Enter commits, Esc cancels.");
            ImGui::BulletText(
                "Ctrl+Enter while editing fills every selected cell with the typed value.");
            ImGui::BulletText(
                "Ctrl+C / Ctrl+V copy and paste the selection as tab-separated values.");
            ImGui::BulletText("Right-click any cell for Copy / Paste / Reset to Source.");
            ImGui::BulletText(
                "Toolbar buttons (+5%%, -5%%, Smooth, Interpolate) act on the selection.");
            ImGui::BulletText("Ctrl+Z / Ctrl+Shift+Z to undo / redo.  Ctrl+S to save.");
            ImGui::Separator();
            text_subtle("Navigation");
            ImGui::BulletText("Ctrl+F focuses the table-filter box.  Esc clears it.");
            ImGui::BulletText("Filter matches both the table's name and its snake_case id.");
            ImGui::Separator();
            text_subtle("Viewing");
            ImGui::BulletText("Switch View: Grid \xE2\x86\x94 Heatmap to inspect a map two ways.");
            ImGui::BulletText("For 3D tables, pick a Z slice above the grid.");
            ImGui::Separator();
            text_subtle("Documentation");
            ImGui::BulletText("Repo:    https://github.com/BuffJesus/SubuwuTuner");
            ImGui::BulletText(
                "Design:  docs/00-overview.md \xE2\x80\xA6 docs/16-custom-features.md");
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
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to browse its calibration tables here.");
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
    ImGui::InputTextWithHint("##table_filter", "Filter tables…  (Ctrl+F)", state.table_filter,
                             sizeof state.table_filter, ImGuiInputTextFlags_EscapeClearsAll);

    std::string_view const filter{state.table_filter};
    // Count matches once so the active-filter header line can report
    // "N of M". When the filter is empty, the count is noise — the tree
    // below already conveys "how many tables" at a glance.
    std::size_t matched = 0;
    for (auto const &t : def.tables()) {
        if (filter.empty() || icontains(t.name, filter) || icontains(t.id, filter)) {
            ++matched;
        }
    }
    if (!filter.empty()) {
        text_subtle("%zu of %zu tables", matched, def.tables().size());
        ImGui::Separator();
    }

    if (!filter.empty() && matched == 0) {
        char title[80];
        std::snprintf(title, sizeof title, "No tables match \"%s\"", state.table_filter);
        render_empty_state(
            title,
            "Try a shorter prefix, or press Esc to clear the filter.");
        ImGui::End();
        return;
    }

    // Group tables by category, preserving first-occurrence order so
    // pack authors get to control the group ordering. Tables without a
    // category fall into "Other"; that group lands wherever the first
    // uncategorized table appears (predictable, not magic-sorted).
    struct Group {
        std::string_view name;
        std::vector<std::size_t> indices; // into def.tables()
    };
    std::vector<Group> groups;
    groups.reserve(8);
    auto const find_or_make = [&](std::string_view cat) -> Group & {
        for (auto &g : groups) {
            if (g.name == cat)
                return g;
        }
        groups.push_back({cat, {}});
        return groups.back();
    };
    for (std::size_t i = 0; i < def.tables().size(); ++i) {
        auto const &t = def.tables()[i];
        std::string_view const cat =
            t.category.empty() ? std::string_view{"Other"} : std::string_view{t.category};
        find_or_make(cat).indices.push_back(i);
    }

    auto const table_matches = [&](st::Table const &t) {
        return filter.empty() || icontains(t.name, filter) || icontains(t.id, filter);
    };

    auto const render_table_row = [&](st::Table const &t) {
        bool const selected = state.selected_table_id == t.id;
        // Prefer the human-readable name as the primary label.
        // Snake-case IDs are developer-facing — surface them in the
        // tooltip instead.
        char const *label = t.name.empty() ? t.id.c_str() : t.name.c_str();
        ImGui::PushID(t.id.c_str());
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowOverlap)) {
            state.select_table(t.id);
        }
        // Capture Selectable hover state BEFORE drawing the badge — the
        // badge becomes the "last item" once it lands, which would
        // otherwise scope the tooltip to just the tiny S/E letters
        // instead of the whole row.
        bool const row_hovered = ImGui::IsItemHovered();
        // Right-aligned policy badges: S (engine-safety-critical, warm
        // amber) and E (emissions-relevant, muted yellow). Drawn AFTER
        // the Selectable so AllowOverlap is needed; reads "tagged" at a
        // glance while browsing.
        if (t.engine_safety_critical || t.emissions_relevant) {
            char buf[4]{};
            std::size_t bi = 0;
            if (t.engine_safety_critical)
                buf[bi++] = 'S';
            if (t.emissions_relevant)
                buf[bi++] = 'E';
            float const w = ImGui::CalcTextSize(buf).x;
            float const right_x =
                ImGui::GetWindowContentRegionMax().x - w - ImGui::GetStyle().FramePadding.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_x);
            // Theme-aware via the same fg colors the chip palette uses:
            // S (engine-safety) shares chip_fg_warn (amber band), E
            // (emissions) shares chip_fg_caution (yellow band). In light
            // theme these flip to dark amber / dark olive so they stay
            // legible against the pale-blue panel background.
            ImVec4 const color =
                t.engine_safety_critical ? chip_fg_warn() : chip_fg_caution();
            ImGui::TextColored(color, "%s", buf);
        }
        if (row_hovered) {
            ImGui::BeginTooltip();
            if (!t.name.empty()) {
                ImGui::TextUnformatted(t.name.c_str());
                text_subtle("%s", t.id.c_str());
            } else {
                ImGui::TextUnformatted(t.id.c_str());
            }
            ImGui::Separator();
            ImGui::Text("%dD  \xC2\xB7  0x%08zX", t.dimensions, t.address);
            if (!t.category.empty()) {
                text_subtle("category: %s", t.category.c_str());
            }
            if (t.engine_safety_critical) {
                ImGui::TextColored(chip_fg_warn(), "engine safety critical");
            } else if (t.emissions_relevant) {
                ImGui::TextColored(chip_fg_caution(), "emissions-relevant");
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
        if (group_matched == 0)
            continue;

        char header[96];
        if (filter.empty()) {
            std::snprintf(header, sizeof header, "%.*s (%zu)", static_cast<int>(g.name.size()),
                          g.name.data(), g.indices.size());
        } else {
            std::snprintf(header, sizeof header, "%.*s (%zu of %zu)",
                          static_cast<int>(g.name.size()), g.name.data(), group_matched,
                          g.indices.size());
        }

        // When filtering, force the group open so matches are always
        // visible. Otherwise default-open on first run; the user can
        // collapse manually and that state persists via imgui.ini.
        if (!filter.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        ImGuiTreeNodeFlags const tn_flags =
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (ImGui::TreeNodeEx(header, tn_flags)) {
            for (auto idx : g.indices) {
                auto const &t = def.tables()[idx];
                if (!table_matches(t))
                    continue;
                render_table_row(t);
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();
}

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
        // canonical "edit this cell" shortcut.
        if (ImGui::IsKeyPressed(ImGuiKey_F2, /*repeat=*/false)) {
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
        // operations when it's active.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copy_rect_to_clipboard(td, selection.as_rect(), precision);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
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
            ImU32 const bg = heatmap_color(v, stats.min, stats.max);
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
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::snprintf(state.edit_buf, sizeof state.edit_buf, "%.*f", precision, v);
                        state.editing_cell = true;
                        state.editor_just_opened = true;
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
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_sel)) {
                        copy_rect_to_clipboard(td, selection.as_rect(), precision);
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false, has_sel)) {
                        paste_clipboard_at_cursor(state);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset to Source", nullptr, false, has_sel)) {
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
inline ImVec4 chip_fg_accent() {
    return theme_is_light() ? ImVec4(0.32f, 0.18f, 0.55f, 1.0f)
                            : ImVec4(0.92f, 0.84f, 1.00f, 1.0f);
}
inline ImVec4 chip_bg_accent() {
    return theme_is_light() ? ImVec4(0.85f, 0.78f, 0.96f, 0.80f)
                            : ImVec4(0.32f, 0.20f, 0.50f, 0.55f);
}
inline ImVec4 chip_fg_warn() {
    return theme_is_light() ? ImVec4(0.55f, 0.32f, 0.05f, 1.0f)
                            : ImVec4(1.00f, 0.86f, 0.55f, 1.0f);
}
inline ImVec4 chip_bg_warn() {
    return theme_is_light() ? ImVec4(1.00f, 0.90f, 0.65f, 0.78f)
                            : ImVec4(0.42f, 0.30f, 0.08f, 0.60f);
}
inline ImVec4 chip_fg_caution() {
    return theme_is_light() ? ImVec4(0.46f, 0.40f, 0.05f, 1.0f)
                            : chip_fg_caution();
}
inline ImVec4 chip_bg_caution() {
    return theme_is_light() ? ImVec4(1.00f, 0.96f, 0.70f, 0.75f)
                            : ImVec4(0.34f, 0.32f, 0.08f, 0.55f);
}
inline ImVec4 chip_fg_muted() {
    return theme_is_light() ? ImVec4(0.32f, 0.36f, 0.42f, 1.0f)
                            : ImVec4(0.78f, 0.80f, 0.82f, 1.0f);
}
inline ImVec4 chip_bg_muted() {
    return theme_is_light() ? ImVec4(0.82f, 0.85f, 0.90f, 0.75f)
                            : ImVec4(0.22f, 0.24f, 0.28f, 0.55f);
}
inline ImVec4 chip_fg_ok() {
    return theme_is_light() ? ImVec4(0.10f, 0.40f, 0.15f, 1.0f)
                            : ImVec4(0.70f, 0.94f, 0.72f, 1.0f);
}
inline ImVec4 chip_bg_ok() {
    return theme_is_light() ? ImVec4(0.75f, 0.93f, 0.78f, 0.75f)
                            : ImVec4(0.14f, 0.34f, 0.18f, 0.55f);
}
inline ImVec4 chip_fg_danger() {
    return theme_is_light() ? ImVec4(0.58f, 0.10f, 0.10f, 1.0f)
                            : ImVec4(1.00f, 0.75f, 0.72f, 1.0f);
}
inline ImVec4 chip_bg_danger() {
    return theme_is_light() ? ImVec4(1.00f, 0.82f, 0.80f, 0.80f)
                            : ImVec4(0.46f, 0.18f, 0.18f, 0.60f);
}
// Info — neutral / informational accent in the blue band. Distinct
// from chip_accent (brand purple) so info-tier indicators don't read
// as "active selection". Used for non-error/non-warn callouts like
// chunk indicators, "preview" labels, etc.
inline ImVec4 chip_fg_info() {
    return theme_is_light() ? ImVec4(0.10f, 0.32f, 0.62f, 1.0f)
                            : ImVec4(0.55f, 0.82f, 1.00f, 1.0f);
}
inline ImVec4 chip_bg_info() {
    return theme_is_light() ? ImVec4(0.78f, 0.88f, 1.00f, 0.75f)
                            : ImVec4(0.14f, 0.26f, 0.42f, 0.55f);
}

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
    bool const has_recents = !state.recents.empty();
    float const min_pad = avail.y * (has_recents ? 0.10f : 0.22f);
    float const center_bias = (avail.y - 320.0f) * 0.40f;
    float const top_pad = std::max(min_pad, center_bias);
    ImGui::Dummy(ImVec2(0.0f, top_pad));

    text_centered("SubuwuTuner", 2.4f);

    // Thin brand-purple accent rule under the title. Visual weight is
    // small but consistent across themes — uses the same accent the
    // status-bar profile chip and tab-selected overline use, so the
    // first thing the user sees on launch matches the active-element
    // language elsewhere in the app.
    {
        constexpr float kRuleW = 64.0f;
        constexpr float kRuleH = 2.0f;
        center_cursor_x(kRuleW);
        ImVec2 const p = ImGui::GetCursorScreenPos();
        auto *const dl = ImGui::GetWindowDrawList();
        // Read from current_theme() not state.settings.theme — the
        // global is what apply_theme() actually wrote, so this matches
        // the ImGui style colors the surrounding widgets are using.
        auto const [accent, accent_hover, accent_active] = accent_for(current_theme());
        (void)accent_hover;
        (void)accent_active;
        ImU32 const col = ImGui::GetColorU32(accent);
        // Rounded pill, not a hard line — softer against the centered
        // title without bleeding into a "load progress" affordance.
        dl->AddRectFilled(p, ImVec2(p.x + kRuleW, p.y + kRuleH), col, kRuleH * 0.5f);
        ImGui::Dummy(ImVec2(kRuleW, kRuleH + 10.0f));
    }

    text_centered_subtle("Open a Subaru ECU calibration to read, edit, and save.");
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
    ImGui::Dummy(ImVec2(0.0f, kSpaceS));
    text_centered_subtle("Ctrl+O");

    // New-project CTA. Slightly smaller than Open Project so the
    // welcome panel keeps its primary-action / secondary-action
    // hierarchy — opening an existing project is the more common
    // first move.
    ImGui::Dummy(ImVec2(0.0f, kSpaceM));
    constexpr float kSecondaryW = 200.0f;
    center_cursor_x(kSecondaryW);
    if (ImGui::Button("New project…", ImVec2(kSecondaryW, 30.0f))) {
        // Welcome panel only renders when no project is loaded, so the
        // dirty-state guard inside request_action is a no-op here —
        // but routing through it keeps every entry point consistent
        // with the File menu's New Project… item.
        request_action(state, ConfirmAction::NewProject);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pick a source ROM and definition pack, then\n"
                          "create a new .stune project directory.");
    }

    // "Try the demo project" CTA — third tier, only renders when
    // the resolver actually located fixtures/demo.stune at startup
    // (dev tree or install layout that ships fixtures alongside the
    // binary). One click → open the bundled project, no file dialog,
    // no required ROM dump. Highest leverage on a fresh first-run
    // experience; quietly absent when not available so packaged
    // installs without the demo don't show a broken button.
    if (state.demo_project_path.has_value()) {
        ImGui::Dummy(ImVec2(0.0f, kSpaceS));
        constexpr float kTertiaryW = 200.0f;
        center_cursor_x(kTertiaryW);
        if (ImGui::Button("Try the demo project", ImVec2(kTertiaryW, 28.0f))) {
            request_action(state, ConfirmAction::OpenRecent,
                           *state.demo_project_path);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open the bundled fixtures/demo.stune project.\n%s",
                              state.demo_project_path->string().c_str());
        }
    }

    // First-run-only pack hint. Answers "what do I need to start?"
    // without forcing the user to open the New Project modal first
    // to find out. Hidden once the user has any recents — they've
    // clearly figured out the flow by then.
    if (!has_recents) {
        ImGui::Dummy(ImVec2(0.0f, 18.0f));
        text_centered_subtle("First time? You'll need an ECU ROM dump + a definition pack.");
        if (!state.demo_project_path.has_value()) {
            // Only mention the demo as a follow-up hint when the
            // button itself isn't already on screen — otherwise the
            // text duplicates the CTA right above it.
            text_centered_subtle("The repo ships fixtures/demo.stune/ as a ready-to-open project.");
        } else {
            text_centered_subtle("Or click \"Try the demo project\" above to explore the UI now.");
        }
    }

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
            ImVec2 const p = ImGui::GetCursorScreenPos();
            auto *const dl = ImGui::GetWindowDrawList();
            ImU32 const col = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + kRowW, p.y + 2.0f), col);
            ImGui::Dummy(ImVec2(kRowW, 4.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));

        // Snapshot indices to act on — modifying recents inside the
        // iteration (via try_open_project) would invalidate iterators.
        std::optional<std::size_t> clicked_idx;
        for (std::size_t i = 0; i < state.recents.size(); ++i) {
            auto const &e = state.recents[i];
            auto const basename =
                e.path.filename().empty() ? e.path.string() : e.path.filename().string();
            std::error_code ec;
            bool const exists = std::filesystem::exists(e.path, ec);

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
                    ImGui::SetTooltip("%s\nOpened %s", e.path.string().c_str(),
                                      e.opened_at.c_str());
                } else {
                    ImGui::SetTooltip("%s\n\nPath no longer exists — the project may "
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
            text_subtle("%s", subtitle.c_str());
            ImGui::Dummy(ImVec2(0.0f, kSpaceS));
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
        text_centered_subtle(state.status_msg.c_str());
    }

    // What's new — short list of recent additions, refreshed when a
    // feature lands. Kept brief on purpose: the welcome panel reads
    // best when it stays uncluttered. Tucked beneath recents so first-
    // run users see CTAs first.
    static constexpr std::array<char const *, 4> kWhatsNew = {
        "SSM-A8 RAM polling + offline correlator — subuwutuner-cli ssm-a8-poll",
        "CAN reverse-engineering toolkit (.asc / .dbc, BaselineModel + decoder)",
        "Custom features designer — Tools \xE2\x86\x92 Feature graph (SH-2A codegen)",
        "Autotune kernels (MAF + knock pull) with project-integration — Edit menu",
    };
    {
        ImGui::Dummy(ImVec2(0.0f, has_recents ? 22.0f : 28.0f));
        constexpr float kRowW = 480.0f;
        center_cursor_x(kRowW);
        ImGui::BeginGroup();
        ImGui::TextUnformatted("What's new");
        {
            ImVec2 const p = ImGui::GetCursorScreenPos();
            auto *const dl = ImGui::GetWindowDrawList();
            ImU32 const col = ImGui::GetColorU32(ImGuiCol_Separator);
            dl->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + kRowW, p.y + 2.0f), col);
            ImGui::Dummy(ImVec2(kRowW, 4.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, kSpaceXS));
        for (auto const *line : kWhatsNew) {
            text_subtle("\xE2\x80\xA2  %s", line);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }
        ImGui::EndGroup();
    }

    // Footer: version + a small Help shortcut. Subtle enough to not
    // compete with the CTAs above, present enough to be discoverable.
    ImGui::Dummy(ImVec2(0.0f, kSpaceXL));
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SubuwuTuner %.*s",
                      static_cast<int>(st::Version::string().size()), st::Version::string().data());
        float const text_w = ImGui::CalcTextSize(buf).x + ImGui::CalcTextSize(" \xC2\xB7 ").x +
                             ImGui::CalcTextSize("Keyboard shortcuts").x;
        center_cursor_x(text_w);
        text_subtle("%s", buf);
        ImGui::SameLine();
        text_subtle(" \xC2\xB7 ");
        ImGui::SameLine();
        // Render as a button styled to look like a link — TextDisabled
        // color, no frame. Reduces visual weight while keeping it
        // clickable and tab-reachable.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Button("Keyboard shortcuts")) {
            state.show_shortcuts_modal = true;
        }
        // It looks like text (no frame, no border) — signal that it
        // actually clicks by switching to the hand cursor on hover.
        // Same affordance the status-bar profile chip uses.
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }
}

// Right-rail quick-stats panel for the currently-selected table.
// Min/max/mean/stddev across all cells, count of edited cells (diff
// against source), and an ImPlot histogram. Read-only; updates each
// frame the table data is loaded.
void render_stats_panel(AppState &state) {
    if (!state.show_stats_panel) {
        return;
    }
    if (!ImGui::Begin("Stats", &state.show_stats_panel)) {
        ImGui::End();
        return;
    }
    if (!state.project.has_value()) {
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to see table statistics here.");
        ImGui::End();
        return;
    }
    if (state.selected_table_id.empty() || !state.current_table_data.has_value()) {
        render_empty_state(
            "Pick a table",
            "Select one in the Tables panel — min / mean / max and a value histogram show up here.");
        ImGui::End();
        return;
    }
    auto const *table = state.project->definition().find_table(state.selected_table_id);
    if (table == nullptr) {
        render_empty_state(
            "Table not found",
            "The selected table id isn't in the loaded pack. Pick another from the Tables panel.");
        ImGui::End();
        return;
    }
    auto const &td = *state.current_table_data;

    // Flatten cells for stats + histogram. Skip dim=0 scalar — stats of
    // one value are useless.
    std::vector<float> cells;
    cells.reserve(td.values.size() * (td.values.empty() ? 0 : td.values[0].size()));
    for (auto const &row : td.values) {
        for (auto v : row)
            cells.push_back(static_cast<float>(v));
    }
    if (cells.empty() || (cells.size() == 1 && table->dimensions == 0)) {
        text_subtle("Scalar table — stats N/A.");
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
        if (v < min)
            min = v;
        if (v > max)
            max = v;
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
    auto const source_td =
        state.project->definition().read_table_values(state.project->source_rom(), *table);
    if (source_td.has_value() && source_td->values.size() == td.values.size()) {
        for (std::size_t r = 0; r < td.values.size(); ++r) {
            if (source_td->values[r].size() != td.values[r].size())
                continue;
            for (std::size_t c = 0; c < td.values[r].size(); ++c) {
                if (td.values[r][c] != source_td->values[r][c])
                    ++edited;
            }
        }
    }

    auto const *scal = state.project->definition().find_scaling(table->scaling);
    auto const unit = (scal != nullptr) ? scal->unit : std::string{};
    auto const prec = (scal != nullptr) ? scal->precision : 2;

    ImGui::Text("%s", table->id.c_str());
    if (!unit.empty())
        text_subtle("unit: %s", unit.c_str());
    ImGui::Separator();

    auto stat_row = [&](char const *label, double v) {
        ImGui::Text("%-7s %.*f%s%s", label, prec, v, unit.empty() ? "" : " ", unit.c_str());
    };
    stat_row("min", static_cast<double>(min));
    stat_row("max", static_cast<double>(max));
    stat_row("mean", mean);
    stat_row("stddev", stddev);
    ImGui::Text("cells:  %zu", cells.size());
    ImGui::Text("edited: %zu", edited);

    ImGui::Separator();
    text_subtle("histogram");
    if (ImPlot::BeginPlot("##stats_hist", ImVec2(-FLT_MIN, 160.0f),
                          ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend | ImPlotFlags_NoTitle |
                              ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus,
                          ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoMenus);
        // Pick a reasonable bin count; ImPlot::PlotHistogram chooses
        // smart defaults when bins=ImPlotBin_Sturges.
        ImPlot::PlotHistogram("##bins", cells.data(), static_cast<int>(cells.size()),
                              ImPlotBin_Sturges);
        ImPlot::EndPlot();
    }
    ImGui::End();
}

// "DTCs" panel — surface the diagnostic trouble codes declared by the pack
// with their current enable/disable state, and let the user toggle them.
// Per-cylinder knock dashboard. Mirrors the CLI's `knock-snapshot`
// subcommand: load a CSV, map columns to mapping fields, compute a
// snapshot via st::log::knock::snapshot_from_csv, render strip charts.
// State lives in AppState (knock_* fields) so the panel survives across
// frames without re-loading the CSV per refresh.
void render_knock_dashboard_panel(AppState &state) {
    if (!state.show_knock_dashboard_panel) {
        return;
    }
    if (!ImGui::Begin("Knock Dashboard (Preview)", &state.show_knock_dashboard_panel)) {
        ImGui::End();
        return;
    }

    text_subtle("Per-cylinder knock from a CSV datalog. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    // ---- Log + Browse -------------------------------------------------
    ImGui::Text("Log:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##knock_log_path", state.knock_log_path, sizeof state.knock_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##knock_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.knock_log_path, sizeof state.knock_log_path, "%s", out_path.get());
            state.knock_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.knock_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.knock_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.knock_load_error.c_str());
        ImGui::PopStyleColor();
    }

    // ---- Column mapping ----------------------------------------------
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderInt("Cylinders", &state.knock_cylinder_count, 1, 6);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("RPM column", state.knock_rpm_col, sizeof state.knock_rpm_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Load column", state.knock_load_col, sizeof state.knock_load_col);
        for (int c = 0; c < state.knock_cylinder_count; ++c) {
            ImGui::PushID(c);
            char label_flkc[32];
            std::snprintf(label_flkc, sizeof label_flkc, "FLKC cyl %d", c + 1);
            char label_fbkc[32];
            std::snprintf(label_fbkc, sizeof label_fbkc, "FBKC cyl %d", c + 1);
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText(label_flkc, state.knock_flkc_cols[c], sizeof state.knock_flkc_cols[c]);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText(label_fbkc, state.knock_fbkc_cols[c], sizeof state.knock_fbkc_cols[c]);
            ImGui::PopID();
        }
    }

    // ---- Window config -----------------------------------------------
    if (ImGui::CollapsingHeader("Window")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Window seconds", &state.knock_window_seconds, 0.5f, 5.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Sample rate Hz", &state.knock_sample_rate_hz, 1.0f, 5.0f, "%.1f");
        ImGui::Checkbox("Load gate", &state.knock_gate_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("min RPM", &state.knock_min_rpm, 100.0f, 500.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("min load", &state.knock_min_load, 0.1f, 0.5f, "%.2f");
    }

    ImGui::Spacing();
    // ---- Compute button ----------------------------------------------
    if (ImGui::Button("Compute snapshot")) {
        state.knock_load_error.clear();
        if (state.knock_log_path[0] == '\0') {
            state.knock_load_error = "Pick a CSV log first.";
        } else {
            st::log::knock::PidMapping mapping;
            mapping.cylinder_count = static_cast<std::uint8_t>(state.knock_cylinder_count);

            // Load + parse header to resolve column names → indices.
            std::ifstream f{state.knock_log_path};
            if (!f) {
                state.knock_load_error = std::string{"Cannot open '"} + state.knock_log_path + "'";
            } else {
                std::string header_line;
                bool got = false;
                while (std::getline(f, header_line)) {
                    while (!header_line.empty() && header_line.back() == '\r') {
                        header_line.pop_back();
                    }
                    if (header_line.empty() || header_line.front() == '#')
                        continue;
                    got = true;
                    break;
                }
                if (!got) {
                    state.knock_load_error = "CSV has no header.";
                } else {
                    std::vector<std::string> header_cols;
                    std::size_t start = 0;
                    for (std::size_t i = 0; i <= header_line.size(); ++i) {
                        if (i == header_line.size() || header_line[i] == ',') {
                            std::size_t a = start;
                            std::size_t b = i;
                            while (a < b &&
                                   std::isspace(static_cast<unsigned char>(header_line[a])))
                                ++a;
                            while (b > a &&
                                   std::isspace(static_cast<unsigned char>(header_line[b - 1])))
                                --b;
                            header_cols.emplace_back(header_line.substr(a, b - a));
                            start = i + 1;
                        }
                    }
                    auto const resolve = [&](char const *name) -> std::size_t {
                        if (name == nullptr || name[0] == '\0') {
                            return st::log::knock::kNoPid;
                        }
                        std::string want{name};
                        for (auto &cc : want) {
                            cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                        }
                        for (std::size_t i = 0; i < header_cols.size(); ++i) {
                            std::string have = header_cols[i];
                            for (auto &cc : have) {
                                cc =
                                    static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            }
                            if (have == want)
                                return i;
                        }
                        return st::log::knock::kNoPid;
                    };
                    mapping.rpm_idx = resolve(state.knock_rpm_col);
                    mapping.load_idx = resolve(state.knock_load_col);
                    bool any_unresolved = false;
                    for (int c = 0; c < state.knock_cylinder_count; ++c) {
                        auto const cs = static_cast<std::size_t>(c);
                        std::size_t const f_idx = resolve(state.knock_flkc_cols[c]);
                        std::size_t const b_idx = resolve(state.knock_fbkc_cols[c]);
                        mapping.fine_knock_learn[cs] = f_idx;
                        mapping.feedback_knock[cs] = b_idx;
                        if (state.knock_flkc_cols[c][0] != '\0' && f_idx == st::log::knock::kNoPid)
                            any_unresolved = true;
                        if (state.knock_fbkc_cols[c][0] != '\0' && b_idx == st::log::knock::kNoPid)
                            any_unresolved = true;
                    }
                    if (any_unresolved) {
                        state.knock_load_error =
                            "One or more column names didn't match the CSV "
                            "header. Mappings without a match will be ignored.";
                    }
                    st::log::knock::WindowConfig cfg;
                    cfg.window_seconds = static_cast<double>(state.knock_window_seconds);
                    cfg.sample_rate_hz = static_cast<double>(state.knock_sample_rate_hz);
                    cfg.min_rpm = static_cast<double>(state.knock_min_rpm);
                    cfg.min_load = static_cast<double>(state.knock_min_load);
                    cfg.require_load_gate = state.knock_gate_enabled;
                    auto const r =
                        st::log::knock::snapshot_from_csv(state.knock_log_path, mapping, cfg);
                    if (!r.has_value()) {
                        state.knock_load_error = r.error().to_string();
                        state.knock_snapshot.reset();
                    } else {
                        state.knock_snapshot = *r;
                        state.knock_compute_msg =
                            "Considered " + std::to_string(r->samples_considered) +
                            " samples (gated out " + std::to_string(r->samples_gated_out) + ").";
                    }
                }
            }
        }
    }
    if (!state.knock_compute_msg.empty() && state.knock_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.knock_compute_msg.c_str());
    }

    // ---- Grid of per-cyl strip charts --------------------------------
    if (state.knock_snapshot.has_value()) {
        auto const &snap = *state.knock_snapshot;
        ImGui::Separator();
        int const cyls = static_cast<int>(snap.cylinder_count);
        int const cols = (cyls <= 4) ? 2 : 3;
        ImVec2 const avail = ImGui::GetContentRegionAvail();
        float const cell_w =
            (avail.x - static_cast<float>(cols - 1) * 8.0f) / static_cast<float>(cols);
        float const cell_h = 180.0f;
        for (int c = 0; c < cyls; ++c) {
            auto const &p = snap.per_cyl[static_cast<std::size_t>(c)];
            bool const no_data = p.strip_flkc.empty() && p.strip_fbkc.empty();
            ImGui::BeginGroup();
            ImGui::Text("Cyl %d", c + 1);
            if (no_data) {
                ImGui::Dummy(ImVec2(cell_w, cell_h));
                ImGui::SameLine(8.0f);
                text_subtle("(no per-cylinder signal in mapping)");
            } else {
                ImGui::Text("FLKC cur: %+.2f  mean: %+.2f  min: %+.2f  "
                            "FBKC cur: %+.2f  events: %u  dMean: %+.2f",
                            p.current_flkc, p.mean_flkc_window, p.min_flkc_window, p.current_fbkc,
                            p.event_count_window, p.delta_from_cyl_mean);
                char plot_id[32];
                std::snprintf(plot_id, sizeof plot_id, "##knock_cyl_%d", c);
                if (ImPlot::BeginPlot(plot_id, ImVec2(cell_w, cell_h),
                                      ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                                          ImPlotFlags_NoMouseText)) {
                    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit,
                                      ImPlotAxisFlags_AutoFit);
                    if (!p.strip_flkc.empty()) {
                        ImPlot::PlotLine("FLKC", p.strip_flkc.data(),
                                         static_cast<int>(p.strip_flkc.size()));
                    }
                    if (!p.strip_fbkc.empty()) {
                        ImPlot::PlotLine("FBKC", p.strip_fbkc.data(),
                                         static_cast<int>(p.strip_fbkc.size()));
                    }
                    ImPlot::EndPlot();
                }
            }
            ImGui::EndGroup();
            // Next column / row
            if ((c + 1) % cols != 0 && c + 1 < cyls) {
                ImGui::SameLine();
            }
        }
    } else if (state.knock_load_error.empty()) {
        text_subtle("No snapshot yet — pick a log and click \"Compute snapshot\".");
    }

    ImGui::End();
}

// Adaptive-learning history visualizer. Same shape as the knock panel:
// load CSV, map columns, compute a snapshot via st::log::adaptive,
// render time-series charts. Domain typing lets us share zero code with
// the knock panel without anything feeling forced — they're different
// shapes of data.
void render_adaptive_history_panel(AppState &state) {
    if (!state.show_adaptive_history_panel) {
        return;
    }
    if (!ImGui::Begin("Adaptive History (Preview)", &state.show_adaptive_history_panel)) {
        ImGui::End();
        return;
    }

    text_subtle("Long-cycle LTFT / DAM / idle-adapt drift. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    ImGui::Text("Log:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##ah_log_path", state.ah_log_path, sizeof state.ah_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##ah_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.ah_log_path, sizeof state.ah_log_path, "%s", out_path.get());
            state.ah_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.ah_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.ah_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.ah_load_error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Timestamp col", state.ah_ts_col, sizeof state.ah_ts_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("LTFT col", state.ah_ltft_col, sizeof state.ah_ltft_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("DAM col", state.ah_dam_col, sizeof state.ah_dam_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("IdleAdapt col", state.ah_iac_col, sizeof state.ah_iac_col);
        text_subtle("Leave a column name blank to skip that signal.");
    }

    if (ImGui::CollapsingHeader("Bucketing")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Bucket seconds", &state.ah_bucket_seconds, 60.0f, 3600.0f, "%.1f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("86400 = 1 day  ·  3600 = 1 hour  ·  0 = no bucketing");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        char const *ts_units[] = {"seconds", "millis", "micros", "rows"};
        ImGui::Combo("Timestamp unit", &state.ah_ts_unit, ts_units, 4);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Min samples / bucket", &state.ah_min_samples_per_bucket, 1, 5);
        if (state.ah_min_samples_per_bucket < 0)
            state.ah_min_samples_per_bucket = 0;
    }

    ImGui::Spacing();
    if (ImGui::Button("Compute snapshot##ah")) {
        state.ah_load_error.clear();
        if (state.ah_log_path[0] == '\0') {
            state.ah_load_error = "Pick a CSV log first.";
        } else {
            // Read header to resolve column names → indices.
            std::ifstream f{state.ah_log_path};
            if (!f) {
                state.ah_load_error = std::string{"Cannot open '"} + state.ah_log_path + "'";
            } else {
                std::string header_line;
                bool got = false;
                while (std::getline(f, header_line)) {
                    while (!header_line.empty() && header_line.back() == '\r') {
                        header_line.pop_back();
                    }
                    if (header_line.empty() || header_line.front() == '#')
                        continue;
                    got = true;
                    break;
                }
                if (!got) {
                    state.ah_load_error = "CSV has no header.";
                } else {
                    std::vector<std::string> header_cols;
                    std::size_t start = 0;
                    for (std::size_t i = 0; i <= header_line.size(); ++i) {
                        if (i == header_line.size() || header_line[i] == ',') {
                            std::size_t a = start;
                            std::size_t b = i;
                            while (a < b &&
                                   std::isspace(static_cast<unsigned char>(header_line[a])))
                                ++a;
                            while (b > a &&
                                   std::isspace(static_cast<unsigned char>(header_line[b - 1])))
                                --b;
                            header_cols.emplace_back(header_line.substr(a, b - a));
                            start = i + 1;
                        }
                    }
                    auto const resolve = [&](char const *name) -> std::size_t {
                        if (name == nullptr || name[0] == '\0') {
                            return st::log::adaptive::kNoColumn;
                        }
                        std::string want{name};
                        for (auto &cc : want) {
                            cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                        }
                        for (std::size_t i = 0; i < header_cols.size(); ++i) {
                            std::string have = header_cols[i];
                            for (auto &cc : have) {
                                cc =
                                    static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            }
                            if (have == want)
                                return i;
                        }
                        return st::log::adaptive::kNoColumn;
                    };
                    st::log::adaptive::ColumnMapping mapping;
                    mapping.timestamp_idx = resolve(state.ah_ts_col);
                    mapping
                        .signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Ltft)] =
                        resolve(state.ah_ltft_col);
                    mapping
                        .signal_idx[static_cast<std::size_t>(st::log::adaptive::SignalKind::Dam)] =
                        resolve(state.ah_dam_col);
                    mapping.signal_idx[static_cast<std::size_t>(
                        st::log::adaptive::SignalKind::IdleAdapt)] = resolve(state.ah_iac_col);

                    st::log::adaptive::BucketConfig cfg;
                    cfg.bucket_seconds = static_cast<double>(state.ah_bucket_seconds);
                    cfg.min_samples_per_bucket =
                        static_cast<std::uint32_t>(state.ah_min_samples_per_bucket);
                    switch (state.ah_ts_unit) {
                    case 1:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMillis;
                        break;
                    case 2:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixMicros;
                        break;
                    case 3:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::RowIndex;
                        break;
                    default:
                        cfg.timestamp_unit = st::log::adaptive::TimestampUnit::UnixSeconds;
                        break;
                    }
                    auto const r =
                        st::log::adaptive::snapshot_from_csv(state.ah_log_path, mapping, cfg);
                    if (!r.has_value()) {
                        state.ah_load_error = r.error().to_string();
                        state.ah_snapshot.reset();
                    } else {
                        state.ah_snapshot = *r;
                        state.ah_compute_msg =
                            std::to_string(r->total_samples) + " samples across " +
                            std::to_string(r->time_span_seconds / 86400.0).substr(0, 5) + " days.";
                    }
                }
            }
        }
    }
    if (!state.ah_compute_msg.empty() && state.ah_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.ah_compute_msg.c_str());
    }

    if (state.ah_snapshot.has_value()) {
        auto const &snap = *state.ah_snapshot;
        ImGui::Separator();

        char const *signal_names[3] = {"LTFT", "DAM", "IdleAdapt"};
        // Summary table
        if (ImGui::BeginTable("##ah_summary", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Signal");
            ImGui::TableSetupColumn("Buckets");
            ImGui::TableSetupColumn("Mean");
            ImGui::TableSetupColumn("Min");
            ImGui::TableSetupColumn("Max");
            ImGui::TableSetupColumn("Drift / day");
            ImGui::TableHeadersRow();
            for (std::size_t k = 0; k < st::log::adaptive::kSignalCount; ++k) {
                auto const &ser = snap.series[k];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(signal_names[k]);
                if (!ser.has_data) {
                    for (int c = 0; c < 5; ++c) {
                        ImGui::TableNextColumn();
                        text_subtle("—");
                    }
                    continue;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", ser.points.size());
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.overall_mean);
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.overall_min);
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.overall_max);
                ImGui::TableNextColumn();
                ImGui::Text("%+.4f", ser.drift_per_second * 86400.0);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();

        // One time-series plot per signal that has data.
        ImVec2 const avail = ImGui::GetContentRegionAvail();
        float const plot_w = avail.x;
        float const plot_h = 140.0f;
        for (std::size_t k = 0; k < st::log::adaptive::kSignalCount; ++k) {
            auto const &ser = snap.series[k];
            if (!ser.has_data || ser.points.empty())
                continue;
            ImGui::Text("%s", signal_names[k]);
            // Build parallel x/y arrays (ImPlot doesn't take struct
            // arrays without explicit stride/offset; this is cleaner).
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(ser.points.size());
            ys.reserve(ser.points.size());
            for (auto const &p : ser.points) {
                xs.push_back(p.bucket_center);
                ys.push_back(p.mean);
            }
            char plot_id[32];
            std::snprintf(plot_id, sizeof plot_id, "##ah_plot_%zu", k);
            if (ImPlot::BeginPlot(plot_id, ImVec2(plot_w, plot_h),
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoMenus |
                                      ImPlotFlags_NoMouseText)) {
                ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit,
                                  ImPlotAxisFlags_AutoFit);
                ImPlot::PlotLine(signal_names[k], xs.data(), ys.data(),
                                 static_cast<int>(xs.size()));
                ImPlot::EndPlot();
            }
            ImGui::Spacing();
        }
    } else if (state.ah_load_error.empty()) {
        text_subtle("No snapshot yet — pick a log and click \"Compute snapshot\".");
    }

    ImGui::End();
}

// Cold-start analysis panel. Same shape as knock/adaptive: load CSV,
// map columns, compute snapshot, render results. The methodology piece
// (target lambda curve) is a free-form text buffer "ect:lambda,..." for
// v1.x; a richer curve editor is roadmap.
void render_coldstart_panel(AppState &state) {
    if (!state.show_coldstart_panel) {
        return;
    }
    if (!ImGui::Begin("Cold-Start Analysis (Preview)", &state.show_coldstart_panel)) {
        ImGui::End();
        return;
    }

    text_subtle("Phase-classify + ECT-bin a cold-start datalog. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    ImGui::Text("Log:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##cs_log_path", state.cs_log_path, sizeof state.cs_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##cs_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.cs_log_path, sizeof state.cs_log_path, "%s", out_path.get());
            state.cs_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.cs_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.cs_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.cs_load_error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Timestamp col", state.cs_ts_col, sizeof state.cs_ts_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("ECT col", state.cs_ect_col, sizeof state.cs_ect_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("IAT col", state.cs_iat_col, sizeof state.cs_iat_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("RPM col", state.cs_rpm_col, sizeof state.cs_rpm_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Obs λ col", state.cs_obs_col, sizeof state.cs_obs_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Cmd λ col", state.cs_cmd_col, sizeof state.cs_cmd_col);
        text_subtle("ECT and RPM are mandatory. Others are optional.");
    }

    if (ImGui::CollapsingHeader("Window")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Cold threshold °C", &state.cs_cold_threshold_c, 1.0f, 5.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("ECT bin width °C", &state.cs_ect_bin_width_c, 0.5f, 2.5f, "%.1f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Min samples/bin", &state.cs_min_samples_per_bin, 1, 5);
        if (state.cs_min_samples_per_bin < 0)
            state.cs_min_samples_per_bin = 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        char const *ts_units[] = {"seconds", "millis", "micros", "rows"};
        ImGui::Combo("Timestamp unit", &state.cs_ts_unit, ts_units, 4);
    }

    if (ImGui::CollapsingHeader("Target lambda curve", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##cs_target_curve", state.cs_target_curve, sizeof state.cs_target_curve);
        text_subtle("Format: ect:lambda,ect:lambda,...  (linear interp).");
    }

    ImGui::Spacing();
    if (ImGui::Button("Compute snapshot##cs")) {
        state.cs_load_error.clear();
        state.cs_target_curve_parsed.clear();
        // Parse target curve.
        bool curve_ok = true;
        {
            std::string buf{state.cs_target_curve};
            std::size_t start = 0;
            for (std::size_t i = 0; i <= buf.size(); ++i) {
                if (i == buf.size() || buf[i] == ',') {
                    std::string_view pair{buf.data() + start, i - start};
                    while (!pair.empty() && std::isspace(static_cast<unsigned char>(pair.front())))
                        pair.remove_prefix(1);
                    while (!pair.empty() && std::isspace(static_cast<unsigned char>(pair.back())))
                        pair.remove_suffix(1);
                    if (!pair.empty()) {
                        auto const colon = pair.find(':');
                        if (colon == std::string_view::npos) {
                            state.cs_load_error =
                                "Target curve entry '" + std::string{pair} + "' must be ect:lambda";
                            curve_ok = false;
                            break;
                        }
                        try {
                            double const ect = std::stod(std::string{pair.substr(0, colon)});
                            double const lam = std::stod(std::string{pair.substr(colon + 1)});
                            state.cs_target_curve_parsed.emplace_back(ect, lam);
                        } catch (...) {
                            state.cs_load_error = "Target curve entry '" + std::string{pair} +
                                                  "' has non-numeric component";
                            curve_ok = false;
                            break;
                        }
                    }
                    start = i + 1;
                }
            }
            std::sort(state.cs_target_curve_parsed.begin(), state.cs_target_curve_parsed.end(),
                      [](auto const &a, auto const &b) { return a.first < b.first; });
        }
        if (!curve_ok) {
            // Error already set; skip the rest.
        } else if (state.cs_log_path[0] == '\0') {
            state.cs_load_error = "Pick a CSV log first.";
        } else {
            std::ifstream f{state.cs_log_path};
            if (!f) {
                state.cs_load_error = std::string{"Cannot open '"} + state.cs_log_path + "'";
            } else {
                std::string header_line;
                bool got = false;
                while (std::getline(f, header_line)) {
                    while (!header_line.empty() && header_line.back() == '\r') {
                        header_line.pop_back();
                    }
                    if (header_line.empty() || header_line.front() == '#')
                        continue;
                    got = true;
                    break;
                }
                if (!got) {
                    state.cs_load_error = "CSV has no header.";
                } else {
                    std::vector<std::string> header_cols;
                    std::size_t start = 0;
                    for (std::size_t i = 0; i <= header_line.size(); ++i) {
                        if (i == header_line.size() || header_line[i] == ',') {
                            std::size_t a = start;
                            std::size_t b = i;
                            while (a < b &&
                                   std::isspace(static_cast<unsigned char>(header_line[a])))
                                ++a;
                            while (b > a &&
                                   std::isspace(static_cast<unsigned char>(header_line[b - 1])))
                                --b;
                            header_cols.emplace_back(header_line.substr(a, b - a));
                            start = i + 1;
                        }
                    }
                    auto const resolve = [&](char const *name) -> std::size_t {
                        if (name == nullptr || name[0] == '\0') {
                            return st::log::coldstart::kNoColumn;
                        }
                        std::string want{name};
                        for (auto &cc : want) {
                            cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                        }
                        for (std::size_t i = 0; i < header_cols.size(); ++i) {
                            std::string have = header_cols[i];
                            for (auto &cc : have) {
                                cc =
                                    static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            }
                            if (have == want)
                                return i;
                        }
                        return st::log::coldstart::kNoColumn;
                    };
                    st::log::coldstart::PidMapping mapping;
                    mapping.timestamp_idx = resolve(state.cs_ts_col);
                    mapping.ect_idx = resolve(state.cs_ect_col);
                    mapping.iat_idx = resolve(state.cs_iat_col);
                    mapping.rpm_idx = resolve(state.cs_rpm_col);
                    mapping.observed_lambda_idx = resolve(state.cs_obs_col);
                    mapping.commanded_lambda_idx = resolve(state.cs_cmd_col);

                    st::log::coldstart::WindowConfig cfg;
                    cfg.cold_threshold_c = static_cast<double>(state.cs_cold_threshold_c);
                    cfg.ect_bin_width_c = static_cast<double>(state.cs_ect_bin_width_c);
                    cfg.min_samples_per_bin =
                        static_cast<std::uint32_t>(state.cs_min_samples_per_bin);
                    switch (state.cs_ts_unit) {
                    case 1:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixMillis;
                        break;
                    case 2:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixMicros;
                        break;
                    case 3:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::RowIndex;
                        break;
                    default:
                        cfg.timestamp_unit = st::log::coldstart::TimestampUnit::UnixSeconds;
                        break;
                    }

                    st::log::coldstart::Methodology methodology;
                    methodology.target_lambda_vs_ect.points = state.cs_target_curve_parsed;

                    auto const r = st::log::coldstart::snapshot_from_csv(state.cs_log_path, mapping,
                                                                         cfg, methodology);
                    if (!r.has_value()) {
                        state.cs_load_error = r.error().to_string();
                        state.cs_snapshot.reset();
                    } else {
                        state.cs_snapshot = *r;
                        state.cs_compute_msg =
                            std::to_string(r->samples_considered) + " samples; mean lambda dev " +
                            (std::to_string(r->mean_lambda_deviation).substr(0, 6));
                    }
                }
            }
        }
    }
    if (!state.cs_compute_msg.empty() && state.cs_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.cs_compute_msg.c_str());
    }

    if (state.cs_snapshot.has_value()) {
        auto const &snap = *state.cs_snapshot;
        ImGui::Separator();
        char const *phase_names[6] = {"PreCrank", "Cranking", "InitialFiring",
                                      "HighIdle", "Warmup",   "ClosedLoop"};

        ImGui::Text("ECT span: %+.1f°C .. %+.1f°C  ·  considered %llu, gated %llu",
                    snap.coldest_ect_c, snap.warmest_ect_c,
                    static_cast<unsigned long long>(snap.samples_considered),
                    static_cast<unsigned long long>(snap.samples_gated_out));
        ImGui::Spacing();

        // Phase summary table
        if (ImGui::BeginTable("##cs_phase", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Phase");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("Seconds");
            ImGui::TableHeadersRow();
            for (std::size_t p = 0; p < st::log::coldstart::kPhaseCount; ++p) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(phase_names[p]);
                ImGui::TableNextColumn();
                ImGui::Text("%u", static_cast<unsigned>(snap.phase_counts[p]));
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", snap.phase_seconds[p]);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();

        // ECT bins + lambda chart
        if (!snap.ect_bins.empty()) {
            ImGui::Text("ECT bins (observed lambda vs target):");
            std::vector<double> bin_ect, bin_obs, bin_target;
            bin_ect.reserve(snap.ect_bins.size());
            bin_obs.reserve(snap.ect_bins.size());
            bin_target.reserve(snap.ect_bins.size());
            for (auto const &b : snap.ect_bins) {
                bin_ect.push_back(b.ect_center_c);
                bin_obs.push_back(b.observed_lambda_mean);
                bin_target.push_back(b.observed_lambda_mean - b.deviation_from_target);
            }
            ImVec2 const avail = ImGui::GetContentRegionAvail();
            if (ImPlot::BeginPlot("##cs_lambda_plot", ImVec2(avail.x, 200.0f),
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoMenus)) {
                ImPlot::SetupAxes("ECT (°C)", "lambda", ImPlotAxisFlags_AutoFit,
                                  ImPlotAxisFlags_AutoFit);
                ImPlot::PlotLine("observed", bin_ect.data(), bin_obs.data(),
                                 static_cast<int>(bin_ect.size()));
                if (!state.cs_target_curve_parsed.empty()) {
                    ImPlot::PlotLine("target", bin_ect.data(), bin_target.data(),
                                     static_cast<int>(bin_ect.size()));
                }
                ImPlot::EndPlot();
            }
            ImGui::Spacing();

            if (ImGui::BeginTable("##cs_bins", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 200.0f))) {
                ImGui::TableSetupColumn("ECT °C");
                ImGui::TableSetupColumn("n");
                ImGui::TableSetupColumn("obs mean");
                ImGui::TableSetupColumn("obs min");
                ImGui::TableSetupColumn("obs max");
                ImGui::TableSetupColumn("dev");
                ImGui::TableHeadersRow();
                for (auto const &b : snap.ect_bins) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.1f", b.ect_center_c);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", static_cast<unsigned>(b.count));
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.observed_lambda_mean);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.observed_lambda_min);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.observed_lambda_max);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.4f", b.deviation_from_target);
                }
                ImGui::EndTable();
            }
        } else {
            text_subtle("No ECT bins — log has no cold-regime samples with "
                        "observed-lambda data above min_samples_per_bin.");
        }
    } else if (state.cs_load_error.empty()) {
        text_subtle("No snapshot yet — pick a log and click \"Compute snapshot\".");
    }

    ImGui::End();
}

// EBCS PID assistant panel. Detect tip-in events, characterize each step
// response, produce heuristic gain-adjustment suggestions. Same shape
// as the other §11 panels.
void render_ebcs_panel(AppState &state) {
    if (!state.show_ebcs_panel) {
        return;
    }
    if (!ImGui::Begin("EBCS PID Assistant (Preview)", &state.show_ebcs_panel)) {
        ImGui::End();
        return;
    }

    text_subtle("Detect tip-in events; suggest Kp / Ki / Kd direction. "
                "Output is advisory — verify on a dyno. "
                "See docs/05-improvements.md §11.");
    ImGui::Separator();

    ImGui::Text("Log:");
    ImGui::SetNextItemWidth(-120.0f);
    ImGui::InputText("##ebcs_log_path", state.ebcs_log_path, sizeof state.ebcs_log_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##ebcs_log")) {
        nfdu8filteritem_t filters[1] = {{"CSV", "csv"}};
        NFD::UniquePathU8 out_path;
        nfdresult_t const r = NFD::OpenDialog(out_path, filters, 1);
        if (r == NFD_OKAY) {
            std::snprintf(state.ebcs_log_path, sizeof state.ebcs_log_path, "%s", out_path.get());
            state.ebcs_load_error.clear();
        } else if (r == NFD_ERROR) {
            state.ebcs_load_error = std::string{"Open dialog error: "} + NFD::GetError();
        }
    }
    if (!state.ebcs_load_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, chip_fg_danger());
        ImGui::TextWrapped("%s", state.ebcs_load_error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Column mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Timestamp col", state.ebcs_ts_col, sizeof state.ebcs_ts_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Target boost col", state.ebcs_target_col, sizeof state.ebcs_target_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Actual boost col", state.ebcs_actual_col, sizeof state.ebcs_actual_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Throttle col", state.ebcs_throttle_col, sizeof state.ebcs_throttle_col);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("WGDC col", state.ebcs_wgdc_col, sizeof state.ebcs_wgdc_col);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("RPM col", state.ebcs_rpm_col, sizeof state.ebcs_rpm_col);
        text_subtle("Timestamp, target boost, actual boost, and throttle are mandatory.");
    }

    if (ImGui::CollapsingHeader("Detection")) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Throttle step %", &state.ebcs_throttle_step_pct, 1.0f, 5.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Target boost step", &state.ebcs_target_step, 0.5f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Max event duration s", &state.ebcs_max_event_duration, 0.5f, 1.0f,
                          "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputFloat("Overshoot warn %", &state.ebcs_overshoot_warn_pct, 1.0f, 5.0f, "%.1f");
        ImGui::SetNextItemWidth(140.0f);
        char const *ts_units[] = {"seconds", "millis", "micros", "rows"};
        ImGui::Combo("Timestamp unit", &state.ebcs_ts_unit, ts_units, 4);
    }

    ImGui::Spacing();
    if (ImGui::Button("Analyze##ebcs")) {
        state.ebcs_load_error.clear();
        if (state.ebcs_log_path[0] == '\0') {
            state.ebcs_load_error = "Pick a CSV log first.";
        } else {
            std::ifstream f{state.ebcs_log_path};
            if (!f) {
                state.ebcs_load_error = std::string{"Cannot open '"} + state.ebcs_log_path + "'";
            } else {
                std::string header_line;
                bool got = false;
                while (std::getline(f, header_line)) {
                    while (!header_line.empty() && header_line.back() == '\r') {
                        header_line.pop_back();
                    }
                    if (header_line.empty() || header_line.front() == '#')
                        continue;
                    got = true;
                    break;
                }
                if (!got) {
                    state.ebcs_load_error = "CSV has no header.";
                } else {
                    std::vector<std::string> header_cols;
                    std::size_t start = 0;
                    for (std::size_t i = 0; i <= header_line.size(); ++i) {
                        if (i == header_line.size() || header_line[i] == ',') {
                            std::size_t a = start;
                            std::size_t b = i;
                            while (a < b &&
                                   std::isspace(static_cast<unsigned char>(header_line[a])))
                                ++a;
                            while (b > a &&
                                   std::isspace(static_cast<unsigned char>(header_line[b - 1])))
                                --b;
                            header_cols.emplace_back(header_line.substr(a, b - a));
                            start = i + 1;
                        }
                    }
                    auto const resolve = [&](char const *name) -> std::size_t {
                        if (name == nullptr || name[0] == '\0') {
                            return st::log::ebcs::kNoColumn;
                        }
                        std::string want{name};
                        for (auto &cc : want) {
                            cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                        }
                        for (std::size_t i = 0; i < header_cols.size(); ++i) {
                            std::string have = header_cols[i];
                            for (auto &cc : have) {
                                cc =
                                    static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            }
                            if (have == want)
                                return i;
                        }
                        return st::log::ebcs::kNoColumn;
                    };
                    st::log::ebcs::PidMapping mapping;
                    mapping.timestamp_idx = resolve(state.ebcs_ts_col);
                    mapping.target_boost_idx = resolve(state.ebcs_target_col);
                    mapping.actual_boost_idx = resolve(state.ebcs_actual_col);
                    mapping.throttle_idx = resolve(state.ebcs_throttle_col);
                    mapping.wgdc_idx = resolve(state.ebcs_wgdc_col);
                    mapping.rpm_idx = resolve(state.ebcs_rpm_col);

                    st::log::ebcs::DetectorConfig cfg;
                    cfg.throttle_step_threshold_pct =
                        static_cast<double>(state.ebcs_throttle_step_pct);
                    cfg.target_boost_step_threshold = static_cast<double>(state.ebcs_target_step);
                    cfg.max_event_duration_s = static_cast<double>(state.ebcs_max_event_duration);
                    cfg.overshoot_warn_pct = static_cast<double>(state.ebcs_overshoot_warn_pct);
                    switch (state.ebcs_ts_unit) {
                    case 1:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixMillis;
                        break;
                    case 2:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixMicros;
                        break;
                    case 3:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::RowIndex;
                        break;
                    default:
                        cfg.timestamp_unit = st::log::ebcs::TimestampUnit::UnixSeconds;
                        break;
                    }
                    auto const r =
                        st::log::ebcs::snapshot_from_csv(state.ebcs_log_path, mapping, cfg);
                    if (!r.has_value()) {
                        state.ebcs_load_error = r.error().to_string();
                        state.ebcs_snapshot.reset();
                    } else {
                        state.ebcs_snapshot = *r;
                        state.ebcs_compute_msg = std::to_string(r->good_event_count) + " good of " +
                                                 std::to_string(r->total_event_count) + " events";
                    }
                }
            }
        }
    }
    if (!state.ebcs_compute_msg.empty() && state.ebcs_snapshot.has_value()) {
        ImGui::SameLine();
        text_subtle("%s", state.ebcs_compute_msg.c_str());
    }

    if (state.ebcs_snapshot.has_value()) {
        auto const &snap = *state.ebcs_snapshot;
        ImGui::Separator();
        char const *quality_names[5] = {"Good", "Choppy", "Slow", "Spiked", "Aborted"};

        if (snap.good_event_count > 0) {
            ImGui::Text("Median rise time:        %.3f s", snap.median_rise_time_s);
            ImGui::Text("Median overshoot:        %.2f %%", snap.median_overshoot_pct);
            ImGui::Text("Median settling time:    %.3f s", snap.median_settling_time_s);
            ImGui::Text("Mean steady-state error: %+.3f", snap.mean_steady_state_error);
        }

        ImGui::Spacing();
        ImGui::Text("Suggestions:");
        for (auto const &line : snap.gain_suggestions) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str());
        }

        if (!snap.events.empty()) {
            ImGui::Spacing();
            ImGui::Text("Tip-in events:");
            if (ImGui::BeginTable("##ebcs_events", 8,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 180.0f))) {
                ImGui::TableSetupColumn("start s");
                ImGui::TableSetupColumn("target");
                ImGui::TableSetupColumn("peak");
                ImGui::TableSetupColumn("ss");
                ImGui::TableSetupColumn("rise s");
                ImGui::TableSetupColumn("os %");
                ImGui::TableSetupColumn("settle s");
                ImGui::TableSetupColumn("quality");
                ImGui::TableHeadersRow();
                for (auto const &ev : snap.events) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", ev.start_timestamp);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.2f", ev.target_boost);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.2f", ev.peak_boost);
                    ImGui::TableNextColumn();
                    ImGui::Text("%+.2f", ev.steady_state_boost);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", ev.rise_time_s);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", ev.overshoot_pct);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", ev.settling_time_s);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(quality_names[static_cast<std::size_t>(ev.quality)]);
                }
                ImGui::EndTable();
            }
        }
    } else if (state.ebcs_load_error.empty()) {
        text_subtle("No analysis yet — pick a log and click \"Analyze\".");
    }

    ImGui::End();
}

// Mirrors the CLI's `project-{enable,disable}-dtc`: each toggle writes the
// bit through `st::set_dtc_enabled` and records the byte change as a
// ByteEdit in `edit::History` so Ctrl+Z restores the original bit pattern.
// Emissions-flagged codes get a yellow "E" chip; the flash-time policy
// gate still enforces the jurisdiction profile.
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
    ImGui::End();
}

// "History" panel — GUI mirror of `subuwutuner-cli project-history`. Lists
// every edit in insertion order with a marker for the current History
// cursor, optional table-id filter, and a per-row "Jump" button that
// walks the cursor to make that edit the most-recently-applied. Jumps
// re-use do_undo / do_redo so the dirty flag, status line, and the
// currently-displayed table data all stay coherent with manual Ctrl+Z.
void render_history_panel(AppState &state) {
    if (!state.show_history_panel) {
        return;
    }
    if (!ImGui::Begin("History", &state.show_history_panel)) {
        ImGui::End();
        return;
    }
    if (!state.project.has_value()) {
        render_empty_state(
            "No project",
            "Open one (Ctrl+O) to see your edit history here. Ctrl+Z and Ctrl+Shift+Z navigate it.");
        ImGui::End();
        return;
    }
    auto const &records = state.project->history().records();
    auto const cursor = state.project->history().cursor();

    ImGui::Text("%zu edit(s), cursor at %zu", records.size(), cursor);
    if (cursor < records.size()) {
        ImGui::SameLine();
        text_subtle("(%zu redo step%s)", records.size() - cursor,
                    records.size() - cursor == 1 ? "" : "s");
    }

    ImGui::InputTextWithHint("##history_filter", "Filter by table id or op…", state.history_filter,
                             sizeof state.history_filter);
    std::string_view const filter{state.history_filter};

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
            std::size_t const c = state.project->history().cursor();
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

// Phase 5 placeholder — custom-features node-graph designer. Data
// model lives in st::feature::Graph and is fully implemented; the
// canvas here is intentionally a stub. Future work (multi-session)
// builds out: a grid-backed canvas with pan/zoom, node rendering
// keyed off the pack's hook declarations, drag-to-connect with
// type-checked edges, undo/redo via st::edit::History.
void render_features_designer(AppState &state) {
    if (!state.show_features_designer) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(720.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Custom Features Designer (Preview)", &state.show_features_designer)) {
        ImGui::End();
        return;
    }

    // Esc / right-click cancel an in-progress wire. The non-obvious
    // bit is the `features_wiring_blocked` latch: ImGui DOES detect
    // Esc fine, but the pin-drag handler below re-spawns a wire on
    // the next frame because the mouse is still held over the source
    // pin. Latch suppresses re-spawn until the mouse button is
    // released.
    bool const was_wiring_at_start = state.features_wiring_active;
    if (was_wiring_at_start && (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) ||
                                ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        state.features_wiring_active = false;
        state.features_wiring_blocked = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.features_wiring_blocked = false;
    }

    ImGui::TextColored(chip_fg_warn(), "\xE2\x9A\xA0 Phase 5 preview.");
    text_subtle("Editor, IR, SH-2A codegen, and .stmod persistence "
                "all shipped. Patch insertion + flashing wait on the "
                "bench rig — see docs/16.");

    ImGui::Spacing();
    ImGui::Text("Nodes: %zu     Edges: %zu", state.features_graph.nodes().size(),
                state.features_graph.edges().size());

    // Helper-lambda for spawning nodes at a free-ish slot.
    auto const next_slot_y = [&]() {
        return 40.0f + 80.0f * static_cast<float>(state.features_graph.nodes().size());
    };

    // Pack-driven node palette. Two groups in one popup:
    //   - Hooks (splice / sensor / action points declared by the pack)
    //   - Primitives (pure-computation nodes)
    // Pin-direction convention differs between the two groups:
    //   - For hooks, pack-declared `inputs` become graph Output pins
    //     (the hook offers data TO user logic) and `outputs` become
    //     graph Input pins (the hook demands data FROM user logic).
    //   - For primitives, the convention is the obvious one:
    //     pack-declared `inputs` = graph Input pins, `outputs` =
    //     graph Output pins.
    // Without a loaded project, fall back to the generic debug
    // buttons so an empty workspace can still be exercised.
    bool const have_palette =
        state.project.has_value() && (!state.project->definition().hooks().empty() ||
                                      !state.project->definition().primitives().empty());
    if (have_palette) {
        if (ImGui::Button("Insert \xE2\x96\xBE")) {
            ImGui::OpenPopup("##features_palette");
        }
        if (ImGui::BeginPopup("##features_palette")) {
            auto const &hooks = state.project->definition().hooks();
            auto const &prims = state.project->definition().primitives();
            // Spawn helper — shared between the two groups. The pin-
            // direction lambda decides what `inputs`/`outputs` mean on
            // a given kind of node.
            auto const spawn_node = [&](std::string const &kind_prefix, std::string const &id,
                                        std::string const &display_name,
                                        std::vector<st::HookSignal> const &decl_inputs,
                                        std::vector<st::HookSignal> const &decl_outputs,
                                        st::feature::PinDirection in_dir,
                                        st::feature::PinDirection out_dir, bool phase_break) {
                st::feature::Node n;
                n.kind = kind_prefix + id;
                n.label = !display_name.empty() ? display_name : id;
                n.x = 40.0f;
                n.y = next_slot_y();
                n.is_phase_break = phase_break;
                st::feature::PinId next_pin = 0;
                auto const push_pin = [&](st::HookSignal const &s, st::feature::PinDirection d) {
                    auto pt = st::feature::parse_pin_type(s.type);
                    // Canonical `name` goes into Pin.name so
                    // codegen can resolve it back to the pack's
                    // HookSignal; pretty `label` (if any)
                    // populates Pin.label for display only.
                    // Before this fix Pin.name held the label,
                    // which silently broke compilation.
                    n.pins.push_back(st::feature::Pin{next_pin++, s.name,
                                                      pt.value_or(st::feature::PinType::Float), d,
                                                      s.unit, s.label});
                };
                for (auto const &s : decl_inputs)
                    push_pin(s, in_dir);
                for (auto const &s : decl_outputs)
                    push_pin(s, out_dir);
                state.features_graph.add_node(std::move(n));
            };

            if (!hooks.empty()) {
                text_subtle("Hooks");
                ImGui::Separator();
                for (auto const &h : hooks) {
                    char const *human =
                        !h.display_name.empty() ? h.display_name.c_str() : h.id.c_str();
                    char label[200];
                    std::snprintf(label, sizeof label, "%s##hook_%s", human, h.id.c_str());
                    if (ImGui::MenuItem(label)) {
                        spawn_node("hook.", h.id, h.display_name, h.inputs, h.outputs,
                                   /*in_dir =*/st::feature::PinDirection::Output,
                                   /*out_dir=*/st::feature::PinDirection::Input,
                                   /*phase_break=*/true);
                    }
                    if (!h.description.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", h.description.c_str());
                    }
                }
            }
            if (!prims.empty()) {
                if (!hooks.empty()) {
                    ImGui::Spacing();
                }
                text_subtle("Primitives");
                ImGui::Separator();
                for (auto const &p : prims) {
                    char const *human =
                        !p.display_name.empty() ? p.display_name.c_str() : p.id.c_str();
                    char label[200];
                    std::snprintf(label, sizeof label, "%s##prim_%s", human, p.id.c_str());
                    if (ImGui::MenuItem(label)) {
                        spawn_node("primitive.", p.id, p.display_name, p.inputs, p.outputs,
                                   /*in_dir =*/st::feature::PinDirection::Input,
                                   /*out_dir=*/st::feature::PinDirection::Output,
                                   /*phase_break=*/false);
                    }
                    if (!p.description.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", p.description.c_str());
                    }
                }
            }
            ImGui::EndPopup();
        }
    } else {
        if (ImGui::Button("Add source (Float out)")) {
            st::feature::Node n;
            n.kind = "sensor.float";
            n.label = "Source";
            n.x = 40.0f;
            n.y = next_slot_y();
            n.pins.push_back(st::feature::Pin{0, "out", st::feature::PinType::Float,
                                              st::feature::PinDirection::Output, ""});
            state.features_graph.add_node(std::move(n));
        }
        ImGui::SameLine();
        if (ImGui::Button("Add sink (Float in)")) {
            st::feature::Node n;
            n.kind = "output.float";
            n.label = "Sink";
            n.x = 320.0f;
            n.y = next_slot_y();
            n.pins.push_back(st::feature::Pin{0, "in", st::feature::PinType::Float,
                                              st::feature::PinDirection::Input, ""});
            state.features_graph.add_node(std::move(n));
        }
        ImGui::SameLine();
        if (ImGui::Button("Add 2-in/1-out (Float)")) {
            st::feature::Node n;
            n.kind = "math.add";
            n.label = "Add";
            n.x = 180.0f;
            n.y = next_slot_y();
            n.pins.push_back(st::feature::Pin{0, "a", st::feature::PinType::Float,
                                              st::feature::PinDirection::Input, ""});
            n.pins.push_back(st::feature::Pin{1, "b", st::feature::PinType::Float,
                                              st::feature::PinDirection::Input, ""});
            n.pins.push_back(st::feature::Pin{2, "out", st::feature::PinType::Float,
                                              st::feature::PinDirection::Output, ""});
            state.features_graph.add_node(std::move(n));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear graph")) {
        state.features_graph = st::feature::Graph{};
        state.features_wiring_active = false;
        state.features_wire_error.clear();
        state.features_selected_nodes.clear();
        state.features_selected_edge.reset();
        state.features_context_edge.reset();
        state.features_band_active = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) {
        state.features_view_offset = ImVec2(0.0f, 0.0f);
        state.features_view_scale = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save…")) {
        nfdu8filteritem_t filters[1] = {{"SubuwuTuner mod (TOML)", "stmod"}};
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::SaveDialog(out, filters, 1, nullptr, "graph.stmod");
        if (r == NFD_OKAY) {
            std::ofstream ofs{out.get(), std::ios::trunc};
            if (!ofs) {
                state.features_wire_error = std::string{"save: cannot open "} + out.get();
            } else {
                ofs << st::feature::to_toml(state.features_graph);
                state.features_wire_error.clear();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load…")) {
        nfdu8filteritem_t filters[1] = {{"SubuwuTuner mod (TOML)", "stmod"}};
        NFD::UniquePathU8 out;
        nfdresult_t const r = NFD::OpenDialog(out, filters, 1);
        if (r == NFD_OKAY) {
            std::ifstream ifs{out.get(), std::ios::binary};
            if (!ifs) {
                state.features_wire_error = std::string{"load: cannot open "} + out.get();
            } else {
                std::stringstream buf;
                buf << ifs.rdbuf();
                auto loaded = st::feature::from_toml(buf.str());
                if (!loaded.has_value()) {
                    state.features_wire_error = "load: " + loaded.error().to_string();
                } else {
                    state.features_graph = std::move(*loaded);
                    state.features_wiring_active = false;
                    state.features_wire_error.clear();
                    state.features_selected_nodes.clear();
                    state.features_selected_edge.reset();
                    state.features_context_edge.reset();
                    state.features_band_active = false;
                }
            }
        }
    }

    // Status chip — three-state, styled like the status-bar profile
    // chip so the editor's quality signal lives at the eye-line of
    // the toolbar rather than in muted body text below it. Click
    // opens a popup with the actionable detail.
    auto const validate_result = state.features_graph.validate();
    auto const lint_findings = st::feature::lint(state.features_graph);
    char const *chip_label;
    ImVec4 chip_fg, chip_bg;
    char chip_buf[48];
    if (!validate_result.has_value()) {
        chip_label = "Invalid";
        chip_fg = chip_fg_danger();
        chip_bg = chip_bg_danger();
    } else if (!lint_findings.empty()) {
        std::snprintf(chip_buf, sizeof chip_buf, "%zu warning%s", lint_findings.size(),
                      lint_findings.size() == 1 ? "" : "s");
        chip_label = chip_buf;
        chip_fg = chip_fg_warn();
        chip_bg = chip_bg_warn();
    } else {
        chip_label = "OK";
        chip_fg = chip_fg_ok();
        chip_bg = chip_bg_ok();
    }
    chip(chip_label, chip_fg, chip_bg);
    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup("##features_status_popup");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click for details.");
    }
    if (ImGui::BeginPopup("##features_status_popup")) {
        if (!validate_result.has_value()) {
            ImGui::TextColored(chip_fg_danger(), "Structural error");
            ImGui::Separator();
            ImGui::TextWrapped("%s", validate_result.error().to_string().c_str());
        } else if (!lint_findings.empty()) {
            text_subtle("Completeness warnings");
            ImGui::Separator();
            for (auto const &f : lint_findings) {
                ImGui::BulletText("%s", f.message.c_str());
            }
        } else {
            text_subtle("Graph passes structural validation "
                        "and has no completeness warnings.");
        }
        ImGui::EndPopup();
    }

    if (!state.features_wire_error.empty()) {
        ImGui::TextColored(chip_fg_danger(), "wire: %s",
                           state.features_wire_error.c_str());
    }
    text_subtle("Click a node or edge to select; Shift+click "
                "to multi-select. Drag from empty canvas to "
                "box-select. Delete removes the selection. "
                "Drag a body to move (group-drag works); "
                "drag pin → pin to wire. Middle-drag pans, "
                "wheel zooms. Right-click (when not wiring) "
                "for delete / disconnect.");

    ImGui::Spacing();

    // ---- Canvas ----------------------------------------------------
    // Layout constants in graph space. The viewport applies a pan
    // (`view_offset`) and zoom (`view_scale`) on top, so anything
    // that converts a graph-space measurement to screen pixels has
    // to multiply by `scale`. Snap-to-grid stays in graph space —
    // the grid drawing scales but the snap step doesn't.
    constexpr float kNodeWidth = 150.0f;
    constexpr float kHeaderHeight = 24.0f;
    constexpr float kPinRowHeight = 20.0f;
    constexpr float kPinRadius = 5.0f;
    constexpr float kPinHitRadius = 10.0f;
    constexpr float kFooterPadding = 8.0f;
    constexpr float kGridStep = 24.0f;

    ImVec2 const canvas_p = ImGui::GetCursorScreenPos();
    ImVec2 const canvas_sz = ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x),
                                    std::max(240.0f, ImGui::GetContentRegionAvail().y - 8.0f));
    ImVec2 const canvas_end(canvas_p.x + canvas_sz.x, canvas_p.y + canvas_sz.y);

    // View transform. Lambdas take graph-space, return screen-space
    // (and back). Kept as locals so the rest of the function can
    // read `scale` directly when it's the cleaner expression.
    float const scale = state.features_view_scale;
    ImVec2 const offset = state.features_view_offset;
    auto const to_screen = [&](float gx, float gy) {
        return ImVec2(canvas_p.x + offset.x + gx * scale, canvas_p.y + offset.y + gy * scale);
    };

    // Pan + zoom input. Middle-mouse drag pans; mouse wheel zooms
    // anchored to the cursor (the graph point under the cursor stays
    // put as the scale changes). Both gated on canvas hover so they
    // don't fight with toolbar / context menus elsewhere in the panel.
    bool const canvas_hovered = ImGui::IsMouseHoveringRect(canvas_p, canvas_end) &&
                                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        ImVec2 const d = ImGui::GetIO().MouseDelta;
        state.features_view_offset.x += d.x;
        state.features_view_offset.y += d.y;
    }
    if (canvas_hovered) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float const old_scale = state.features_view_scale;
            float const factor = wheel > 0.0f ? 1.1f : 1.0f / 1.1f;
            float const new_scale = std::clamp(old_scale * factor, 0.25f, 3.0f);
            if (new_scale != old_scale) {
                ImVec2 const m = ImGui::GetIO().MousePos;
                // Graph-space point under the cursor at the old scale.
                float const gx = (m.x - canvas_p.x - state.features_view_offset.x) / old_scale;
                float const gy = (m.y - canvas_p.y - state.features_view_offset.y) / old_scale;
                state.features_view_offset.x = m.x - canvas_p.x - gx * new_scale;
                state.features_view_offset.y = m.y - canvas_p.y - gy * new_scale;
                state.features_view_scale = new_scale;
            }
        }
    }

    auto *const dl = ImGui::GetWindowDrawList();
    ImU32 const grid_col = ImGui::GetColorU32(ImGuiCol_Separator);
    ImU32 const bg_col = ImGui::GetColorU32(ImGuiCol_ChildBg);
    dl->AddRectFilled(canvas_p, canvas_end, bg_col);
    // Grid lines: drawn in graph-aligned screen positions. Hidden
    // when the on-screen step would be too dense to be useful.
    {
        float const step_s = kGridStep * scale;
        if (step_s >= 4.0f) {
            float start_x = std::fmod(state.features_view_offset.x, step_s);
            if (start_x > 0.0f)
                start_x -= step_s;
            for (float x = start_x; x < canvas_sz.x; x += step_s) {
                if (x < 0.0f)
                    continue;
                dl->AddLine(ImVec2(canvas_p.x + x, canvas_p.y),
                            ImVec2(canvas_p.x + x, canvas_p.y + canvas_sz.y), grid_col);
            }
            float start_y = std::fmod(state.features_view_offset.y, step_s);
            if (start_y > 0.0f)
                start_y -= step_s;
            for (float y = start_y; y < canvas_sz.y; y += step_s) {
                if (y < 0.0f)
                    continue;
                dl->AddLine(ImVec2(canvas_p.x, canvas_p.y + y),
                            ImVec2(canvas_p.x + canvas_sz.x, canvas_p.y + y), grid_col);
            }
        }
    }
    dl->PushClipRect(canvas_p, canvas_end, true);

    // Per-pin screen-position cache. Built while drawing nodes and
    // read when rendering edges + during wiring hit-tests.
    struct PinPos {
        st::feature::NodeId node_id;
        st::feature::PinId pin_id;
        st::feature::PinDirection direction;
        st::feature::PinType type;
        ImVec2 pos;
    };
    std::vector<PinPos> pin_positions;
    pin_positions.reserve(state.features_graph.nodes().size() * 3);

    // Set of (node, pin) pairs for Input pins currently driven by
    // an edge. Used both to render the "= value" suffix on
    // unconnected-but-defaulted pins and to gate the default-value
    // editor (only shows on undriven inputs). Pre-computed once so
    // the per-pin path doesn't redo it.
    auto const pin_key = [](st::feature::NodeId nid, st::feature::PinId pid) {
        return (static_cast<std::uint64_t>(nid) << 32) | static_cast<std::uint64_t>(pid);
    };
    std::unordered_set<std::uint64_t> driven_inputs;
    for (auto const &e : state.features_graph.edges()) {
        driven_inputs.insert(pin_key(e.to_node, e.to_pin));
    }
    auto const is_pin_driven = [&](st::feature::NodeId nid, st::feature::PinId pid) {
        return driven_inputs.find(pin_key(nid, pid)) != driven_inputs.end();
    };

    // Helper: what text the editor renders for a pin's label. For
    // unconnected Input pins with a default_value set, the suffix
    // "= value" is appended so the constant is visible inline.
    // Bool prints true/false; Int truncates; Float uses %g.
    auto const pin_display_text = [&](st::feature::Node const &node,
                                      st::feature::Pin const &p) -> std::string {
        // Display prefers `label` (the pretty name from the pack);
        // falls back to `name` (the snake_case canonical) when no
        // label was supplied. Codegen lookups still use `name`.
        std::string const &disp = !p.label.empty() ? p.label : p.name;
        if (p.direction == st::feature::PinDirection::Input && !is_pin_driven(node.id, p.id) &&
            p.default_value.has_value()) {
            char buf[96];
            if (p.type == st::feature::PinType::Bool) {
                std::snprintf(buf, sizeof buf, "%s = %s", disp.c_str(),
                              *p.default_value > 0.5 ? "true" : "false");
            } else if (p.type == st::feature::PinType::Int) {
                std::snprintf(buf, sizeof buf, "%s = %lld", disp.c_str(),
                              static_cast<long long>(*p.default_value));
            } else {
                std::snprintf(buf, sizeof buf, "%s = %g", disp.c_str(), *p.default_value);
            }
            return buf;
        }
        return disp;
    };

    // Pending default-value mutation, applied after the loop. Empty
    // value clears the default; populated value sets it. Captured
    // by the pin context menu's editor.
    struct PendingDefault {
        st::feature::NodeId node_id;
        st::feature::PinId pin_id;
        std::optional<double> value;
    };
    std::optional<PendingDefault> pending_default;

    // Pin appearance constants — derived from the active accent. The
    // pin fill matches the type's category (today only Float/Int/Bool
    // share one color; future work could vary).
    auto const accent = accent_for(current_theme());
    ImU32 const pin_fill = ImGui::GetColorU32(accent.base);
    ImU32 const pin_brd = ImGui::GetColorU32(ImGuiCol_Border);
    ImU32 const node_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 const node_brd = ImGui::GetColorU32(ImGuiCol_Border);
    ImU32 const hdr_bg = ImGui::GetColorU32(ImGuiCol_TitleBgActive);
    ImU32 const txt_col = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 const disabled_t = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    (void)disabled_t; // reserved for future pin labels in disabled state

    // Snapshot the nodes by id so we can iterate stably while issuing
    // ImGui::InvisibleButton hit-tests (the node positions get
    // mutated mid-loop via set_node_position).
    auto const &nodes = state.features_graph.nodes();

    // Per-node screen-rect cache, populated during the draw pass and
    // consumed by the rubber-band hit-test below. Saves recomputing
    // the per-node width measurement (which involves CalcTextSizeA
    // on every pin label).
    struct NodeRect {
        st::feature::NodeId id;
        ImVec2 tl;
        ImVec2 br;
    };
    std::vector<NodeRect> node_rects;
    node_rects.reserve(nodes.size());

    // Pending mutations from context-menu actions. Deferred until
    // after the loop so we don't invalidate iteration / pin caches.
    // Nodes are batched for the multi-select Delete-key path; the
    // edge variant stays singular (no edge multi-select yet).
    std::vector<st::feature::NodeId> pending_delete_nodes;
    std::optional<st::feature::Edge> pending_delete_edge;

    // Delete key removes the current selection — every selected
    // node, or the selected edge, depending on which one is set.
    // Gated on window focus so deletes elsewhere (table-cell editing,
    // etc.) don't blow away the designer's selection by accident.
    bool const designer_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (designer_focused && ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false)) {
        if (!state.features_selected_nodes.empty()) {
            pending_delete_nodes = state.features_selected_nodes;
        } else if (state.features_selected_edge.has_value()) {
            pending_delete_edge = state.features_selected_edge;
        }
    }

    ImFont *const fnt = ImGui::GetFont();
    float const fnt_sz_s = ImGui::GetFontSize() * scale;
    float const hdr_h_s = kHeaderHeight * scale;
    float const row_h_s = kPinRowHeight * scale;
    float const foot_s = kFooterPadding * scale;
    float const width_s = kNodeWidth * scale;
    float const pin_r_s = kPinRadius * scale;
    float const pin_hr_s = kPinHitRadius * scale;

    // Tracks whether any per-node body or pin InvisibleButton ended
    // up hovered this frame. Edge hit-test consumes this to decide
    // whether to even consider beziers — but ImGui's stock
    // `IsAnyItemHovered()` ORs in the *previous* frame's hovered id,
    // which means the canvas-bg button (always hovered last frame
    // when cursor is over empty canvas) keeps it permanently true.
    // Tracking manually inside the loop dodges that one-frame lag.
    bool any_node_pin_hovered = false;

    for (auto const &n : nodes) {
        std::size_t const inputs = static_cast<std::size_t>(
            std::count_if(n.pins.begin(), n.pins.end(), [](st::feature::Pin const &p) {
                return p.direction == st::feature::PinDirection::Input;
            }));
        std::size_t const outputs = static_cast<std::size_t>(
            std::count_if(n.pins.begin(), n.pins.end(), [](st::feature::Pin const &p) {
                return p.direction == st::feature::PinDirection::Output;
            }));
        std::size_t const rows = std::max<std::size_t>(1, std::max(inputs, outputs));
        float const node_h_s = hdr_h_s + static_cast<float>(rows) * row_h_s + foot_s;

        // Per-node body width — fits the longest input-name +
        // longest output-name on any row, plus the header label,
        // so pack-driven hooks with verbose signal names (e.g.
        // `commanded_pw_override`) don't overflow the 150 px
        // default. Falls back to the default when labels are short.
        char const *header_text = n.label.empty() ? n.kind.c_str() : n.label.c_str();
        float const header_w = fnt->CalcTextSizeA(fnt_sz_s, FLT_MAX, 0.0f, header_text).x;
        float max_in_w = 0.0f;
        float max_out_w = 0.0f;
        for (auto const &p : n.pins) {
            std::string const txt = pin_display_text(n, p);
            float const w = fnt->CalcTextSizeA(fnt_sz_s, FLT_MAX, 0.0f, txt.c_str()).x;
            if (p.direction == st::feature::PinDirection::Input) {
                max_in_w = std::max(max_in_w, w);
            } else {
                max_out_w = std::max(max_out_w, w);
            }
        }
        // Pin row: [circle][6px gap][in_name] … [out_name][6px gap][circle]
        float const pad_s = 6.0f * scale;
        float const inner_gap_s = 12.0f * scale; // visible gutter
                                                 // between the two
                                                 // text columns
        float const pins_w = pin_r_s + pad_s + max_in_w + inner_gap_s + max_out_w + pad_s + pin_r_s;
        float const header_w_total = header_w + 16.0f * scale;
        float const node_w_s = std::max({width_s, pins_w, header_w_total});

        ImVec2 const node_tl = to_screen(n.x, n.y);
        ImVec2 const node_br(node_tl.x + node_w_s, node_tl.y + node_h_s);
        node_rects.push_back({n.id, node_tl, node_br});

        // Body hit zone — InvisibleButton lives at the body origin so
        // ImGui's IsItemActive + drag-delta machinery handles the
        // move logic without needing manual click bookkeeping.
        char body_id[24];
        std::snprintf(body_id, sizeof body_id, "##fnode_%u", static_cast<unsigned>(n.id));
        ImGui::SetCursorScreenPos(node_tl);
        ImGui::InvisibleButton(body_id, ImVec2(node_w_s, node_h_s));
        bool const body_hovered = ImGui::IsItemHovered();
        if (body_hovered)
            any_node_pin_hovered = true;
        bool const body_active = ImGui::IsItemActive();
        bool const body_deactivated = ImGui::IsItemDeactivated();
        bool const body_left_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool const body_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

        // Hover tooltip — surfaces the human-friendly description
        // from the pack-declared hook or primitive (or just the
        // kind for generic debug nodes). Suppressed during drag so
        // it doesn't track the cursor mid-move. Suppressed while
        // wiring so the user can read the in-flight target instead.
        if (body_hovered && !body_active && !state.features_wiring_active) {
            char const *node_kind = n.kind.c_str();
            std::string description;
            bool is_pack_node = false;
            if (state.project.has_value()) {
                auto const &def = state.project->definition();
                if (n.kind.starts_with("hook.")) {
                    is_pack_node = true;
                    std::string_view const id{n.kind.data() + 5, n.kind.size() - 5};
                    if (auto const *h = def.find_hook(id); h != nullptr) {
                        description = h->description;
                    }
                } else if (n.kind.starts_with("primitive.")) {
                    is_pack_node = true;
                    std::string_view const id{n.kind.data() + 10, n.kind.size() - 10};
                    if (auto const *p = def.find_primitive(id); p != nullptr) {
                        description = p->description;
                    }
                }
            }
            ImGui::BeginTooltip();
            char const *title_str = !n.label.empty() ? n.label.c_str() : node_kind;
            ImGui::TextUnformatted(title_str);
            text_subtle("%s", node_kind);
            if (!description.empty()) {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextWrapped("%s", description.c_str());
                ImGui::PopTextWrapPos();
            } else if (!is_pack_node) {
                ImGui::Separator();
                text_subtle("Generic debug node. Load a project with hooks "
                            "to insert pack-declared nodes from the palette.");
            }
            ImGui::EndTooltip();
        }
        if (body_left_clicked) {
            // Shift toggles the node in/out of the selection set.
            // Plain click on a non-selected node replaces with
            // {this}; plain click on an already-selected node keeps
            // the selection intact so a subsequent drag moves every
            // selected node together (file-manager / Sketch / Figma
            // style group-drag). Edge selection always clears — only
            // one kind of thing highlighted at a time.
            bool const shift = ImGui::GetIO().KeyShift;
            auto &sel = state.features_selected_nodes;
            auto it = std::find(sel.begin(), sel.end(), n.id);
            bool const was_in_sel = (it != sel.end());
            if (shift) {
                if (was_in_sel)
                    sel.erase(it);
                else
                    sel.push_back(n.id);
            } else if (!was_in_sel) {
                sel.assign({n.id});
            }
            state.features_selected_edge.reset();
        }
        if (body_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            // Mouse delta is in screen pixels; convert back to graph
            // space before storing — otherwise a zoomed-in canvas
            // would over-shoot moves and a zoomed-out one would lag.
            // Move every selected node by the same delta so group
            // drag works; fall back to just the active node if it
            // somehow isn't in the selection (shouldn't happen given
            // the click handler above, but the cost is one set
            // membership check).
            ImVec2 const d = ImGui::GetIO().MouseDelta;
            auto const &sel = state.features_selected_nodes;
            bool const active_in_sel = std::find(sel.begin(), sel.end(), n.id) != sel.end();
            if (active_in_sel) {
                for (auto id : sel) {
                    if (auto const *nn = state.features_graph.find_node(id)) {
                        state.features_graph.set_node_position(id, nn->x + d.x / scale,
                                                               nn->y + d.y / scale);
                    }
                }
            } else {
                state.features_graph.set_node_position(n.id, n.x + d.x / scale, n.y + d.y / scale);
            }
        }
        // Snap-to-grid on drag release. IsItemDeactivated fires once
        // on the frame the active button stops being active — exactly
        // the "user just let go" event we want. Snap is intentionally
        // release-only (not live) so the drag feels smooth and the
        // tidy-up happens after the user commits the move. With
        // group drag, snap every selected node so the whole group
        // lands on the grid.
        if (body_deactivated) {
            auto const &sel = state.features_selected_nodes;
            bool const active_in_sel = std::find(sel.begin(), sel.end(), n.id) != sel.end();
            auto const snap_one = [&](st::feature::NodeId id) {
                if (auto const *nn = state.features_graph.find_node(id)) {
                    float const sx = std::round(nn->x / kGridStep) * kGridStep;
                    float const sy = std::round(nn->y / kGridStep) * kGridStep;
                    state.features_graph.set_node_position(id, sx, sy);
                }
            };
            if (active_in_sel) {
                for (auto id : sel)
                    snap_one(id);
            } else {
                snap_one(n.id);
            }
        }
        // Right-click on the body opens a context menu. Suppressed
        // when the click cancelled an in-progress wire (handled at
        // the top of the function) — using `was_wiring_at_start`
        // because by this point in the frame, features_wiring_active
        // has already been cleared.
        char node_popup_id[28];
        std::snprintf(node_popup_id, sizeof node_popup_id, "##nctx_%u",
                      static_cast<unsigned>(n.id));
        if (body_right_clicked && !was_wiring_at_start) {
            ImGui::OpenPopup(node_popup_id);
        }
        if (ImGui::BeginPopup(node_popup_id)) {
            text_subtle("%s", n.label.empty() ? n.kind.c_str() : n.label.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Node")) {
                pending_delete_nodes.push_back(n.id);
            }
            ImGui::EndPopup();
        }

        // Draw chrome. Selected nodes get a thicker accent-coloured
        // border so the active selection reads at a glance.
        bool const is_selected =
            std::find(state.features_selected_nodes.begin(), state.features_selected_nodes.end(),
                      n.id) != state.features_selected_nodes.end();
        dl->AddRectFilled(node_tl, node_br, node_bg, 6.0f);
        dl->AddRectFilled(node_tl, ImVec2(node_br.x, node_tl.y + hdr_h_s), hdr_bg, 6.0f,
                          ImDrawFlags_RoundCornersTop);
        ImU32 const this_brd = is_selected ? ImGui::GetColorU32(accent.base) : node_brd;
        float const this_thick = is_selected ? 2.5f : 1.0f;
        dl->AddRect(node_tl, node_br, this_brd, 6.0f, 0, this_thick);
        // Header label. Drawn at the scaled font size so it tracks
        // the zoom level — ImGui's default AddText overload uses the
        // active font size, which is wrong here.
        char const *label = n.label.empty() ? n.kind.c_str() : n.label.c_str();
        dl->AddText(fnt, fnt_sz_s,
                    ImVec2(node_tl.x + 8.0f * scale, node_tl.y + (hdr_h_s - fnt_sz_s) * 0.5f),
                    txt_col, label);

        // Lay pins out by direction. Inputs sit on the left edge,
        // outputs on the right; both vertically distributed within
        // the pin-row band.
        std::size_t in_idx = 0;
        std::size_t out_idx = 0;
        for (auto const &p : n.pins) {
            bool const is_input = p.direction == st::feature::PinDirection::Input;
            std::size_t const row_idx = is_input ? in_idx++ : out_idx++;
            float const row_center_y =
                node_tl.y + hdr_h_s + (static_cast<float>(row_idx) + 0.5f) * row_h_s;
            float const pin_x = is_input ? node_tl.x : node_br.x;
            ImVec2 const pin_center(pin_x, row_center_y);

            dl->AddCircleFilled(pin_center, pin_r_s, pin_fill);
            dl->AddCircle(pin_center, pin_r_s, pin_brd);
            // Pin name beside the circle, on the inside edge. Text
            // width measured at the scaled font size so the trailing
            // offset for output pins stays correct under zoom. The
            // display text includes a "= value" suffix when the
            // pin is an undriven Input with a default_value — the
            // constant editor populates this and the renderer
            // surfaces it inline.
            std::string const display_text = pin_display_text(n, p);
            ImVec2 const text_size =
                fnt->CalcTextSizeA(fnt_sz_s, FLT_MAX, 0.0f, display_text.c_str());
            ImVec2 const text_pos =
                is_input
                    ? ImVec2(pin_center.x + pin_r_s + 6.0f * scale, row_center_y - fnt_sz_s * 0.5f)
                    : ImVec2(pin_center.x - pin_r_s - 6.0f * scale - text_size.x,
                             row_center_y - fnt_sz_s * 0.5f);
            dl->AddText(fnt, fnt_sz_s, text_pos, txt_col, display_text.c_str());

            pin_positions.push_back(PinPos{n.id, p.id, p.direction, p.type, pin_center});

            // Pin hit zone — slightly larger than the visible circle
            // so the pin is easy to grab. Tracked by InvisibleButton
            // so we can detect drag start cleanly without colliding
            // with the body's hit zone (which we cover with the body
            // button above; ImGui's last-issued button wins on overlap,
            // so issuing the pin zone AFTER the body means the pin
            // takes precedence in the overlap region).
            char pin_id_buf[40];
            std::snprintf(pin_id_buf, sizeof pin_id_buf, "##fpin_%u_%u",
                          static_cast<unsigned>(n.id), static_cast<unsigned>(p.id));
            ImGui::SetCursorScreenPos(ImVec2(pin_center.x - pin_hr_s, pin_center.y - pin_hr_s));
            ImGui::InvisibleButton(pin_id_buf, ImVec2(pin_hr_s * 2.0f, pin_hr_s * 2.0f));
            bool const pin_hovered = ImGui::IsItemHovered();
            bool const pin_active = ImGui::IsItemActive();
            if (pin_hovered)
                any_node_pin_hovered = true;
            // Tooltip: prefers the pretty label, parenthesizes the
            // canonical name when both are present (so users can see
            // the snake_case id needed for hand-authoring .stmod
            // files or for codegen error messages).
            if (pin_hovered) {
                if (!p.label.empty() && p.label != p.name) {
                    ImGui::SetTooltip("%s (%s) : %s%s%s", p.label.c_str(), p.name.c_str(),
                                      st::feature::pin_type_name(p.type),
                                      p.unit.empty() ? "" : "  ", p.unit.c_str());
                } else {
                    ImGui::SetTooltip("%s : %s%s%s", p.name.c_str(),
                                      st::feature::pin_type_name(p.type),
                                      p.unit.empty() ? "" : "  ", p.unit.c_str());
                }
            }
            // Start wiring on drag-out from a pin. The wiring is
            // bi-directional intent: dragging from an output starts a
            // wire heading toward an input; dragging from an input
            // starts a wire heading toward an output. We resolve the
            // direction on drop so the user doesn't have to think
            // about pin polarity.
            bool const pin_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            if (pin_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) &&
                !state.features_wiring_active && !state.features_wiring_blocked) {
                state.features_wiring_active = true;
                state.features_wiring_from_node = n.id;
                state.features_wiring_from_pin = p.id;
                state.features_wire_error.clear();
            }
            // Right-click on a pin opens a context menu listing every
            // edge touching it (typically 0 or 1, more on output fan-
            // out). Each row deletes that edge.
            char pin_popup_id[40];
            std::snprintf(pin_popup_id, sizeof pin_popup_id, "##pctx_%u_%u",
                          static_cast<unsigned>(n.id), static_cast<unsigned>(p.id));
            if (pin_right_clicked && !was_wiring_at_start) {
                ImGui::OpenPopup(pin_popup_id);
                // Pre-fill the constant editor with the pin's
                // current value (or 0 when none), so the InputFloat
                // inside the popup starts on the right number.
                state.features_pin_edit_buf =
                    p.default_value.has_value() ? static_cast<float>(*p.default_value) : 0.0f;
            }
            if (ImGui::BeginPopup(pin_popup_id)) {
                if (!p.label.empty() && p.label != p.name) {
                    text_subtle("%s (%s) : %s", p.label.c_str(), p.name.c_str(),
                                st::feature::pin_type_name(p.type));
                } else {
                    text_subtle("%s : %s", p.name.c_str(), st::feature::pin_type_name(p.type));
                }
                ImGui::Separator();
                bool any_edge = false;
                for (auto const &e : state.features_graph.edges()) {
                    bool const touches = (e.from_node == n.id && e.from_pin == p.id) ||
                                         (e.to_node == n.id && e.to_pin == p.id);
                    if (!touches)
                        continue;
                    any_edge = true;
                    // Label the other end so the user knows what
                    // they're disconnecting on a fan-out.
                    st::feature::NodeId other_n = (e.from_node == n.id) ? e.to_node : e.from_node;
                    st::feature::PinId other_p = (e.from_node == n.id) ? e.to_pin : e.from_pin;
                    auto const *on = state.features_graph.find_node(other_n);
                    auto const *op = state.features_graph.find_pin(other_n, other_p);
                    char item_label[96];
                    std::snprintf(item_label, sizeof item_label, "Disconnect from %s.%s",
                                  on != nullptr
                                      ? (on->label.empty() ? on->kind.c_str() : on->label.c_str())
                                      : "?",
                                  op != nullptr ? op->name.c_str() : "?");
                    if (ImGui::MenuItem(item_label)) {
                        pending_delete_edge = e;
                    }
                }
                // Constant-value editor — only shown on Input pins
                // (Output pins don't consume values) and only when
                // no edge drives the pin (a driver always wins over
                // the default; surfacing the editor for a wired
                // input would be misleading).
                bool const editor_eligible =
                    p.direction == st::feature::PinDirection::Input && !is_pin_driven(n.id, p.id);
                if (editor_eligible) {
                    if (any_edge)
                        ImGui::Separator();
                    text_subtle("Constant value");
                    ImGui::SetNextItemWidth(120.0f);
                    bool commit = false;
                    if (p.type == st::feature::PinType::Bool) {
                        bool b = state.features_pin_edit_buf > 0.5f;
                        if (ImGui::Checkbox("##pin_def_bool", &b)) {
                            state.features_pin_edit_buf = b ? 1.0f : 0.0f;
                            commit = true;
                        }
                    } else if (p.type == st::feature::PinType::Int) {
                        int v = static_cast<int>(state.features_pin_edit_buf);
                        if (ImGui::InputInt("##pin_def_int", &v, 0, 0,
                                            ImGuiInputTextFlags_EnterReturnsTrue)) {
                            state.features_pin_edit_buf = static_cast<float>(v);
                            commit = true;
                        }
                    } else {
                        if (ImGui::InputFloat("##pin_def_float", &state.features_pin_edit_buf, 0.0f,
                                              0.0f, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue)) {
                            commit = true;
                        }
                    }
                    if (commit) {
                        pending_default = PendingDefault{
                            n.id, p.id, static_cast<double>(state.features_pin_edit_buf)};
                        ImGui::CloseCurrentPopup();
                    }
                    if (p.default_value.has_value()) {
                        if (ImGui::MenuItem("Clear Value")) {
                            pending_default = PendingDefault{n.id, p.id, std::nullopt};
                        }
                    }
                }
                if (!any_edge && !editor_eligible) {
                    text_subtle("(no edges)");
                }
                ImGui::EndPopup();
            }
            // Highlight pins that would accept the in-flight wire.
            if (state.features_wiring_active && (state.features_wiring_from_node != n.id ||
                                                 state.features_wiring_from_pin != p.id)) {
                auto const *from = state.features_graph.find_pin(state.features_wiring_from_node,
                                                                 state.features_wiring_from_pin);
                if (from != nullptr && from->direction != p.direction && from->type == p.type) {
                    dl->AddCircle(pin_center, pin_r_s + 3.0f * scale, pin_fill, 0, 2.0f);
                }
            }
        }
    }

    // ---- Edge hit-test, click handling, and rendering -------------
    // Edges aren't ImGui items (no InvisibleButton — they're
    // drawn-only beziers), so click handling rolls its own. The
    // approach: sample N points along each curve, find the closest
    // edge to the cursor under an 8px threshold. Skip while any
    // node/pin is hovered (those win in overlap) and while a wire
    // is in flight (the in-flight bezier would self-hit otherwise).
    ImU32 const edge_col = ImGui::GetColorU32(accent.base);
    ImU32 const edge_sel_col = ImGui::GetColorU32(accent.hover);
    auto const find_pos = [&](st::feature::NodeId nid, st::feature::PinId pid) -> ImVec2 const * {
        for (auto const &pp : pin_positions) {
            if (pp.node_id == nid && pp.pin_id == pid)
                return &pp.pos;
        }
        return nullptr;
    };
    auto const edge_eq = [](st::feature::Edge const &a, st::feature::Edge const &b) {
        return a.from_node == b.from_node && a.from_pin == b.from_pin && a.to_node == b.to_node &&
               a.to_pin == b.to_pin;
    };
    auto const cubic_at = [](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
        float const u = 1.0f - t;
        float const w0 = u * u * u;
        float const w1 = 3.0f * u * u * t;
        float const w2 = 3.0f * u * t * t;
        float const w3 = t * t * t;
        return ImVec2(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                      w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y);
    };

    auto const &edges = state.features_graph.edges();

    // Find the closest edge under the cursor. Empty means none in
    // range or hit-test suppressed this frame.
    std::optional<std::size_t> hovered_edge_idx;
    {
        bool const can_hit = !state.features_wiring_active && !any_node_pin_hovered &&
                             ImGui::IsMouseHoveringRect(canvas_p, canvas_end);
        if (can_hit) {
            ImVec2 const mouse = ImGui::GetIO().MousePos;
            // 14 px screen-space threshold — wide enough that the
            // user doesn't need to land precisely on the 2.5-3 px
            // stroke. Tightening this past ~10 makes the curve
            // genuinely fiddly to click on a moving cursor.
            float const thresh_px = 14.0f;
            float best_d2 = thresh_px * thresh_px;
            constexpr int kSamples = 24;
            for (std::size_t i = 0; i < edges.size(); ++i) {
                auto const &e = edges[i];
                auto const *a = find_pos(e.from_node, e.from_pin);
                auto const *b = find_pos(e.to_node, e.to_pin);
                if (a == nullptr || b == nullptr)
                    continue;
                float const d = std::max(40.0f, std::abs(b->x - a->x) * 0.5f);
                ImVec2 const c1(a->x + d, a->y);
                ImVec2 const c2(b->x - d, b->y);
                for (int s = 0; s <= kSamples; ++s) {
                    float const t = static_cast<float>(s) / static_cast<float>(kSamples);
                    ImVec2 const pt = cubic_at(*a, c1, c2, *b, t);
                    float const dx = pt.x - mouse.x;
                    float const dy = pt.y - mouse.y;
                    float const d2 = dx * dx + dy * dy;
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        hovered_edge_idx = i;
                    }
                }
            }
        }
    }

    // Edge click → select (clears node selection — only one thing
    // highlighted at a time). Right-click → open context menu
    // WITHOUT changing selection; the popup reads from
    // features_context_edge instead, so dismissing the menu leaves
    // the prior selection alone (matches typical desktop UX).
    bool edge_consumed_left_click = false;
    if (hovered_edge_idx.has_value() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.features_selected_edge = edges[*hovered_edge_idx];
        state.features_selected_nodes.clear();
        edge_consumed_left_click = true;
    }
    if (hovered_edge_idx.has_value() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !was_wiring_at_start) {
        state.features_context_edge = edges[*hovered_edge_idx];
        ImGui::OpenPopup("##fectx");
    }
    if (ImGui::BeginPopup("##fectx")) {
        if (state.features_context_edge.has_value()) {
            auto const &se = *state.features_context_edge;
            auto const *fn = state.features_graph.find_node(se.from_node);
            auto const *fp = state.features_graph.find_pin(se.from_node, se.from_pin);
            auto const *tn = state.features_graph.find_node(se.to_node);
            auto const *tp = state.features_graph.find_pin(se.to_node, se.to_pin);
            char label[128];
            std::snprintf(
                label, sizeof label, "%s.%s → %s.%s",
                fn != nullptr ? (fn->label.empty() ? fn->kind.c_str() : fn->label.c_str()) : "?",
                fp != nullptr ? fp->name.c_str() : "?",
                tn != nullptr ? (tn->label.empty() ? tn->kind.c_str() : tn->label.c_str()) : "?",
                tp != nullptr ? tp->name.c_str() : "?");
            text_subtle("%s", label);
            ImGui::Separator();
            if (ImGui::MenuItem("Disconnect")) {
                pending_delete_edge = se;
            }
        }
        ImGui::EndPopup();
    }

    // Render edges. Selected and hovered states are deliberately
    // exaggerated — at 2.5 px default stroke a one-pixel difference
    // doesn't read on a moving cursor.
    for (std::size_t i = 0; i < edges.size(); ++i) {
        auto const &e = edges[i];
        auto const *p1 = find_pos(e.from_node, e.from_pin);
        auto const *p2 = find_pos(e.to_node, e.to_pin);
        if (p1 == nullptr || p2 == nullptr)
            continue;
        float const dist = std::max(40.0f, std::abs(p2->x - p1->x) * 0.5f);
        ImVec2 const c1(p1->x + dist, p1->y);
        ImVec2 const c2(p2->x - dist, p2->y);
        bool const is_sel =
            state.features_selected_edge.has_value() && edge_eq(*state.features_selected_edge, e);
        bool const is_hov = hovered_edge_idx.has_value() && *hovered_edge_idx == i;
        ImU32 const col = (is_sel || is_hov) ? edge_sel_col : edge_col;
        float const thick = is_sel ? 5.0f : (is_hov ? 4.0f : 2.5f);
        dl->AddBezierCubic(*p1, c1, c2, *p2, col, thick);
    }

    // ---- In-progress wire ------------------------------------------
    if (state.features_wiring_active) {
        auto const *from_pos =
            find_pos(state.features_wiring_from_node, state.features_wiring_from_pin);
        if (from_pos != nullptr) {
            ImVec2 const mouse = ImGui::GetIO().MousePos;
            float const dist = std::max(40.0f, std::abs(mouse.x - from_pos->x) * 0.5f);
            auto const *from = state.features_graph.find_pin(state.features_wiring_from_node,
                                                             state.features_wiring_from_pin);
            bool const from_is_output =
                from != nullptr && from->direction == st::feature::PinDirection::Output;
            ImVec2 const c1 = from_is_output ? ImVec2(from_pos->x + dist, from_pos->y)
                                             : ImVec2(from_pos->x - dist, from_pos->y);
            ImVec2 const c2 =
                from_is_output ? ImVec2(mouse.x - dist, mouse.y) : ImVec2(mouse.x + dist, mouse.y);
            dl->AddBezierCubic(*from_pos, c1, c2, mouse, edge_col, 2.0f);
        }

        // Mouse-up while wiring: try to land on a pin. Hit-test by
        // distance to the cached pin centers.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ImVec2 const mouse = ImGui::GetIO().MousePos;
            PinPos const *target = nullptr;
            for (auto const &pp : pin_positions) {
                if (pp.node_id == state.features_wiring_from_node &&
                    pp.pin_id == state.features_wiring_from_pin) {
                    continue;
                }
                float const dx = pp.pos.x - mouse.x;
                float const dy = pp.pos.y - mouse.y;
                if (dx * dx + dy * dy <= pin_hr_s * pin_hr_s) {
                    target = &pp;
                    break;
                }
            }
            if (target != nullptr) {
                auto const *from = state.features_graph.find_pin(state.features_wiring_from_node,
                                                                 state.features_wiring_from_pin);
                if (from != nullptr) {
                    // Normalize endpoints to (output → input) regardless
                    // of which pin the user grabbed first.
                    st::feature::NodeId src_n = state.features_wiring_from_node;
                    st::feature::PinId src_p = state.features_wiring_from_pin;
                    st::feature::NodeId dst_n = target->node_id;
                    st::feature::PinId dst_p = target->pin_id;
                    if (from->direction == st::feature::PinDirection::Input) {
                        std::swap(src_n, dst_n);
                        std::swap(src_p, dst_p);
                    }
                    auto r = state.features_graph.connect(src_n, src_p, dst_n, dst_p);
                    if (!r.has_value()) {
                        state.features_wire_error = r.error().to_string();
                    } else {
                        state.features_wire_error.clear();
                    }
                }
            }
            state.features_wiring_active = false;
        }
    }

    // Canvas-background hit zone — handles click-to-deselect AND
    // box-select rubber-banding. Issued AFTER every node body and
    // pin so the earlier items naturally claim their own hit
    // regions. The same InvisibleButton drives both gestures:
    // IsItemActivated → start the band; IsItemActive → drag the
    // band; IsItemDeactivated → finalize; IsItemClicked (which
    // ImGui fires for press+release with no movement) → treat as
    // empty-click deselect.
    ImGui::SetCursorScreenPos(canvas_p);
    ImGui::InvisibleButton("##fcanvas_bg", canvas_sz);
    bool const canvas_empty_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    // Rubber-band start. Suppressed while wiring so the wire-cancel
    // path stays on the empty-release branch.
    if (ImGui::IsItemActivated() && !state.features_wiring_active) {
        state.features_band_active = true;
        state.features_band_start = ImGui::GetIO().MousePos;
    }
    // Rubber-band drag — recompute selection live each frame from
    // the rectangle. Without Shift, every drag starts from a clean
    // set so the rectangle alone determines membership. With Shift,
    // the existing selection is preserved and the rectangle adds
    // to it (toggling individual nodes already works via
    // Shift+click on a body).
    if (state.features_band_active && ImGui::IsItemActive()) {
        ImVec2 const m = ImGui::GetIO().MousePos;
        float const x0 = std::min(state.features_band_start.x, m.x);
        float const x1 = std::max(state.features_band_start.x, m.x);
        float const y0 = std::min(state.features_band_start.y, m.y);
        float const y1 = std::max(state.features_band_start.y, m.y);
        bool const additive = ImGui::GetIO().KeyShift;
        // Cache the additive set on the first frame so re-sweeps
        // don't double-count already-toggled nodes. Stored as a
        // function-local static via `state` isn't needed — we
        // just rebuild from scratch each frame using the current
        // node positions. With additive=true, start from whatever
        // was selected when the band started; without, start empty.
        std::vector<st::feature::NodeId> result;
        if (additive)
            result = state.features_selected_nodes;
        for (auto const &nr : node_rects) {
            bool const intersects = !(nr.br.x < x0 || nr.tl.x > x1 || nr.br.y < y0 || nr.tl.y > y1);
            if (!intersects)
                continue;
            if (std::find(result.begin(), result.end(), nr.id) == result.end()) {
                result.push_back(nr.id);
            }
        }
        state.features_selected_nodes = std::move(result);
        state.features_selected_edge.reset();
        // Draw the band rectangle on top of nodes + edges so it
        // reads as a marquee. Filled rect for area cue, accent
        // border for the edge. Cheap to compute, redrawn every
        // frame so it tracks the cursor without lag.
        ImU32 const fill =
            ImGui::GetColorU32(ImVec4(accent.base.x, accent.base.y, accent.base.z, 0.20f));
        ImU32 const brd = ImGui::GetColorU32(accent.base);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), brd, 0.0f, 0, 1.5f);
    }
    if (state.features_band_active && ImGui::IsItemDeactivated()) {
        state.features_band_active = false;
    }

    dl->PopClipRect();

    // Apply pending mutations from context menus. Done here, after
    // every reference into `nodes` / `pin_positions` has been
    // consumed, so we never touch invalidated state.
    if (pending_default.has_value()) {
        state.features_graph.set_pin_default(pending_default->node_id, pending_default->pin_id,
                                             pending_default->value);
    }
    if (pending_delete_edge.has_value()) {
        state.features_graph.remove_edge(*pending_delete_edge);
        if (state.features_selected_edge.has_value() &&
            edge_eq(*state.features_selected_edge, *pending_delete_edge)) {
            state.features_selected_edge.reset();
        }
    }
    for (auto const id : pending_delete_nodes) {
        state.features_graph.remove_node(id);
        // If the user was about to wire FROM this node, drop the
        // wiring state so we don't reference a vanished pin.
        if (state.features_wiring_active && state.features_wiring_from_node == id) {
            state.features_wiring_active = false;
        }
        auto &sel = state.features_selected_nodes;
        sel.erase(std::remove(sel.begin(), sel.end(), id), sel.end());
        // remove_node cascades to edges touching the removed node —
        // drop the selected edge if it referenced either endpoint.
        if (state.features_selected_edge.has_value() &&
            (state.features_selected_edge->from_node == id ||
             state.features_selected_edge->to_node == id)) {
            state.features_selected_edge.reset();
        }
    }

    // Click on empty canvas → drop selection. Gated on:
    // - edge_consumed_left_click: edges aren't ImGui items; clicking
    //   a bezier still leaves the canvas-bg button "hovered", so
    //   without this gate every edge-click would be immediately
    //   followed by a deselect-on-empty.
    // - Shift: Shift+empty-click is additive (no-op), to match the
    //   Shift+band-drag and Shift+body-click semantics.
    if (canvas_empty_clicked && !edge_consumed_left_click && !ImGui::GetIO().KeyShift) {
        state.features_selected_nodes.clear();
        state.features_selected_edge.reset();
    }

    // Reserve the canvas footprint so the rest of the window scrolls
    // sensibly when more nodes are added than fit.
    ImGui::SetCursorScreenPos(canvas_p);
    ImGui::Dummy(canvas_sz);

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

    // Header: human-readable name as the title; id/dim/address/category
    // tucked into a subtitle. Chips on the right of the title carry the
    // unit and the safety/emissions flags so they catch the eye without
    // shouting.
    bool const have_name = (tbl != nullptr && !tbl->name.empty());
    char const *title = have_name ? tbl->name.c_str() : state.selected_table_id.c_str();

    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.0f);

    // Place a chip on the heading row, wrapping to a new line when it
    // would overflow the panel's right edge. Without this, narrow
    // window widths push chips off-screen instead of stacking them.
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
            text_subtle("%s  \xC2\xB7  %dD  \xC2\xB7  0x%08zX  \xC2\xB7  %s",
                        state.selected_table_id.c_str(), tbl->dimensions, tbl->address,
                        tbl->category.c_str());
        } else if (have_name) {
            text_subtle("%s  \xC2\xB7  %dD  \xC2\xB7  0x%08zX", state.selected_table_id.c_str(),
                        tbl->dimensions, tbl->address);
        } else if (!tbl->category.empty()) {
            text_subtle("%dD  \xC2\xB7  0x%08zX  \xC2\xB7  %s", tbl->dimensions, tbl->address,
                        tbl->category.c_str());
        } else {
            text_subtle("%dD  \xC2\xB7  0x%08zX", tbl->dimensions, tbl->address);
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
        // Table-wide stats are noise when the user has a selection — the
        // selection-summary line below covers the same shape for the
        // chosen scope. Hide the table-wide text in that case; keep the
        // scale legend, which describes the whole-table heatmap mapping
        // and is useful regardless of selection.
        bool const show_table_stats = !state.selection.enabled;
        if (show_table_stats) {
            text_subtle("min %.*f  ·  max %.*f  ·  mean %.*f  ·  %zu cells", precision, stats.min,
                        precision, stats.max, precision, stats.mean, stats.count);
        }
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
            if (show_table_stats) {
                ImGui::SameLine(0.0f, 24.0f);
            }
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
            ImGui::SetTooltip("Write the working ROM + edits to disk.  (Ctrl+S)");
        } else {
            ImGui::SetTooltip("No unsaved edits.  (Ctrl+S)");
        }
    }
    ImGui::EndDisabled();

    // Flash button — opens the policy-gate modal. Enabled whenever there's
    // a project loaded; the modal itself handles the "nothing to flash"
    // and "blocked by policy" branches.
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.project.has_value());
    if (ImGui::Button("Flash...")) {
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
constexpr double kStatusMsgTtlSeconds = 5.0;

// Mirror a status/error string to stderr the moment its value changes,
// so the console window that opens alongside the GUI captures every
// user-visible status/error as a copy-pasteable line. Heuristic:
// classify as `[err]` when the text matches an error-flavored regex
// (Failed / Error / cannot / Invalid / missing / Reject — same set
// other error sites use), otherwise `[status]`. Both classifications
// land on stderr so the console captures the full timeline.
//
// Called from tick_status_msg() once per frame — cheap because the
// fprintf only fires when `current` differs from the shadow `prev`.
void mirror_status_change(std::string const &current, std::string &prev,
                          char const *surface_label) {
    if (current == prev) {
        return;
    }
    prev = current;
    if (current.empty()) {
        return;
    }
    auto const looks_like_error = [&](std::string const &s) noexcept {
        constexpr std::string_view kErrorMarkers[] = {
            "Failed", "failed", "Error", "error", "cannot", "Cannot",
            "Invalid", "missing", "Reject", "reject",
        };
        for (auto const &m : kErrorMarkers) {
            if (s.find(m) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    char const *severity = looks_like_error(current) ? "err" : "status";
    std::fprintf(stderr, "[%s][%s] %s\n", severity, surface_label, current.c_str());
    std::fflush(stderr);
}

void tick_status_msg(AppState &state) {
    double const now = ImGui::GetTime();
    // Stderr-mirror every surface that carries a user-visible status
    // string, BEFORE the TTL pass — so the console captures the
    // message even if it's about to be auto-cleared this frame.
    // Both `status_msg` (the status bar) and `read_rom_error_msg`
    // (the Tools → Read ROM from Car modal) get the same treatment;
    // adding more surfaces is one line per new shadow field.
    bool const status_changed = state.status_msg != state.status_msg_prev;
    mirror_status_change(state.status_msg, state.status_msg_prev, "status-bar");
    mirror_status_change(state.read_rom_error_msg, state.read_rom_error_msg_prev, "read-rom");
    if (status_changed) {
        state.status_msg_seen_at = now;
        return;
    }
    if (!state.status_msg.empty() && now - state.status_msg_seen_at >= kStatusMsgTtlSeconds) {
        state.status_msg.clear();
        state.status_msg_prev.clear();
    }
}

// Toast tuning. Lifetime + fade kept short so toasts feel responsive
// (a 4-second window is long enough to read a one-line message, short
// enough not to cover the workspace). Max stack caps a burst of
// notifications from tiling the screen — oldest drop off first.
constexpr std::chrono::milliseconds kToastLifetime{4000};
constexpr std::chrono::milliseconds kToastFadeWindow{500};
constexpr std::size_t kToastMaxStack = 5;
constexpr float kToastWidth = 320.0f;
constexpr float kToastVerticalGap = 8.0f;

void enqueue_toast(AppState &state, ToastKind kind, std::string text) {
    state.toasts.push_back({
        std::move(text),
        kind,
        std::chrono::steady_clock::now() + kToastLifetime,
    });
    // Drop oldest if we've exceeded the visible cap.
    if (state.toasts.size() > kToastMaxStack) {
        state.toasts.erase(state.toasts.begin());
    }
}

// Toast colors by kind — composes the existing theme-aware chip
// palette so toasts inherit the same dark/light contrast story.
inline ImVec4 toast_fg(ToastKind k) {
    switch (k) {
    case ToastKind::Success:
        return chip_fg_ok();
    case ToastKind::Warn:
        return chip_fg_warn();
    case ToastKind::Danger:
        return chip_fg_danger();
    case ToastKind::Info:
    default:
        return chip_fg_info();
    }
}

inline ImVec4 toast_bg(ToastKind k) {
    switch (k) {
    case ToastKind::Success:
        return chip_bg_ok();
    case ToastKind::Warn:
        return chip_bg_warn();
    case ToastKind::Danger:
        return chip_bg_danger();
    case ToastKind::Info:
    default:
        return chip_bg_info();
    }
}

void render_toasts(AppState &state) {
    auto const now = std::chrono::steady_clock::now();

    // GC expired toasts first so we don't waste a frame's worth of
    // window setup on something we're about to drop.
    std::erase_if(state.toasts,
                  [&](Toast const &t) { return t.expires_at <= now; });
    if (state.toasts.empty()) {
        return;
    }

    auto const *const vp = ImGui::GetMainViewport();
    float const right_x = vp->WorkPos.x + vp->WorkSize.x;
    float const bottom_y =
        vp->WorkPos.y + vp->WorkSize.y - static_cast<float>(kStatusBarHeight);

    // Stack newest-at-bottom (closest to the status bar) and grow
    // upward. anchor=(0,1) means SetNextWindowPos specifies the
    // bottom-LEFT corner of the toast.
    float y_cursor = bottom_y - kToastVerticalGap;

    for (std::size_t i = 0; i < state.toasts.size(); ++i) {
        // Iterate from end (newest) backward to front (oldest).
        auto const &t = state.toasts[state.toasts.size() - 1 - i];

        // Compute fade alpha — linear ramp during the last
        // kToastFadeWindow before expiry.
        auto const remaining = t.expires_at - now;
        float alpha = 1.0f;
        if (remaining < kToastFadeWindow) {
            float const num = std::chrono::duration<float>(remaining).count();
            float const den = std::chrono::duration<float>(kToastFadeWindow).count();
            alpha = std::clamp(num / den, 0.0f, 1.0f);
        }

        // Estimate height from wrapped-text size so the bottom anchor
        // lands consistently on the very first frame (otherwise
        // ImGui's auto-resize would briefly render at zero size).
        // 24px = 2*WindowPadding.y; the chip-style frame padding is
        // implicit in the WindowPadding chosen by the active style.
        ImVec2 const text_sz = ImGui::CalcTextSize(
            t.text.c_str(), nullptr, /*hide_text_after_hash=*/false,
            kToastWidth - 24.0f);
        float const toast_h = text_sz.y + 20.0f;

        ImGui::SetNextWindowPos(
            ImVec2(right_x - kToastWidth - kToastVerticalGap, y_cursor),
            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(kToastWidth, toast_h), ImGuiCond_Always);

        ImVec4 const bg = toast_bg(t.kind);
        ImVec4 const fg = toast_fg(t.kind);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.x, bg.y, bg.z, bg.w * alpha));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(fg.x, fg.y, fg.z, fg.w * alpha));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        char wid[32];
        std::snprintf(wid, sizeof wid, "##toast_%zu", i);
        ImGui::Begin(wid, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoDocking);
        ImGui::TextWrapped("%s", t.text.c_str());
        ImGui::End();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        y_cursor -= toast_h + kToastVerticalGap;
    }
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
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
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
            ImGui::SetTooltip("%s\nPack: %s", state.project->dir().string().c_str(),
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
            // After a successful save this session, swap the generic
            // "Clean" reading for "Saved <relative time>" — at-a-glance
            // freshness. Before the first save (project just opened),
            // fall back to "Clean" since the project on disk is still
            // saved, we just don't have a session-local timestamp.
            if (state.last_save_iso.has_value()) {
                auto const rel = format_relative_time(*state.last_save_iso);
                char buf[48];
                if (rel.empty()) {
                    std::snprintf(buf, sizeof buf, "Saved");
                } else {
                    std::snprintf(buf, sizeof buf, "Saved %s", rel.c_str());
                }
                chip(buf, chip_fg_muted(), chip_bg_muted());
                if (ImGui::IsItemHovered()) {
                    // Human-friendly absolute timestamp — strip the
                    // ISO 'T' separator and trailing 'Z', append "UTC".
                    // "2026-05-29T14:23:51Z" → "2026-05-29 14:23:51 UTC".
                    std::string when = *state.last_save_iso;
                    if (auto const t = when.find('T'); t != std::string::npos) {
                        when[t] = ' ';
                    }
                    if (!when.empty() && when.back() == 'Z') {
                        when.pop_back();
                    }
                    ImGui::SetTooltip("Last save: %s UTC.\nAll edits are on disk.",
                                      when.c_str());
                }
            } else {
                chip("Clean", chip_fg_muted(), chip_bg_muted());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("All edits are saved to disk.");
                }
            }
        }

        // Jurisdiction profile chip — disabled-muted in motorsport-only
        // (the default, silent gate), accent for any other profile so the
        // user notices when they're under a real regulatory posture. Click
        // to open a chooser. See docs/06-legal-ethics.md.
        ImGui::SameLine();
        {
            auto const profile = state.project->policy_profile();
            auto const profile_str = std::string{st::policy::profile_name(profile)};
            bool const is_default = profile == st::policy::Profile::MotorsportOnly;
            // Appended "▾" makes the chip read as a menu trigger
            // without needing a hover. The chip helper renders text
            // verbatim, so the glyph is part of the label.
            auto const chip_label = profile_str + "  \xE2\x96\xBE";
            if (is_default) {
                chip(chip_label.c_str(), chip_fg_muted(), chip_bg_muted());
            } else {
                chip(chip_label.c_str(), chip_fg_accent(), chip_bg_accent());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Active jurisdiction profile (project.toml).\n"
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
                text_subtle("Jurisdiction profile (this project)");
                ImGui::Separator();
                auto pick = [&](st::policy::Profile p, char const *label, char const *desc) {
                    bool const is_current = (profile == p);
                    bool const is_saved_default = (state.settings.default_policy_profile == p);
                    char row[64];
                    if (is_saved_default) {
                        std::snprintf(row, sizeof row, "%s  (default)", label);
                    } else {
                        std::snprintf(row, sizeof row, "%s", label);
                    }
                    if (ImGui::Selectable(row, is_current)) {
                        state.project->set_policy_profile(p);
                        if (auto s = state.project->save_metadata(); !s.has_value()) {
                            state.status_msg = "Profile save failed: " + s.error().to_string();
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
                if (ImGui::MenuItem("Save current as default for new projects")) {
                    state.settings.default_policy_profile = profile;
                    save_settings(state.settings);
                    state.status_msg = std::string{"Default profile: "} +
                                       std::string{st::policy::profile_name(profile)};
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Persist the project's current profile in\n"
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
                std::size_t disabled = 0;
                std::size_t emissions_off = 0;
                for (auto const &d : def.dtcs()) {
                    auto const *bm = def.find_dtc_bitmap(d.bitmap_id);
                    if (bm == nullptr)
                        continue;
                    auto const en = st::is_dtc_enabled(rom, *bm, d);
                    if (en.has_value() && !*en) {
                        ++disabled;
                        if (d.emissions_relevant)
                            ++emissions_off;
                    }
                }
                if (disabled > 0) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%zu DTC off", disabled);
                    ImGui::SameLine();
                    chip(buf, chip_fg_muted(), chip_bg_muted());
                    if (ImGui::IsItemHovered()) {
                        if (emissions_off > 0) {
                            ImGui::SetTooltip("%zu DTC(s) disabled in this working ROM\n"
                                              "(%zu emissions-flagged).\n"
                                              "See the DTCs panel to toggle.",
                                              disabled, emissions_off);
                        } else {
                            ImGui::SetTooltip("%zu DTC(s) disabled in this working ROM.\n"
                                              "See the DTCs panel to toggle.",
                                              disabled);
                        }
                    }
                }
            }
        }

        ImGui::SameLine();
        text_subtle("edits %zu / %zu", state.project->history().cursor(),
                    state.project->history().size());

        // Middle cluster: transient status message. save_project sets
        // this to "Saved."; edit-op errors land here too. Previously
        // the with-project branch never showed it, so Ctrl+S
        // succeeded silently — now it gives the user feedback.
        if (!state.status_msg.empty()) {
            ImGui::SameLine(0.0f, 24.0f);
            text_subtle("\xE2\x80\x94 %s", state.status_msg.c_str());
        }

        // Right cluster: source / working CRCs, right-aligned. Compute the
        // text width up front so we can place the cursor cleanly.
        char crc_buf[80];
        std::snprintf(crc_buf, sizeof(crc_buf), "source 0x%08X  \xC2\xB7  working 0x%08X",
                      state.project->source_crc32_at_create(),
                      state.project->working_rom().crc32());
        float const crc_w = ImGui::CalcTextSize(crc_buf).x;
        float const right_x =
            ImGui::GetWindowContentRegionMax().x - crc_w - ImGui::GetStyle().FramePadding.x;
        if (right_x > ImGui::GetCursorPosX()) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_x);
            text_subtle("%s", crc_buf);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("CRC32 of the source ROM (immutable) and the current\n"
                                  "working ROM. Any change in working bytes shifts the CRC.");
            }
        }
    } else {
        text_subtle("No project loaded. %s",
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

        tick_status_msg(state);
        render_menubar(state);
        render_dockspace_host();
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
