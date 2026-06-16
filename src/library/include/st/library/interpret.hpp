// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::interpret_inspect / interpret_diff — heuristic prose
// generators that turn the structured `DecodedPtm` / `TuneDiffResult`
// surfaces into human-readable sentences describing the tune's
// character ("Stage-tier throttle remap", "boost-up", "FA24 HPFP
// lobe-phase compensation", etc.).
//
// These are pure if-then heuristics over the existing aggregates — no
// LLM, no learned classifier, no external data. Each rule encodes a
// pattern observed during the 2026-06-10..06-12 RE wave (see MEMORY.md
// references to Fehr WRK1/2/3, COBB/NexGen philosophy, FA24 HPFP
// cluster, MAF Sensor Scale BigSF distinguisher). Failing to interpret
// is fine — we emit nothing rather than wrong prose.
//
// Shared between CLI's `ptm inspect` / `ptm diff` text mode and the
// matching GUI modals so both surfaces speak the same vocabulary by
// construction.

#ifndef ST_LIBRARY_INTERPRET_HPP
#define ST_LIBRARY_INTERPRET_HPP

#include "st/library/atlas.hpp"
#include "st/library/patch_decoder.hpp"
#include "st/library/table_mapping.hpp"
#include "st/library/tune_diff.hpp"

#include <string>
#include <vector>

namespace st::library {

// Heuristic prose for `ptm inspect`. Builds a small vector of complete
// sentences (already capitalized + punctuated) summarizing what the
// tune looks like at a glance. Table-name-based rules only fire when
// `top_tables` is non-empty (i.e. a def-pack was supplied); patch-
// count thresholds always fire. May return an empty vector when no
// rule triggers cleanly — that's fine, the caller just skips the
// Interpretation section.
//
// `atlas` is optional. When non-null, anchor-based prose (HPFP cluster,
// MAF Sensor Scale, etc.) is sourced from the atlas instead of the
// hardcoded fallbacks — and safety-pair detection (corpus-derived
// rules) is enabled. Pass nullptr to use the legacy hardcoded path.
[[nodiscard]] std::vector<std::string>
interpret_inspect(DecodedPtm const &decoded,
                  std::vector<TableHit> const &top_tables,
                  Atlas const *atlas = nullptr);

// Heuristic prose for `ptm diff`. Reads the result's by_table /
// per-bucket counts to describe the delta from A to B in terms a
// tuner would recognize ("B reverts the Requested Torque edits A
// carried", "B adds +2 uniformly across 112 cells of Wastegate Duty",
// etc.). The decoded patches are needed to detect uniform deltas
// (which the bucketed `TuneDiffResult` discards). Returns empty when
// no rule triggers.
//
// `atlas` is optional. Currently unused on the diff side but reserved
// for future cross-corpus comparisons (e.g. "B converges toward the
// Fehr-family signature").
[[nodiscard]] std::vector<std::string>
interpret_diff(DecodedPtm const &a, DecodedPtm const &b,
               TuneDiffResult const &result,
               Atlas const *atlas = nullptr);

} // namespace st::library

#endif // ST_LIBRARY_INTERPRET_HPP
