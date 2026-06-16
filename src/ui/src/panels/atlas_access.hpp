// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Shared lazy-loaded atlas singleton for GUI panels that aren't the
// Library panel. The Library panel keeps its own file-static atlas
// load (it's the canonical Library-of-tunes surface; the analyst's
// S1 plumbed annotate_tune through there). Other panels — sidebar,
// table editor, stats — want atlas access for per-table context but
// shouldn't depend on the Library panel having been opened.
//
// Discovery: ST_TUNER_ATLAS env var first; otherwise walk parents of
// cwd looking for fixtures/tuner_atlas/tuner_atlas.toml. Same posture
// as the CLI `tuner-atlas` subcommand + library.cpp's load_atlas_lazy.
//
// Returns nullptr when load failed / no atlas file found. Consumers
// degrade silently (no badges, no context section rendered).

#ifndef ST_UI_PANELS_ATLAS_ACCESS_HPP
#define ST_UI_PANELS_ATLAS_ACCESS_HPP

#include "st/library/atlas.hpp"

namespace st::ui {

// Lazy-loaded process-wide atlas. First call attempts the discovery
// + load and caches the result; subsequent calls return the cached
// pointer (or nullptr if the first attempt failed). Thread-safety:
// the singleton init is std::call_once-style via a function-local
// static, so concurrent first calls don't race. GUI panels are all
// on the main thread anyway, but the contract leaves room for the
// future worker-queue panels to read this without coordination.
[[nodiscard]] st::library::Atlas const *shared_atlas();

} // namespace st::ui

#endif // ST_UI_PANELS_ATLAS_ACCESS_HPP
