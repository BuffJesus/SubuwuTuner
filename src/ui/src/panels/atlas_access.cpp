// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "panels/atlas_access.hpp"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>

namespace st::ui {

namespace {

// One-shot lazy initialization. Cached result + done flag + mutex so
// concurrent first callers race harmlessly. After init, reads are
// lock-free against the cached optional.
struct AtlasCache {
    std::once_flag once;
    std::optional<st::library::Atlas> atlas;
};

AtlasCache &cache() {
    static AtlasCache c;
    return c;
}

void try_load(std::optional<st::library::Atlas> &out) {
    // 1. Env override first — explicit absolute path. Matches the CLI
    // `tuner-atlas` subcommand convention.
    if (char const *override_path = std::getenv("ST_TUNER_ATLAS");
        override_path != nullptr && override_path[0] != '\0') {
        if (auto r = st::library::Atlas::load_from_file(override_path);
            r.has_value()) {
            out = std::move(*r);
            return;
        }
        // Env var pointed somewhere broken — silently fall through to
        // the cwd walk. A failure to load with the env var set isn't
        // worth a stderr / toast here since this is a pure read path.
    }
    // 2. Walk parents of cwd looking for the canonical fixture path.
    // Hops capped at 6 to match library.cpp's behavior — covers
    // build/<preset>/bin → repo-root walks but stops before climbing
    // out of any reasonable checkout.
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        return;
    }
    for (int hops = 0; hops < 6; ++hops) {
        auto const candidate =
            cwd / "fixtures" / "tuner_atlas" / "tuner_atlas.toml";
        if (std::filesystem::exists(candidate, ec) && !ec) {
            if (auto r = st::library::Atlas::load_from_file(candidate);
                r.has_value()) {
                out = std::move(*r);
                return;
            }
        }
        ec.clear();
        if (!cwd.has_parent_path() || cwd == cwd.parent_path()) {
            break;
        }
        cwd = cwd.parent_path();
    }
}

} // namespace

st::library::Atlas const *shared_atlas() {
    auto &c = cache();
    std::call_once(c.once, [&] { try_load(c.atlas); });
    return c.atlas.has_value() ? &*c.atlas : nullptr;
}

} // namespace st::ui
