// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Config-dir + recents + settings persistence. Plain-text formats
// (one-line-per-entry recents.txt, key=value settings.txt) next to
// the OS-conventional user-config dir. Unknown settings keys are
// silently ignored on load so newer builds can read older files.

#include "persistence.hpp"

#include "st/policy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace st::ui {

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

std::optional<std::filesystem::path>
resolve_docs_dir(char const *argv0) {
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
        exe_dir / ".." / ".." / ".." / "docs",
        exe_dir / ".." / "docs",
        exe_dir / "docs",
    };
    for (auto const &c : candidates) {
        std::error_code dir_ec;
        if (!fs::is_directory(c, dir_ec) || dir_ec) {
            continue;
        }
        std::error_code file_ec;
        if (!fs::exists(c / "00-overview.md", file_ec) || file_ec) {
            continue;
        }
        std::error_code canon_ec;
        auto canon = fs::weakly_canonical(c, canon_ec);
        return canon_ec ? c : canon;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path>
resolve_changelog_path(char const *argv0) {
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
        exe_dir / ".." / ".." / ".." / "CHANGELOG.md",
        exe_dir / ".." / "CHANGELOG.md",
        exe_dir / "CHANGELOG.md",
    };
    for (auto const &c : candidates) {
        std::error_code file_ec;
        if (!fs::exists(c, file_ec) || file_ec) {
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

// User-preferences persistence. Stored next to recents.txt as a
// `key=value\n` plain-text file (one setting per line). New settings
// land without breaking older builds — unknown keys are silently
// ignored on load. Currently:
//   default_policy_profile = motorsport-only|alberta-ca|eu-roadworthy|california-us
//   theme                  = dark|light
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
        } else if (key == "first_run_complete") {
            s.first_run_complete = (val == "true" || val == "1");
        } else if (key == "active_vehicle_profile_id") {
            s.active_vehicle_profile_id = std::string{val};
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
    out << "first_run_complete=" << (s.first_run_complete ? "true" : "false") << '\n';
    out << "active_vehicle_profile_id=" << s.active_vehicle_profile_id << '\n';
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

} // namespace st::ui
