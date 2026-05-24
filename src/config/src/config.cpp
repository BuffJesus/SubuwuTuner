// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/config.hpp"

#include "st/core/error.hpp"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif

namespace st::config {

namespace {

// Process-wide overrides set by override_*(); applied on top of the
// cached Config by the free-function facade.
struct Overrides {
    std::optional<std::filesystem::path> definitions_root;
    std::optional<std::filesystem::path> rom_dump_root;
};

Overrides &overrides_state() {
    static Overrides s;
    return s;
}

std::optional<Config> &cached_state() {
    static std::optional<Config> s;
    return s;
}

[[nodiscard]] std::optional<std::string> env_var(char const *name) {
    char const *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return std::nullopt;
    }
    return std::string{v};
}

[[nodiscard]] std::filesystem::path user_home() {
#ifdef _WIN32
    if (auto p = env_var("USERPROFILE")) {
        return std::filesystem::path{*p};
    }
    if (auto p = env_var("HOME")) {
        return std::filesystem::path{*p};
    }
#else
    if (auto p = env_var("HOME")) {
        return std::filesystem::path{*p};
    }
#endif
    return {};
}

[[nodiscard]] std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[32768]; // MAX_PATH is too small for long paths on modern Windows.
    DWORD const n = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
    if (n == 0 || n == sizeof(buf) / sizeof(buf[0])) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }
    return std::filesystem::path{std::wstring{buf, n}}.parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n) != 0) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }
    return std::filesystem::path{buf}.parent_path();
#else
    char buf[4096];
    ssize_t const n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }
    buf[n] = '\0';
    return std::filesystem::path{buf}.parent_path();
#endif
}

// Strip leading + trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// Parse a `"..."` string value with backslash escapes (\\, \", \n, \r, \t).
// Returns the unquoted decoded string on success or nullopt on syntax error.
[[nodiscard]] std::optional<std::string> parse_quoted(std::string_view in) {
    if (in.size() < 2 || in.front() != '"' || in.back() != '"') {
        return std::nullopt;
    }
    in.remove_prefix(1);
    in.remove_suffix(1);
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char const c = in[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= in.size()) {
            return std::nullopt;
        }
        char const esc = in[++i];
        switch (esc) {
        case '\\': out.push_back('\\'); break;
        case '"':  out.push_back('"'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        default:
            return std::nullopt;
        }
    }
    return out;
}

// Escape for round-trip writing back out.
[[nodiscard]] std::string toml_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c;
        }
    }
    out += '"';
    return out;
}

// Minimal single-section TOML parser. Schema (docs/25 §4):
//
//   [paths]
//   definitions_root = "..."
//   rom_dump_root    = "..."
//
// Unknown keys are silently ignored (forward-compat). Unknown sections
// are silently ignored. Lines that don't match the `key = "value"`
// shape, after stripping comments + whitespace, are syntax errors.
[[nodiscard]] Result<Paths> parse_config_text(std::string_view text) {
    Paths out;
    std::size_t lineno = 0;
    std::string_view current_section;
    while (!text.empty()) {
        ++lineno;
        // Find end of line.
        auto const eol = text.find('\n');
        std::string_view line =
            (eol == std::string_view::npos) ? text : text.substr(0, eol);
        text = (eol == std::string_view::npos) ? std::string_view{}
                                                : text.substr(eol + 1);
        // Strip comment.
        if (auto const h = line.find('#'); h != std::string_view::npos) {
            line = line.substr(0, h);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            current_section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        // Expect key = value.
        auto const eq = line.find('=');
        if (eq == std::string_view::npos) {
            return failure(ErrorCode::ParseError,
                           "config:" + std::to_string(lineno) +
                               ": expected `key = value`, got: " +
                               std::string{line});
        }
        auto const key = trim(line.substr(0, eq));
        auto const raw_value = trim(line.substr(eq + 1));
        auto value = parse_quoted(raw_value);
        if (!value.has_value()) {
            return failure(ErrorCode::ParseError,
                           "config:" + std::to_string(lineno) + ": key '" +
                               std::string{key} +
                               "': expected quoted string value");
        }
        if (current_section == "paths") {
            if (key == "definitions_root") {
                out.definitions_root = *value;
            } else if (key == "rom_dump_root") {
                out.rom_dump_root = *value;
            }
            // Unknown key under [paths] silently ignored — forward-compat.
        }
        // Unknown sections silently ignored.
    }
    return out;
}

[[nodiscard]] std::string render_config_text(Paths const &p) {
    std::ostringstream os;
    os << "# SubuwuTuner runtime config\n";
    os << "# Edit via GUI Tools->Settings... or `subuwutuner-cli config set ...`\n";
    os << "# Spec: docs/25-config-system.md\n";
    os << "\n";
    os << "[paths]\n";
    os << "definitions_root = " << toml_quote(p.definitions_root.string()) << "\n";
    os << "rom_dump_root    = " << toml_quote(p.rom_dump_root.string()) << "\n";
    return os.str();
}

} // namespace

std::filesystem::path default_config_path() {
    if (auto override_path = env_var("ST_CONFIG_FILE")) {
        return std::filesystem::path{*override_path};
    }
#ifdef _WIN32
    if (auto p = env_var("APPDATA")) {
        return std::filesystem::path{*p} / "SubuwuTuner" / "config.toml";
    }
    return user_home() / "AppData" / "Roaming" / "SubuwuTuner" / "config.toml";
#elif defined(__APPLE__)
    return user_home() / "Library" / "Application Support" / "SubuwuTuner" /
           "config.toml";
#else
    if (auto p = env_var("XDG_CONFIG_HOME")) {
        return std::filesystem::path{*p} / "SubuwuTuner" / "config.toml";
    }
    return user_home() / ".config" / "SubuwuTuner" / "config.toml";
#endif
}

std::filesystem::path default_definitions_root() {
    return executable_dir() / "definitions";
}

std::filesystem::path default_rom_dump_root() {
#ifdef _WIN32
    return user_home() / "Documents" / "SubuwuTuner" / "roms";
#else
    if (auto p = env_var("XDG_DATA_HOME")) {
        return std::filesystem::path{*p} / "SubuwuTuner" / "roms";
    }
    return user_home() / ".local" / "share" / "SubuwuTuner" / "roms";
#endif
}

Config Config::defaults() {
    Config c;
    c.paths_.definitions_root = default_definitions_root();
    c.paths_.rom_dump_root = default_rom_dump_root();
    c.source_path_ = default_config_path();
    return c;
}

Result<Config> Config::load() {
    return load_from(default_config_path());
}

Result<Config> Config::load_from(std::filesystem::path const &path) {
    std::error_code ec;
    auto const exists = std::filesystem::exists(path, ec);
    if (ec) {
        return failure(ErrorCode::IoFailure,
                       "config: cannot stat " + path.string() + ": " +
                           ec.message());
    }
    if (!exists) {
        // Missing-file is not an error (docs/25 §12).
        Config c = defaults();
        c.source_path_ = path;
        return c;
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::IoFailure,
                       "config: cannot open " + path.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto parsed = parse_config_text(buf.str());
    if (!parsed.has_value()) {
        return failure(parsed.error().code(), std::string{parsed.error().message()});
    }
    Config c = defaults(); // start from defaults; fill in what the file said
    if (!parsed->definitions_root.empty()) {
        c.paths_.definitions_root = parsed->definitions_root;
    }
    if (!parsed->rom_dump_root.empty()) {
        c.paths_.rom_dump_root = parsed->rom_dump_root;
    }
    c.source_path_ = path;
    return c;
}

Status Config::save() const {
    if (source_path_.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "config: save() requires a source_path; use defaults() "
                       "or load_from() first");
    }
    std::error_code ec;
    if (auto parent = source_path_.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return failure(ErrorCode::IoFailure,
                           "config: create_directories(" + parent.string() +
                               ") failed: " + ec.message());
        }
    }
    auto const tmp = source_path_.string() + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return failure(ErrorCode::PermissionDenied,
                           "config: cannot write " + tmp);
        }
        out << render_config_text(paths_);
        if (!out) {
            return failure(ErrorCode::IoFailure,
                           "config: write failed for " + tmp);
        }
    } // ofstream destructor closes the file before rename
    // Atomic-ish rename: on POSIX rename(2) replaces; on Windows
    // std::filesystem::rename replaces on most modern toolchains, but
    // tolerate older behavior by removing the target first.
    std::filesystem::remove(source_path_, ec);
    ec.clear();
    std::filesystem::rename(tmp, source_path_, ec);
    if (ec) {
        return failure(ErrorCode::IoFailure,
                       "config: rename " + tmp + " -> " + source_path_.string() +
                           " failed: " + ec.message());
    }
    return ok();
}

Config const &current() {
    static std::once_flag once;
    auto &cache = cached_state();
    std::call_once(once, []() {
        auto loaded = Config::load();
        cached_state() = loaded.has_value() ? *loaded : Config::defaults();
    });
    return *cache;
}

void override_definitions_root(std::filesystem::path p) {
    overrides_state().definitions_root = std::move(p);
}

void override_rom_dump_root(std::filesystem::path p) {
    overrides_state().rom_dump_root = std::move(p);
}

std::filesystem::path definitions_root() {
    if (auto const &o = overrides_state().definitions_root; o.has_value()) {
        return *o;
    }
    if (auto e = env_var("ST_DEFINITIONS_ROOT"); e.has_value()) {
        return std::filesystem::path{*e};
    }
    return current().paths().definitions_root;
}

std::filesystem::path rom_dump_root() {
    if (auto const &o = overrides_state().rom_dump_root; o.has_value()) {
        return *o;
    }
    if (auto e = env_var("ST_ROM_DUMP_ROOT"); e.has_value()) {
        return std::filesystem::path{*e};
    }
    return current().paths().rom_dump_root;
}

void reset_for_testing() {
    cached_state().reset();
    overrides_state() = Overrides{};
    // std::call_once's std::once_flag can't be reset, so the production
    // current() will keep using the cached value once called. Tests that
    // exercise current() should do so via fresh threads or accept that
    // current() is one-shot per process; the cached_state().reset() above
    // is for paths that bypass the call_once gate.
}

} // namespace st::config
