// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ai/drift.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace st::ai::drift {

namespace {

using st::log::adaptive::SignalKind;

constexpr double kSecondsPerDay = 86400.0;

// Format a percentage with consistent sign + 2 decimals. Wraps the
// value in a string so call sites can paste into evidence lines
// without their own snprintf.
std::string fmt_pct(double v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.2f%%", v);
    return std::string{buf};
}

std::string fmt_unit(double v, char const *unit) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.2f%s", v, unit);
    return std::string{buf};
}

std::string fmt_days(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f days", seconds / kSecondsPerDay);
    return std::string{buf};
}

std::string fmt_per_day(double per_second) {
    return fmt_pct(per_second * kSecondsPerDay);
}

st::log::adaptive::SignalSeries const &
series_of(st::log::adaptive::HistorySnapshot const &history, SignalKind k) noexcept {
    return history.series[static_cast<std::size_t>(k)];
}

bool span_sufficient(st::log::adaptive::HistorySnapshot const &history,
                     Thresholds const &t) noexcept {
    return history.time_span_seconds >= t.minimum_span_seconds;
}

// Build a "no signal" diagnosis with the right evidence for why we
// have nothing to report. Three flavors: empty snapshot, too-short
// span, or all signals near baseline.
DriftDiagnosis make_no_signal(st::log::adaptive::HistorySnapshot const &history,
                              Thresholds const &t,
                              char const *reason) {
    DriftDiagnosis out;
    out.cause = "no_signal";
    out.confidence = Confidence::NoSignal;
    out.description = "No actionable drift detected.";
    out.evidence.emplace_back(reason);
    if (history.total_samples > 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "Samples: %llu over %s",
                      static_cast<unsigned long long>(history.total_samples),
                      fmt_days(history.time_span_seconds).c_str());
        out.evidence.emplace_back(buf);
    }
    out.recommended_checks.emplace_back(
        "Re-run after another week of driving to gather longer-baseline data.");
    out.recommended_checks.emplace_back(
        "Confirm the adaptive-history CSV maps LTFT / DAM / IdleAdapt columns correctly.");
    (void)t;
    return out;
}

} // namespace

std::string_view confidence_name(Confidence c) noexcept {
    switch (c) {
    case Confidence::Likely:
        return "likely";
    case Confidence::Possible:
        return "possible";
    case Confidence::Ambiguous:
        return "ambiguous";
    case Confidence::NoSignal:
        return "no_signal";
    }
    return "unknown";
}

DriftDiagnosis classify(st::log::adaptive::HistorySnapshot const &history) {
    return classify_with(history, Thresholds{});
}

DriftDiagnosis classify_with(st::log::adaptive::HistorySnapshot const &history,
                              Thresholds const &t) {
    // Trivial-case rejection. An empty / single-sample snapshot can't
    // tell us anything; surface "no signal" with the why.
    if (history.total_samples == 0) {
        return make_no_signal(history, t, "No samples in snapshot.");
    }
    if (!span_sufficient(history, t)) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "Time span %s is shorter than minimum %.1f days; "
                      "drift trends not yet meaningful.",
                      fmt_days(history.time_span_seconds).c_str(),
                      t.minimum_span_seconds / kSecondsPerDay);
        return make_no_signal(history, t, buf);
    }

    auto const &ltft = series_of(history, SignalKind::Ltft);
    auto const &dam = series_of(history, SignalKind::Dam);
    auto const &idle = series_of(history, SignalKind::IdleAdapt);

    // Feature extraction. Each `<signal>_off` flag fires when the
    // absolute deviation exceeds the threshold; `<signal>_drift`
    // fires on the per-day slope. We weight by how many features
    // corroborate (>=2 ⇒ Likely, ==1 ⇒ Possible, ambiguous mix ⇒
    // Ambiguous).
    bool const ltft_present = ltft.has_data;
    bool const dam_present = dam.has_data;
    bool const idle_present = idle.has_data;

    double const ltft_mean = ltft.overall_mean;
    double const ltft_drift_pd = ltft.drift_per_second * kSecondsPerDay;
    double const dam_mean = dam.overall_mean;
    double const idle_mean = idle.overall_mean;
    double const idle_drift_pd = idle.drift_per_second * kSecondsPerDay;

    bool const ltft_neg_off = ltft_present && ltft_mean <= -t.ltft_offset_pct;
    bool const ltft_pos_off = ltft_present && ltft_mean >= t.ltft_offset_pct;
    bool const ltft_drifting_neg =
        ltft_present && ltft_drift_pd <= -t.ltft_drift_per_day_pct;
    bool const ltft_drifting_pos =
        ltft_present && ltft_drift_pd >= t.ltft_drift_per_day_pct;
    bool const dam_low =
        dam_present && dam_mean <= t.dam_lower_threshold;
    bool const idle_pos_off =
        idle_present && idle_mean >= t.idle_adapt_offset;
    bool const idle_drifting_pos =
        idle_present && idle_drift_pd >= 0.5; // 0.5 unit/day baseline

    DriftDiagnosis out;

    // Rule 1: DAM clearly below baseline → knock-correction drift.
    // Strongest standalone signal; takes priority because knock is
    // engine-safety territory and the user should see it first.
    if (dam_low) {
        out.cause = "knock_correction";
        out.confidence = dam_mean < (t.dam_lower_threshold - 0.03) ? Confidence::Likely
                                                                    : Confidence::Possible;
        out.description =
            "Persistent knock correction — DAM has settled below the baseline.";
        out.evidence.emplace_back("DAM mean: " + fmt_unit(dam_mean, ""));
        if (ltft_pos_off) {
            out.evidence.emplace_back("LTFT mean: " + fmt_pct(ltft_mean) +
                                      " (consistent with rich-compensation "
                                      "for lower fuel quality)");
            out.alternatives.emplace_back("fuel_quality_drift");
        }
        out.recommended_checks.emplace_back(
            "Verify fuel grade — DAM responds quickly to lower-octane fills.");
        out.recommended_checks.emplace_back(
            "Pull a WOT log and check the knock-event count + retard table.");
        out.recommended_checks.emplace_back(
            "Inspect ignition coils + plugs if DAM doesn't recover with premium.");
        return out;
    }

    // Rule 2: LTFT positive (commanding lean correction) with positive
    // drift → fuel-quality drift (higher ethanol than expected, or fuel
    // density change). Tier 1 can't distinguish ethanol-content from
    // injector-overflow without per-cell data; we report the broader
    // cause with both alternatives.
    if (ltft_pos_off && ltft_drifting_pos) {
        out.cause = "fuel_quality_drift";
        out.confidence = Confidence::Likely;
        out.description =
            "Fuel quality drift — LTFT is positive and trending further positive.";
        out.evidence.emplace_back("LTFT mean: " + fmt_pct(ltft_mean));
        out.evidence.emplace_back("LTFT drift: " + fmt_per_day(ltft.drift_per_second) +
                                  " / day over " + fmt_days(history.time_span_seconds));
        out.alternatives.emplace_back("injector_overflow_or_leak");
        out.recommended_checks.emplace_back(
            "Switch fuel brand / batch and re-run for a tank to confirm.");
        out.recommended_checks.emplace_back(
            "If on flex-fuel, log ethanol % directly — drift may reflect E-content.");
        return out;
    }

    // Rule 3: LTFT negative and drifting more negative → fuel-system
    // drift (vacuum leak, injector aging, MAF aging). Without per-
    // cell LTFT we can't disambiguate, so the v1 diagnosis lists all
    // three as alternatives and routes the user to a triage checklist.
    if (ltft_neg_off && ltft_drifting_neg) {
        out.cause = "fuel_system_drift";
        out.confidence = Confidence::Likely;
        out.description =
            "Fuel-system drift — LTFT is negative and trending further negative.";
        out.evidence.emplace_back("LTFT mean: " + fmt_pct(ltft_mean));
        out.evidence.emplace_back("LTFT drift: " + fmt_per_day(ltft.drift_per_second) +
                                  " / day over " + fmt_days(history.time_span_seconds));
        if (idle_pos_off || idle_drifting_pos) {
            out.evidence.emplace_back(
                "Idle adapt: " + fmt_unit(idle_mean, "") +
                " (positive idle-adapt corroborates a vacuum-side cause)");
            out.alternatives.emplace_back("vacuum_leak");
            out.alternatives.emplace_back("evap_leak");
        } else {
            // No idle-adapt corroboration — the diagnosis is murkier.
            // List the three classic causes; the user runs the
            // recommended checks to narrow down.
            out.confidence = Confidence::Ambiguous;
            out.alternatives.emplace_back("vacuum_leak");
            out.alternatives.emplace_back("injector_aging");
            out.alternatives.emplace_back("maf_aging");
        }
        out.recommended_checks.emplace_back(
            "Smoke-test the intake manifold + vacuum hoses for leaks.");
        out.recommended_checks.emplace_back(
            "Compare injector dead-time + flow-rate against pack baseline.");
        out.recommended_checks.emplace_back(
            "Inspect MAF element for oil contamination; clean with MAF cleaner.");
        return out;
    }

    // Rule 4: LTFT off-baseline OR drifting (single signal) — Possible,
    // not Likely. The user has SOMETHING worth investigating but not
    // enough to call a cause confidently.
    if (ltft_neg_off || ltft_pos_off || ltft_drifting_neg || ltft_drifting_pos) {
        out.cause = ltft_neg_off || ltft_drifting_neg ? "fuel_system_drift"
                                                       : "fuel_quality_drift";
        out.confidence = Confidence::Possible;
        out.description =
            "LTFT is off baseline; cause not yet corroborated by other signals.";
        out.evidence.emplace_back("LTFT mean: " + fmt_pct(ltft_mean));
        out.evidence.emplace_back("LTFT drift: " + fmt_per_day(ltft.drift_per_second) +
                                  " / day over " + fmt_days(history.time_span_seconds));
        if (!dam_present) {
            out.evidence.emplace_back("DAM signal absent — cannot rule out "
                                      "knock-correlated drift.");
        }
        if (!idle_present) {
            out.evidence.emplace_back("Idle-adapt signal absent — cannot rule out "
                                      "vacuum-side cause.");
        }
        out.recommended_checks.emplace_back(
            "Re-log with DAM + IdleAdapt columns mapped for a corroborating signal.");
        out.recommended_checks.emplace_back(
            "Re-run after a tank of consistent fuel to rule out fuel-quality drift.");
        return out;
    }

    // Rule 5: Idle-adapt off baseline without an LTFT signal —
    // idle_air_drift. Common after intake-piping work, throttle-body
    // cleaning, or a recent battery disconnect that reset adaptations.
    if (idle_pos_off || idle_drifting_pos) {
        out.cause = "idle_air_drift";
        out.confidence = Confidence::Possible;
        out.description =
            "Idle adapt is off baseline; LTFT remains within normal range.";
        out.evidence.emplace_back("Idle adapt mean: " + fmt_unit(idle_mean, ""));
        out.evidence.emplace_back("Idle adapt drift: " +
                                  fmt_unit(idle.drift_per_second * kSecondsPerDay, "") +
                                  " / day");
        out.alternatives.emplace_back("post_disconnect_relearn");
        out.alternatives.emplace_back("throttle_body_carbon");
        out.recommended_checks.emplace_back(
            "Allow another week of mixed driving for the IAC to converge.");
        out.recommended_checks.emplace_back(
            "If recent throttle-body work, perform the throttle-relearn drive cycle.");
        return out;
    }

    // No rule fired — signals are within baseline. Surface the "all
    // clear" reading explicitly so the user knows the classifier saw
    // them and chose "no signal" rather than failing silently.
    out = make_no_signal(history, t, "All signals are within baseline tolerance.");
    // Replace the "no samples" / "too short" recommendations with
    // an all-clear set.
    out.recommended_checks.clear();
    out.recommended_checks.emplace_back(
        "Adaptive trims look healthy. Re-run after the next month of driving.");
    if (ltft_present) {
        out.evidence.emplace_back("LTFT mean: " + fmt_pct(ltft_mean));
    }
    if (dam_present) {
        out.evidence.emplace_back("DAM mean: " + fmt_unit(dam_mean, ""));
    }
    if (idle_present) {
        out.evidence.emplace_back("Idle adapt mean: " + fmt_unit(idle_mean, ""));
    }
    return out;
}

} // namespace st::ai::drift
