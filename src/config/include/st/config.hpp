// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::config — runtime path configuration (Phase 2 installer / config story).
//
// See docs/25-config-system.md for the full design. Quick recap of the
// shape this header exposes:
//
//   Phase 2 makes two paths user-configurable at install time (NSIS
//   custom pages, future) and editable post-install (GUI Settings,
//   CLI `config` subcommand). The runtime layer is this module.
//
//   Override precedence, most-specific first:
//     1. CLI flag       — `override_definitions_root(...)` etc.
//     2. Environment    — ST_DEFINITIONS_ROOT, ST_ROM_DUMP_ROOT
//     3. Config file    — %APPDATA%\SubuwuTuner\config.toml (Windows)
//                         $XDG_CONFIG_HOME/SubuwuTuner/config.toml (POSIX)
//     4. Built-in default — see default_*_root() below
//
// The library has no domain deps (lives alongside st::core). Other
// modules call it; nobody calls them.

#ifndef ST_CONFIG_HPP
#define ST_CONFIG_HPP

#include "st/core/result.hpp"

#include <filesystem>
#include <string>

namespace st::config {

// Configurable paths in Phase 2 scope. Adding fields here is an additive
// schema change — until a renaming / semantic break needs migration,
// no schema-version field. See docs/25 §13.
struct Paths {
    std::filesystem::path definitions_root;
    std::filesystem::path rom_dump_root;
};

class Config {
public:
    // Load from the canonical user-config path (see default_config_path).
    // ST_CONFIG_FILE env var, when set, redirects to that path instead.
    // Missing file is not an error — silently returns a defaults()-shaped
    // Config with source_path() pointing where the file WOULD have lived.
    [[nodiscard]] static Result<Config> load();

    // Explicit-path loader. Used by tests and `--config <path>` CLI flag.
    // Missing file returns defaults (same silent-fallback rule as load()).
    // Existing file with parse errors returns failure(ParseError).
    [[nodiscard]] static Result<Config> load_from(std::filesystem::path const &);

    // Hard-coded §2 defaults; no file consulted. Source path is set to
    // default_config_path() so a subsequent save() writes there.
    [[nodiscard]] static Config defaults();

    // Atomic write: parent dirs created, temp file written, rename to
    // source_path. Status::ok on success; PermissionDenied if the dir
    // is read-only, IoFailure on rename failure.
    [[nodiscard]] Status save() const;

    [[nodiscard]] Paths const &paths() const noexcept { return paths_; }
    [[nodiscard]] Paths &paths() noexcept { return paths_; }

    [[nodiscard]] std::filesystem::path const &source_path() const noexcept {
        return source_path_;
    }
    void set_source_path(std::filesystem::path p) {
        source_path_ = std::move(p);
    }

private:
    Paths paths_;
    std::filesystem::path source_path_;
};

// ----------------------------------------------------------------------------
// Process-lifetime facade
// ----------------------------------------------------------------------------
//
// `current()` lazy-loads the canonical config the first time it's called
// and caches the result. Override hooks (`override_*`) update overrides
// that take precedence over the cached values; the cache itself isn't
// invalidated.
//
// Thread-safety: the cache is constructed once via a Meyers singleton.
// override_* writers must serialize externally (the codebase uses main-
// thread-only for config mutation today).

[[nodiscard]] Config const &current();

void override_definitions_root(std::filesystem::path);
void override_rom_dump_root(std::filesystem::path);

// Per-key resolved lookups. Applies override > env > config > default.
[[nodiscard]] std::filesystem::path definitions_root();
[[nodiscard]] std::filesystem::path rom_dump_root();

// ----------------------------------------------------------------------------
// Path computation
// ----------------------------------------------------------------------------
//
// Pure functions — no caching, no env consultation. The defaults the
// rest of the system falls back to when no config file is present.

// Canonical user-config-file path. Honors ST_CONFIG_FILE env when set.
[[nodiscard]] std::filesystem::path default_config_path();

// `<install>/definitions/` — install dir derived from the running binary.
[[nodiscard]] std::filesystem::path default_definitions_root();

// User's typical writable docs/data location for captured ROMs.
[[nodiscard]] std::filesystem::path default_rom_dump_root();

// ----------------------------------------------------------------------------
// Testing helpers (not for production callers)
// ----------------------------------------------------------------------------

// Clears the current() cache + override state. Tests call this between
// scenarios so one test's state doesn't bleed into the next.
void reset_for_testing();

} // namespace st::config

#endif // ST_CONFIG_HPP
