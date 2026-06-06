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
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // On-disk format: `<opened_at>\t<path>` for legacy entries OR
        // `<opened_at>\t<path>\t<flags>` for new pin-aware entries.
        // The trailing field is a comma-separated bag of name=value
        // pairs (just `pinned=1` for now). Unknown keys are ignored so
        // we can add more fields later without a schema bump.
        auto const tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) {
            continue;
        }
        auto const second_tab = line.find('\t', tab + 1);
        RecentEntry e;
        e.opened_at = line.substr(0, tab);
        if (second_tab == std::string::npos) {
            e.path = std::filesystem::path{line.substr(tab + 1)};
        } else {
            e.path = std::filesystem::path{line.substr(tab + 1, second_tab - tab - 1)};
            auto const flags = std::string_view{line}.substr(second_tab + 1);
            std::size_t pos = 0;
            while (pos < flags.size()) {
                auto const comma = flags.find(',', pos);
                auto const tok = flags.substr(pos, comma - pos);
                if (tok == "pinned=1") {
                    e.pinned = true;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
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
        // Forward-compat note: builds older than 2026-06-03 read a
        // 2-field format (opened_at\tpath). The 3rd field below makes
        // those older builds parse `path\tpinned=1` as the entire path,
        // showing a missing entry. Users downgrading delete recents.txt
        // to recover. The format is one-way upgrade-safe; load_recents
        // above accepts both shapes for the forward path.
        out << e.opened_at << '\t' << e.path.generic_string();
        if (e.pinned) {
            out << '\t' << "pinned=1";
        }
        out << '\n';
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

char const *ai_provider_name(AiProvider p) noexcept {
    switch (p) {
    case AiProvider::Anthropic:
        return "anthropic";
    case AiProvider::OpenAI:
        return "openai";
    }
    return "anthropic";
}

std::optional<AiProvider> parse_ai_provider(std::string_view s) noexcept {
    if (s == "anthropic" || s == "claude")
        return AiProvider::Anthropic;
    if (s == "openai" || s == "gpt")
        return AiProvider::OpenAI;
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
        } else if (key == "help_active_topic_id") {
            s.help_active_topic_id = std::string{val};
        } else if (key == "ai_narration_enabled") {
            s.ai_narration_enabled = (val == "true" || val == "1");
        } else if (key == "ai_provider") {
            if (auto p = parse_ai_provider(val); p.has_value()) {
                s.ai_provider = *p;
            }
        } else if (key == "ai_api_key") {
            s.ai_api_key = std::string{val};
        } else if (key == "ai_model") {
            s.ai_model = std::string{val};
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
    out << "help_active_topic_id=" << s.help_active_topic_id << '\n';
    out << "ai_narration_enabled=" << (s.ai_narration_enabled ? "true" : "false") << '\n';
    out << "ai_provider=" << ai_provider_name(s.ai_provider) << '\n';
    out << "ai_api_key=" << s.ai_api_key << '\n';
    out << "ai_model=" << s.ai_model << '\n';
}

// Move `path` to the front of `recents`, deduplicating by canonical
// path comparison and capping the list at `kRecentsCap`. Idempotent.
void push_recent(std::vector<RecentEntry> &recents, std::filesystem::path const &path) {
    std::error_code ec;
    auto const canon = std::filesystem::weakly_canonical(path, ec);
    auto const compare_to = canon.empty() ? path : canon;
    // Preserve the pinned flag from any existing entry pointing at the
    // same path — `push_recent` is called every time the user opens a
    // project, and we don't want to silently strip pin state.
    bool was_pinned = false;
    for (auto const &e : recents) {
        std::error_code ec2;
        auto const ec_path = std::filesystem::weakly_canonical(e.path, ec2);
        if ((ec_path.empty() ? e.path : ec_path) == compare_to && e.pinned) {
            was_pinned = true;
            break;
        }
    }
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
    e.pinned = was_pinned;
    recents.insert(recents.begin(), std::move(e));
    // Cap the list, but never evict pinned entries — they survive the
    // kRecentsCap LRU. Unpinned-only LIFO trim: keep walking from the
    // back, drop the first unpinned entry past the cap.
    while (recents.size() > kRecentsCap) {
        auto it = std::find_if(recents.rbegin(), recents.rend(),
                               [](RecentEntry const &r) { return !r.pinned; });
        if (it == recents.rend()) {
            break; // every entry is pinned — leave the list as-is
        }
        // Erase reverse_iterator → forward iterator dance.
        recents.erase(std::next(it).base());
    }
}

std::vector<std::string> load_sidebar_category_order(std::filesystem::path const &project_dir) {
    std::vector<std::string> out;
    auto const path = project_dir / "sidebar_order.txt";
    std::ifstream in{path};
    if (!in)
        return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        out.push_back(std::move(line));
    }
    return out;
}

void save_sidebar_category_order(std::filesystem::path const &project_dir,
                                 std::vector<std::string> const &order) {
    auto const path = project_dir / "sidebar_order.txt";
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    for (auto const &cat : order) {
        out << cat << '\n';
    }
}

std::vector<std::string>
load_sidebar_hidden_categories(std::filesystem::path const &project_dir) {
    std::vector<std::string> out;
    auto const path = project_dir / "sidebar_hidden.txt";
    std::ifstream in{path};
    if (!in)
        return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        out.push_back(std::move(line));
    }
    return out;
}

void save_sidebar_hidden_categories(std::filesystem::path const &project_dir,
                                    std::vector<std::string> const &hidden) {
    auto const path = project_dir / "sidebar_hidden.txt";
    if (hidden.empty()) {
        // Empty list = "no hidden categories"; remove the file so the
        // project root stays tidy and the loader's absent-file path is
        // the canonical "all visible" representation.
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    for (auto const &cat : hidden) {
        out << cat << '\n';
    }
}

std::vector<std::string>
load_compare_pinned(std::filesystem::path const &project_dir) {
    std::vector<std::string> out;
    auto const path = project_dir / "compare.pinned";
    std::ifstream in{path};
    if (!in)
        return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        out.push_back(std::move(line));
    }
    return out;
}

void save_compare_pinned(std::filesystem::path const &project_dir,
                         std::vector<std::string> const &pinned) {
    auto const path = project_dir / "compare.pinned";
    if (pinned.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    for (auto const &id : pinned) {
        out << id << '\n';
    }
}

std::optional<CompareConfig>
load_compare_config(std::filesystem::path const &project_dir) {
    auto const path = project_dir / "compare.config";
    std::ifstream in{path};
    if (!in)
        return std::nullopt;
    CompareConfig cfg;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto const eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string_view const key{line.data(), eq};
        std::string_view const val{line.data() + eq + 1, line.size() - eq - 1};
        if (key == "rom_a_path") {
            cfg.rom_a_path = std::string{val};
        } else if (key == "rom_b_path") {
            cfg.rom_b_path = std::string{val};
        } else if (key == "epsilon") {
            try {
                cfg.epsilon = std::stof(std::string{val});
            } catch (...) {
                // malformed float — keep default, don't fail the load
            }
        } else if (key == "include_identical") {
            cfg.include_identical = (val == "true" || val == "1");
        } else if (key == "filter_chip") {
            cfg.filter_chip = std::string{val};
        }
        // Unknown keys silently ignored so newer builds can read older
        // files without forcing a migration.
    }
    return cfg;
}

void save_compare_config(std::filesystem::path const &project_dir,
                         CompareConfig const &cfg) {
    auto const path = project_dir / "compare.config";
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    out << "rom_a_path=" << cfg.rom_a_path << '\n';
    out << "rom_b_path=" << cfg.rom_b_path << '\n';
    out << "epsilon=" << cfg.epsilon << '\n';
    out << "include_identical=" << (cfg.include_identical ? "true" : "false") << '\n';
    out << "filter_chip=" << cfg.filter_chip << '\n';
}

// pack-lint.toml format. Hand-rolled rather than going through
// tomlplusplus — three string fields + one int + an optional
// multi-line body. Toml-quoting a multi-line message would mean
// reinventing escape rules; instead the body lives below a sentinel
// line so the parser can read it verbatim. Anything we don't
// understand decays to "not yet validated" so a malformed file is
// indistinguishable from a missing one.
//
// Schema:
//   status              = 0                 # 0 = ok, >0 = violation count
//   pack_id             = "demo-pack"
//   last_validated_at   = "2026-06-04T18:23:11Z"
//   # The message body, when present, follows below the literal
//   # line `--- message ---` and consumes the rest of the file.
namespace {
constexpr char const *kPackLintFilename = "pack-lint.toml";
constexpr char const *kMessageMarker = "--- message ---";

std::string strip_quotes(std::string_view v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v.remove_prefix(1);
        v.remove_suffix(1);
    }
    return std::string{v};
}

std::string_view trim(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.remove_suffix(1);
    return v;
}
} // namespace

std::optional<PackLintSnapshot>
load_pack_lint(std::filesystem::path const &project_dir) {
    auto const path = project_dir / kPackLintFilename;
    std::ifstream in{path};
    if (!in)
        return std::nullopt;

    PackLintSnapshot snap;
    bool reading_message = false;
    std::string line;
    std::string body;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (reading_message) {
            if (!body.empty())
                body.push_back('\n');
            body.append(line);
            continue;
        }
        if (line == kMessageMarker) {
            reading_message = true;
            continue;
        }
        auto const eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string const key = std::string{trim(std::string_view{line}.substr(0, eq))};
        std::string const val = std::string{trim(std::string_view{line}.substr(eq + 1))};
        if (key == "status") {
            try {
                snap.status = std::stoi(val);
            } catch (...) {
                return std::nullopt;
            }
        } else if (key == "pack_id") {
            snap.pack_id = strip_quotes(val);
        } else if (key == "last_validated_at") {
            snap.last_validated_at = strip_quotes(val);
        }
    }
    snap.message = std::move(body);
    if (snap.status < 0) {
        // status is the only field we *require* to be meaningful; an
        // unparseable status means the file isn't a snapshot we wrote.
        return std::nullopt;
    }
    return snap;
}

void save_pack_lint(std::filesystem::path const &project_dir,
                    PackLintSnapshot const &snap) {
    auto const path = project_dir / kPackLintFilename;
    std::ofstream out{path, std::ios::trunc};
    if (!out)
        return;
    out << "# SubuwuTuner pack-lint snapshot — written by\n";
    out << "# Settings → Validate pack. Safe to delete (loses the\n";
    out << "# cached status, never the project itself).\n";
    out << "status            = " << snap.status << '\n';
    out << "pack_id           = \"" << snap.pack_id << "\"\n";
    out << "last_validated_at = \"" << snap.last_validated_at << "\"\n";
    if (!snap.message.empty()) {
        out << kMessageMarker << '\n';
        out << snap.message;
        if (snap.message.back() != '\n') {
            out << '\n';
        }
    }
}

} // namespace st::ui
