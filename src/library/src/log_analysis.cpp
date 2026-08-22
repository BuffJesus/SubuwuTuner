// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/log_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st::library::log_analysis {

namespace {

// Compact fixed-precision number for finding prose (std::to_string emits 6
// trailing decimals, which read as noise: "-2.100000 deg").
std::string num(double v, int precision = 2) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return std::string{buf};
}

std::string to_lower(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(std::string const &h, std::string_view needle) {
    return h.find(needle) != std::string::npos;
}

Role classify_header(std::string_view header) {
    std::string const h = to_lower(header);
    // Knock first — most safety-critical and unambiguous.
    if (contains(h, "fbkc") || contains(h, "fbk") ||
        (contains(h, "feedback") && contains(h, "knock"))) {
        return Role::FeedbackKnock;
    }
    if (contains(h, "flkc") || (contains(h, "fine") && contains(h, "knock")) ||
        (contains(h, "knock") && contains(h, "learn"))) {
        return Role::FineKnock;
    }
    if (h == "dam" || contains(h, "dynamic advance") || contains(h, "advance mult") ||
        contains(h, "advance_mult")) {
        return Role::Dam;
    }
    if (contains(h, "boost")) {
        if (contains(h, "target") || contains(h, "desired") || contains(h, "req")) {
            return Role::TargetBoost;
        }
        if (contains(h, "actual") || contains(h, "obs") || contains(h, "measured")) {
            return Role::ActualBoost;
        }
    }
    if (h == "cmd" || contains(h, "commanded") || contains(h, "af_target") ||
        contains(h, "afr_target") || contains(h, "lambda_target") ||
        contains(h, "target_afr") || contains(h, "target afr")) {
        return Role::CommandedAfr;
    }
    if (h == "obs" || contains(h, "wideband") || contains(h, "af_sensor") ||
        contains(h, "afr_actual") || contains(h, "lambda_actual") ||
        contains(h, "observed") || contains(h, "af sensor")) {
        return Role::ObservedAfr;
    }
    if (h == "rpm" || contains(h, "rpm") || contains(h, "engine speed")) {
        return Role::Rpm;
    }
    if (h == "load" || contains(h, "calc load") || contains(h, "calculated load") ||
        (contains(h, "load") && !contains(h, "download"))) {
        return Role::Load;
    }
    return Role::Unknown;
}

// First resolved column for a role, or nullopt.
std::optional<std::size_t> first_of(std::vector<ResolvedChannel> const &chans, Role role) {
    for (auto const &c : chans) {
        if (c.role == role) {
            return c.column;
        }
    }
    return std::nullopt;
}

double sample_at(datalog_csv::ParsedDatalog const &d, std::size_t col, std::size_t row) {
    if (col >= d.data.size() || row >= d.data[col].size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(d.data[col][row]);
}

// Fill rpm/load context on a finding at its worst row.
void add_context(LogFinding &f, datalog_csv::ParsedDatalog const &d,
                 std::vector<ResolvedChannel> const &chans) {
    if (!f.worst_row.has_value()) {
        return;
    }
    if (auto rpm = first_of(chans, Role::Rpm)) {
        double const v = sample_at(d, *rpm, *f.worst_row);
        if (!std::isnan(v)) {
            f.at_rpm = v;
        }
    }
    if (auto load = first_of(chans, Role::Load)) {
        double const v = sample_at(d, *load, *f.worst_row);
        if (!std::isnan(v)) {
            f.at_load = v;
        }
    }
}

int severity_rank(Severity s) { return static_cast<int>(s); }

constexpr double kKnockEps = -0.05;      // ignore float noise below this
constexpr double kOverboostRel = 0.10;   // >10% over target = overboost
constexpr double kLeanRel = 0.04;        // >4% leaner than commanded
constexpr double kHighLoadFraction = 0.7; // "under load" = top 30% of load

}  // namespace

std::string_view to_string(Role role) noexcept {
    switch (role) {
    case Role::Rpm: return "RPM";
    case Role::Load: return "Load";
    case Role::FeedbackKnock: return "Feedback knock";
    case Role::FineKnock: return "Fine knock";
    case Role::Dam: return "DAM";
    case Role::TargetBoost: return "Target boost";
    case Role::ActualBoost: return "Actual boost";
    case Role::CommandedAfr: return "Commanded AFR";
    case Role::ObservedAfr: return "Observed AFR";
    case Role::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info: return "Info";
    case Severity::Low: return "Low";
    case Severity::Medium: return "Medium";
    case Severity::High: return "High";
    }
    return "Info";
}

std::vector<ResolvedChannel> resolve_channels(datalog_csv::ParsedDatalog const &datalog) {
    std::vector<ResolvedChannel> out;
    for (std::size_t col = 0; col < datalog.headers.size(); ++col) {
        Role const role = classify_header(datalog.headers[col]);
        if (role != Role::Unknown) {
            out.push_back({role, col, datalog.headers[col]});
        }
    }
    return out;
}

LogAnalysis analyze(datalog_csv::ParsedDatalog const &datalog) {
    LogAnalysis result;
    result.channels = resolve_channels(datalog);
    auto const &chans = result.channels;
    std::size_t const rows = datalog.row_count;

    // ---- Feedback knock (active retard happening now) ------------------
    {
        std::size_t events = 0;
        double worst = 0.0;
        std::optional<std::size_t> worst_row;
        int worst_cyl = 0;
        int cyl = 0;
        bool any_col = false;
        for (auto const &c : chans) {
            if (c.role != Role::FeedbackKnock) {
                continue;
            }
            any_col = true;
            ++cyl;
            for (std::size_t r = 0; r < rows; ++r) {
                double const v = sample_at(datalog, c.column, r);
                if (std::isnan(v) || v >= kKnockEps) {
                    continue;
                }
                ++events;
                if (v < worst) {
                    worst = v;
                    worst_row = r;
                    worst_cyl = cyl;
                }
            }
        }
        if (any_col) {
            LogFinding f;
            f.id = "knock.feedback";
            if (events == 0) {
                f.title = "No active knock retard";
                f.severity = Severity::Info;
                f.detail = "Feedback knock correction stayed at zero for the whole "
                           "log \xE2\x80\x94 no cylinder pulled timing in real time.";
            } else {
                f.title = "Active knock retard";
                f.severity = worst <= -1.0 ? Severity::High : Severity::Medium;
                f.event_count = events;
                f.worst_value = worst;
                f.worst_row = worst_row;
                f.detail = std::to_string(events) +
                           " sample(s) show the ECU pulling timing in real time; worst " +
                           num(worst) + " deg on cyl " + std::to_string(worst_cyl) +
                           ". Investigate fuel/timing at that load before the next pull.";
                add_context(f, datalog, chans);
            }
            result.findings.push_back(std::move(f));
        }
    }

    // ---- Fine (learned) knock ------------------------------------------
    {
        double worst = 0.0;
        std::optional<std::size_t> worst_row;
        int worst_cyl = 0;
        int cyl = 0;
        bool any_col = false;
        for (auto const &c : chans) {
            if (c.role != Role::FineKnock) {
                continue;
            }
            any_col = true;
            ++cyl;
            for (std::size_t r = 0; r < rows; ++r) {
                double const v = sample_at(datalog, c.column, r);
                if (std::isnan(v)) {
                    continue;
                }
                if (v < worst) {
                    worst = v;
                    worst_row = r;
                    worst_cyl = cyl;
                }
            }
        }
        if (any_col && worst < kKnockEps) {
            LogFinding f;
            f.id = "knock.fine";
            f.title = "Learned timing pull";
            f.severity = worst <= -3.0 ? Severity::High : Severity::Medium;
            f.worst_value = worst;
            f.worst_row = worst_row;
            f.detail = "Fine/learned knock correction reached " + num(worst) +
                       " deg on cyl " + std::to_string(worst_cyl) +
                       " \xE2\x80\x94 the ECU has learned to pull timing here, a sign of "
                       "repeated knock rather than a one-off event.";
            add_context(f, datalog, chans);
            result.findings.push_back(std::move(f));
        }
    }

    // ---- DAM (dynamic advance multiplier) ------------------------------
    if (auto dam = first_of(chans, Role::Dam)) {
        double min_dam = std::numeric_limits<double>::infinity();
        std::optional<std::size_t> worst_row;
        for (std::size_t r = 0; r < rows; ++r) {
            double const v = sample_at(datalog, *dam, r);
            if (std::isnan(v)) {
                continue;
            }
            if (v < min_dam) {
                min_dam = v;
                worst_row = r;
            }
        }
        if (std::isfinite(min_dam)) {
            LogFinding f;
            f.id = "dam";
            f.worst_value = min_dam;
            f.worst_row = worst_row;
            if (min_dam < 0.999) {
                f.title = "DAM below 1.0";
                f.severity = Severity::High;
                f.detail = "Dynamic Advance Multiplier fell to " + num(min_dam, 3) +
                           " \xE2\x80\x94 the ECU has globally de-rated timing. The tune is "
                           "too aggressive for the fuel/conditions, or knock is persistent.";
                add_context(f, datalog, chans);
            } else {
                f.title = "DAM held at 1.0";
                f.severity = Severity::Info;
                f.detail = "Dynamic Advance Multiplier stayed at maximum \xE2\x80\x94 the "
                           "ECU trusts full timing.";
            }
            result.findings.push_back(std::move(f));
        }
    }

    // ---- Boost tracking / overboost ------------------------------------
    if (auto tb = first_of(chans, Role::TargetBoost)) {
        if (auto ab = first_of(chans, Role::ActualBoost)) {
            double worst_over_rel = 0.0;
            double worst_abs_err = 0.0;
            std::optional<std::size_t> worst_row;
            for (std::size_t r = 0; r < rows; ++r) {
                double const t = sample_at(datalog, *tb, r);
                double const a = sample_at(datalog, *ab, r);
                if (std::isnan(t) || std::isnan(a)) {
                    continue;
                }
                double const abs_err = std::fabs(a - t);
                if (abs_err > worst_abs_err) {
                    worst_abs_err = abs_err;
                }
                if (t > 0.5) { // avoid divide-by-near-zero at vacuum
                    double const over_rel = (a - t) / t;
                    if (over_rel > worst_over_rel) {
                        worst_over_rel = over_rel;
                        worst_row = r;
                    }
                }
            }
            LogFinding f;
            f.id = "boost.tracking";
            f.worst_value = worst_over_rel;
            f.worst_row = worst_row;
            if (worst_over_rel > kOverboostRel) {
                f.title = "Overboost";
                f.severity = worst_over_rel > 0.20 ? Severity::High : Severity::Medium;
                f.detail = "Actual boost exceeded target by up to " +
                           num(worst_over_rel * 100.0, 1) +
                           "%. Check the wastegate duty and boost-target tables for a "
                           "spike the closed loop can't catch.";
                add_context(f, datalog, chans);
            } else {
                f.title = "Boost tracked target";
                f.severity = Severity::Info;
                f.detail = "Actual boost stayed within " + num(kOverboostRel * 100.0, 0) +
                           "% of target (worst absolute error " + num(worst_abs_err) +
                           ").";
            }
            result.findings.push_back(std::move(f));
        }
    }

    // ---- AFR: lean under load ------------------------------------------
    if (auto cmd = first_of(chans, Role::CommandedAfr)) {
        if (auto obs = first_of(chans, Role::ObservedAfr)) {
            // Determine a high-load cutoff if load resolves.
            std::optional<std::size_t> load_col = first_of(chans, Role::Load);
            double load_cut = -std::numeric_limits<double>::infinity();
            if (load_col) {
                double lo = std::numeric_limits<double>::infinity();
                double hi = -std::numeric_limits<double>::infinity();
                for (std::size_t r = 0; r < rows; ++r) {
                    double const v = sample_at(datalog, *load_col, r);
                    if (std::isnan(v)) {
                        continue;
                    }
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                if (std::isfinite(lo) && std::isfinite(hi) && hi > lo) {
                    load_cut = lo + (hi - lo) * kHighLoadFraction;
                }
            }
            double worst_lean_rel = 0.0;
            std::optional<std::size_t> worst_row;
            for (std::size_t r = 0; r < rows; ++r) {
                double const c = sample_at(datalog, *cmd, r);
                double const o = sample_at(datalog, *obs, r);
                if (std::isnan(c) || std::isnan(o) || c <= 0.0) {
                    continue;
                }
                if (load_col) {
                    double const lv = sample_at(datalog, *load_col, r);
                    if (std::isnan(lv) || lv < load_cut) {
                        continue;
                    }
                }
                double const lean_rel = (o - c) / c; // higher AFR/lambda = leaner
                if (lean_rel > worst_lean_rel) {
                    worst_lean_rel = lean_rel;
                    worst_row = r;
                }
            }
            if (worst_lean_rel > kLeanRel) {
                LogFinding f;
                f.id = "afr.lean";
                f.title = "Lean under load";
                f.severity = worst_lean_rel > 0.08 ? Severity::High : Severity::Medium;
                f.worst_value = worst_lean_rel;
                f.worst_row = worst_row;
                f.detail = "Observed AFR ran up to " + num(worst_lean_rel * 100.0, 1) +
                           "% leaner than commanded under load \xE2\x80\x94 a lean spike at "
                           "load is the classic knock precursor. Check fueling / MAF scaling.";
                add_context(f, datalog, chans);
                result.findings.push_back(std::move(f));
            }
        }
    }

    // Rank most-severe first, then by event count.
    std::stable_sort(result.findings.begin(), result.findings.end(),
                     [](LogFinding const &a, LogFinding const &b) {
                         if (severity_rank(a.severity) != severity_rank(b.severity)) {
                             return severity_rank(a.severity) > severity_rank(b.severity);
                         }
                         return a.event_count > b.event_count;
                     });

    // Headline summary.
    int attention = 0;
    for (auto const &f : result.findings) {
        if (f.severity == Severity::Medium || f.severity == Severity::High) {
            ++attention;
        }
    }
    if (result.channels.empty()) {
        result.summary = "No recognized tuning channels in this log.";
    } else if (attention == 0) {
        result.summary = "Clean log \xE2\x80\x94 no knock, boost, or fueling issues detected.";
    } else {
        result.summary = std::to_string(attention) + " finding(s) need attention.";
    }
    return result;
}

}  // namespace st::library::log_analysis
