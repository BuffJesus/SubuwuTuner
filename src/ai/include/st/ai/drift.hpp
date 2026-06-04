// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ai::drift — Tier 1 drift classifier per docs/20.
//
// Pure function over an adaptive-history snapshot → a structured
// diagnosis (cause + confidence + evidence). No LLM dependency, no
// external state, no I/O. The classifier exists so the CLI / GUI can
// surface a "what's likely wrong" reading the user can act on without
// reading the time series themselves.
//
// Deliberate constraints (carry forward from docs/20):
//   1. The classifier is *advisory*. Output never gates an edit
//      decision; the user reads it and chooses. No "AI auto-pull"
//      channel exists or will exist.
//   2. The classifier is *testable*. Synthetic snapshots in the test
//      suite pin every rule output without firing up a model.
//   3. The classifier is *narrow*. v1 ships rules over the aggregated
//      LTFT/DAM/IdleAdapt signals already in HistorySnapshot. Per-
//      cell LTFT diagnoses (vacuum-leak-vs-injector-aging
//      disambiguation) are flagged as Ambiguous in v1 and refined
//      when per-cell data lands.
//   4. The classifier returns *evidence* alongside its conclusion so
//      the user can cross-check ("LTFT mean = -7.2% over 28 days;
//      drift slope -0.1%/day"). Evidence is pre-rendered text that
//      callers paste verbatim — keeps the GUI / CLI surfaces in
//      lock-step.
//
// What's NOT here (intentional):
//   - LLM narration. That's `st::ai::explain` (future). The drift
//     classifier hands its DriftDiagnosis to the explain layer when
//     the user clicks "explain".
//   - Per-cell vacuum-vs-injector disambiguation. Requires per-cell
//     LTFT data that HistorySnapshot doesn't carry yet. v1 returns
//     `Cause::FuelSystemDrift` with the candidate causes named in
//     `alternatives` until the snapshot grows the field.
//   - Auto-applied recommendations. `recommended_checks` is text the
//     user reads; no executor consumes them.

#ifndef ST_AI_DRIFT_HPP
#define ST_AI_DRIFT_HPP

#include "st/log/adaptive_history.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::ai::drift {

// How sure the classifier is. Values mirror docs/20 §"Output shape".
//   Likely    — multiple corroborating pieces of evidence; one cause
//               clearly dominates.
//   Possible  — single piece of evidence; the cause fits but other
//               causes aren't ruled out.
//   Ambiguous — multiple causes overlap within tolerance; alternatives
//               vector lists them in order of evidence strength.
//   NoSignal  — snapshot is empty / too short / signals all near
//               baseline; nothing actionable to report.
enum class Confidence : std::uint8_t {
    Likely,
    Possible,
    Ambiguous,
    NoSignal,
};

[[nodiscard]] std::string_view confidence_name(Confidence c) noexcept;

// Diagnosis taxonomy. Strings (in `cause`) follow the docs/20 rule
// table verbatim so the wire format stays stable across tooling.
//
// v1 v2 split:
//   v1 (this module) — fuel_system_drift, knock_correction,
//                      idle_air_drift, recent_change, no_signal
//   v2 (per-cell)    — vacuum_leak, injector_aging, maf_aging,
//                      o2_sensor_failing, fuel_quality_drift
struct DriftDiagnosis {
    // Stable wire-format id (snake_case). Empty when confidence ==
    // NoSignal; the description still carries a helpful "no signal"
    // narration.
    std::string cause;
    Confidence confidence{Confidence::NoSignal};
    // One-line human summary; the CLI prints this as the headline,
    // the GUI surfaces it as a diagnosis chip + body.
    std::string description;
    // Pre-rendered evidence lines. Format is "<field>: <observation>"
    // so a reader can grep / scan without parsing prose. Always
    // included even when confidence == NoSignal (the "why I have
    // nothing to report" lines).
    std::vector<std::string> evidence;
    // Other causes the snapshot is consistent with, in order of
    // evidence strength. Empty when the diagnosis is unambiguous.
    std::vector<std::string> alternatives;
    // Suggested next steps. Text only — no executable action behind
    // these. v1 strings are stable; consumers shouldn't programmatically
    // parse them.
    std::vector<std::string> recommended_checks;
};

// Classifier entry point. Pure function — no I/O, no allocations
// beyond the returned struct's contents.
//
// The snapshot must come from `st::log::adaptive::snapshot_from_*`.
// Any signal with `has_data == false` is treated as "absent"; the
// classifier still runs, but evidence is correspondingly narrower
// and confidence degrades (Likely → Possible, Possible → Ambiguous).
[[nodiscard]] DriftDiagnosis classify(st::log::adaptive::HistorySnapshot const &history);

// Thresholds — exposed so tests can drive synthetic snapshots into
// each rule cleanly + so future callers can tune sensitivity without
// patching the source. Defaults follow the docs/20 commentary and
// validation against synthetic fixtures in tests/unit/ai/.
struct Thresholds {
    // LTFT magnitude considered "off baseline" (percent correction,
    // -25..+25 range). 5% is the conservative limit per Subaru OEM
    // adaptive logic — outside this band the ECU starts trimming
    // closed-loop authority.
    double ltft_offset_pct{5.0};
    // LTFT drift slope per day considered "drifting". 0.5%/day = 14%
    // over a month, which is well past the safe operating band.
    double ltft_drift_per_day_pct{0.5};
    // DAM below this fraction = persistent knock correction. Stock
    // FA-DIT DAM lives at 1.0; drops to 0.94 or 0.88 are well-known
    // knock-triggered demerits.
    double dam_lower_threshold{0.95};
    // Idle-adapt magnitude considered drift. Sign matters: positive
    // = adding air (vacuum loss), negative = restricting air. Units
    // depend on the log but a 5-unit step is significant on every
    // observed channel.
    double idle_adapt_offset{5.0};
    // Minimum time span before the classifier reports non-trivial
    // findings. Below this, we don't have enough history to call
    // anything drift — return Possible/Ambiguous at best regardless
    // of magnitudes.
    double minimum_span_seconds{86400.0 * 3.0};
};

[[nodiscard]] DriftDiagnosis classify_with(st::log::adaptive::HistorySnapshot const &history,
                                            Thresholds const &thresholds);

} // namespace st::ai::drift

#endif // ST_AI_DRIFT_HPP
