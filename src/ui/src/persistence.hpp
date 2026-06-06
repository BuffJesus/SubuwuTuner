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

// AI narration backend (docs/20 Tier 2). Two cloud providers in v1 —
// Ollama-local stays a v2.1+ item per the doc. User supplies the API
// key; storage is plaintext settings.txt (local-user trust model,
// same threat surface as recents.txt). Off by default; the GUI shows
// a Settings → AI tab toggle with provider radio + key field +
// model name.
enum class AiProvider { Anthropic, OpenAI };
char const *ai_provider_name(AiProvider p) noexcept;
std::optional<AiProvider> parse_ai_provider(std::string_view s) noexcept;

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
    // Last-active help-modal topic id (TopicSource::filename, e.g.
    // "10-glossary.md"). Restored on the first help-modal open of a
    // session so the user re-finds whatever they were reading. F1
    // per-panel routing still overrides this — context wins over
    // memory. Empty = no prior session, fall through to topic 0.
    std::string help_active_topic_id;
    // AI Tier 2 narration (docs/20). Opt-in; off by default. When
    // enabled, the engine call site (next sprint) routes through
    // ai_provider with ai_api_key. ai_model is the per-provider
    // model identifier — empty means use the provider's default
    // (claude-opus-4-7 for Anthropic, gpt-4o for OpenAI). Plaintext
    // settings.txt storage per user direction; local-user trust
    // model — same threat surface as other user-supplied secrets
    // already in this file.
    bool        ai_narration_enabled{false};
    AiProvider  ai_provider{AiProvider::Anthropic};
    std::string ai_api_key;
    std::string ai_model;
};

std::filesystem::path config_dir_root();
std::filesystem::path recents_config_path();
std::filesystem::path settings_config_path();

// Cross-session per-CID audit sink directory (analyst Issue #8). Lives
// at `<config>/audit-by-cid/`; each file inside is
// `<sanitized_cid>.log`. AuditLog::set_per_cid_sink(this) tees every
// project's appended entry into the matching CID file so a tech can
// pull the full cross-project history for one car.
std::filesystem::path audit_by_cid_dir();

// Resolve the CID + VIN for the active VehicleProfile from
// `Settings.active_vehicle_profile_id`. Returns empty strings when no
// profile is active, the .stprofile file can't be loaded, or the
// profile has no engine ECU recorded. Reads the .stprofile each call
// — typically called once at project-open / profile-switch, so the
// disk-read cost is negligible.
struct ActiveVehicleIdentity {
    std::string cid;
    std::string vin;
};
ActiveVehicleIdentity resolve_active_vehicle_identity(Settings const &settings);

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

// Sidebar per-category visibility (handoff §UX-polish backlog —
// caps the long tail of rarely-touched categories on packs that
// expose a huge surface). Stored at <project_dir>/sidebar_hidden.txt
// as one category name per line. Absent file = empty vector = all
// categories visible. Save is best-effort and runs every time the
// user toggles a category via the sidebar context menu.
std::vector<std::string> load_sidebar_hidden_categories(std::filesystem::path const &project_dir);
void save_sidebar_hidden_categories(std::filesystem::path const &project_dir,
                                    std::vector<std::string> const &hidden);

// Compare panel pinned-table set. Persisted at
// <project_dir>/compare.pinned, one table id per line. Mirrors the
// audit.pinned sidecar pattern (cb921ef / 2a84616). Survives across
// sessions so a user can star tables relevant to a tuning question,
// hand the .stune to an e-tuner, and the star markers travel with
// the project.
std::vector<std::string> load_compare_pinned(std::filesystem::path const &project_dir);
void save_compare_pinned(std::filesystem::path const &project_dir,
                         std::vector<std::string> const &pinned);

// Compare panel last-comparison setup. Persisted at
// <project_dir>/compare.config as `key=value` lines (same shape
// settings.txt uses). Restored on project open so the user picks
// up where they left off — Compare's ROM A/B + epsilon +
// include_identical + filter chip survive across sessions. Empty
// optional = no saved state yet (fresh project / never compared).
struct CompareConfig {
    std::string rom_a_path;        // path string OR sentinel "<project working>"
    std::string rom_b_path;
    float       epsilon{0.0f};
    bool        include_identical{false};
    std::string filter_chip;       // "@safety" / "@emissions" / "@flagged" /
                                   // a category name / empty for "All"
};
std::optional<CompareConfig>
load_compare_config(std::filesystem::path const &project_dir);
void save_compare_config(std::filesystem::path const &project_dir,
                         CompareConfig const &cfg);

// Per-project pack-validation snapshot. Settings → "Validate pack"
// writes this so the next session can render the chip without re-
// running the validator, and so the welcome panel can surface a
// per-recent "Pack ✓ <date>" / "Pack ✗ N issues" hint without the
// user opening anything. Stored at <project_dir>/pack-lint.toml.
// Missing file = "not yet validated" (status == -1). All fields
// are best-effort: a malformed file decays to "not yet validated"
// rather than failing the project open.
struct PackLintSnapshot {
    int status{-1};            // -1 = not validated, 0 = ok, >0 = violation count
    std::string pack_id;       // the pack id the snapshot was recorded against
    std::string last_validated_at; // ISO 8601 UTC; empty on a never-validated project
    std::string message;       // violation body, empty on ok / unvalidated
};

std::optional<PackLintSnapshot>
load_pack_lint(std::filesystem::path const &project_dir);
void save_pack_lint(std::filesystem::path const &project_dir,
                    PackLintSnapshot const &snap);

Settings load_settings();
void save_settings(Settings const &s);

} // namespace st::ui

#endif
