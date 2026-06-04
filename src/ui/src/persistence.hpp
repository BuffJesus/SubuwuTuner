// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Recents + settings + config-dir paths. One-line-per-entry plain-text
// persistence next to imgui.ini. Decisions sit in main.cpp's anon
// namespace today; this header is the seam future per-file moves use.

#ifndef ST_UI_PERSISTENCE_HPP
#define ST_UI_PERSISTENCE_HPP

#include "st/policy.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

struct RecentEntry {
    std::string opened_at; // ISO 8601 UTC, e.g. "2026-05-12T15:30:00Z"
    std::filesystem::path path;
    // Pin/star (UX polish backlog from 2026-06-02 handoff). When true
    // the welcome panel renders the entry at the top of the list with
    // a ★ glyph and survives the kRecentsCap LRU eviction. Persisted
    // to disk alongside the other fields.
    bool pinned{false};
};

inline constexpr std::size_t kRecentsCap = 8;

enum class Theme { Dark, Light };

char const *theme_name(Theme t) noexcept;
std::optional<Theme> parse_theme(std::string_view s) noexcept;

struct Settings {
    st::policy::Profile default_policy_profile{st::policy::Profile::MotorsportOnly};
    Theme theme{Theme::Dark};
    // Set true once the first-run wizard (Help → Welcome wizard, or
    // automatic on first launch) has been completed. False on fresh
    // install triggers the wizard. `subuwutuner-gui --reset-config`
    // flips this back to false on disk.
    bool first_run_complete{false};
    // Active vehicle profile id (analyst Issue #7). Points into the
    // `.stprofile` files under st::profile::default_profile_dir().
    // Empty when no profile has been picked. The GUI uses this to
    // surface vehicle context in modals (Flash, Read ROM) and to
    // default transport selection. Profile creation/editing happens
    // via the CLI (`subuwutuner-cli profile ...`) in v1; the GUI just
    // selects from the dir.
    std::string active_vehicle_profile_id;
};

std::filesystem::path config_dir_root();
std::filesystem::path recents_config_path();
std::filesystem::path settings_config_path();

std::optional<std::filesystem::path> resolve_demo_project_path(char const *argv0);

// Locate the project's docs/ directory relative to argv[0]. Same
// candidate ladder as resolve_demo_project_path — dev tree, sibling
// install, flat install — but anchored to docs/00-overview.md. Returns
// nullopt when no candidate contains overview.md.
std::optional<std::filesystem::path> resolve_docs_dir(char const *argv0);

// Locate CHANGELOG.md alongside the binary. Same candidate ladder.
// Returns nullopt when no candidate dir contains a CHANGELOG.md.
std::optional<std::filesystem::path> resolve_changelog_path(char const *argv0);

std::string iso8601_utc_now();
std::string format_relative_time(std::string const &iso);

std::vector<RecentEntry> load_recents();
void save_recents(std::vector<RecentEntry> const &recents);
void push_recent(std::vector<RecentEntry> &recents, std::filesystem::path const &path);

// Sidebar category-ordering persistence (handoff §UX-polish backlog).
// Stored at <project_dir>/sidebar_order.txt as one category name per
// line. Absent file = empty vector = pack default. Save is best-
// effort; disk failure loses the cross-session preference but never
// the live runtime ordering.
std::vector<std::string> load_sidebar_category_order(std::filesystem::path const &project_dir);
void save_sidebar_category_order(std::filesystem::path const &project_dir,
                                 std::vector<std::string> const &order);

Settings load_settings();
void save_settings(Settings const &s);

} // namespace st::ui

#endif
