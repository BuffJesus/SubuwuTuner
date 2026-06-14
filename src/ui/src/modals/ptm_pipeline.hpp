// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// UI-side helper for building a .ptm payload from the currently-loaded
// project. Shared by:
//
//   - render_ptm_export_modal (writes the bytes to a user-chosen path)
//   - render_ptm_save_and_push_modal (writes the bytes to the AP's
//     /maps/ via cmd 0x22 + cmd 0x05 NAND barrier)
//
// Keeping the build phase in one place means both flows produce a
// byte-identical .ptm for the same project + seed; the round-trip test
// in `findings/handoffs/HANDOFF-from-analyst-2026-06-14-edit-export-
// push-ptm.md` §T29 stays valid for Save & Push without re-running the
// export pipeline.

#ifndef ST_UI_MODALS_PTM_PIPELINE_HPP
#define ST_UI_MODALS_PTM_PIPELINE_HPP

#include "app_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::ui {

// Build the .ptm bytes for the loaded project at the given XTEA seed.
//
// Reads `project.toml` [ptm_metadata] + `ptm_patches.toml` from the
// project directory, runs them through the XML builders, then the AES
// + bzip2 + XTEA cipher chain. Returns the resulting .ptm file bytes
// on success.
//
// On failure, returns nullopt and writes a human-readable error string
// into `err_out`. Errors include:
//   - no project open
//   - missing ptm_patches.toml (project wasn't created via `ptm import`)
//   - malformed [[patch]] entries (all dropped → would emit an empty tune)
//   - encrypt_ptm returned NotImplemented (build flag off)
//
// `seed` is the per-file XTEA IV. Pass the original .ptm's seed when
// round-tripping a tune to preserve byte-identity; pass any fresh u32
// when authoring a new tune.
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
build_ptm_bytes(AppState &state, std::uint32_t seed, std::string &err_out);

} // namespace st::ui

#endif // ST_UI_MODALS_PTM_PIPELINE_HPP
