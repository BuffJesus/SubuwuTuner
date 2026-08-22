// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::library::log_analysis — tuning-domain datalog analysis.
//
// The datalog_csv layer gives generic per-channel stats and arithmetic-
// derived channels. This layer adds the tuning-specific reading a tuner
// actually cares about: did the engine knock, did the ECU pull global
// timing (DAM), did boost track its target, did fueling stay on the
// commanded AFR? It resolves channel roles from header names (logger
// column naming varies) and produces plain-language findings ranked by
// severity, so the UI can show "3 active knock-retard events, worst
// -2.1 deg on cyl 1 at 5200 rpm" instead of a wall of numbers.
//
// Pure and UI-free: operates on a parsed datalog, returns data. Grounded
// in the corpus datalog-mining track (findings/tuning-knowledge-2026-06-13).

#ifndef ST_LIBRARY_LOG_ANALYSIS_HPP
#define ST_LIBRARY_LOG_ANALYSIS_HPP

#include "st/library/datalog_csv.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::library::log_analysis {

// Semantic role a datalog column plays. A role can map to several
// columns (e.g. FeedbackKnock -> fbkc1..fbkc4).
enum class Role {
    Rpm,
    Load,
    FeedbackKnock, // active knock correction (negative = pulling timing now)
    FineKnock,     // fine/learned knock correction (negative = learned pull)
    Dam,           // dynamic advance multiplier (< 1.0 = global timing pull)
    TargetBoost,
    ActualBoost,
    CommandedAfr, // commanded / target AFR (or lambda)
    ObservedAfr,  // observed / wideband AFR (or lambda)
    Unknown,
};

enum class Severity {
    Info,   // reassuring ("no knock seen")
    Low,
    Medium,
    High,   // needs attention before the next pull
};

[[nodiscard]] std::string_view to_string(Role role) noexcept;
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

// A resolved (role, column) binding. `header` is the original column
// name so the UI can show what matched.
struct ResolvedChannel {
    Role role{Role::Unknown};
    std::size_t column{0};
    std::string header;
};

// One tuning finding. `event_count` is how many samples tripped it;
// `worst_value` / `worst_row` locate the single worst sample (with
// rpm/load context filled when those channels resolve).
struct LogFinding {
    std::string id;    // stable slug, e.g. "knock.feedback"
    std::string title; // short label
    Severity severity{Severity::Info};
    std::string detail; // plain-language, one or two sentences
    std::size_t event_count{0};
    double worst_value{0.0};
    std::optional<std::size_t> worst_row;
    std::optional<double> at_rpm;
    std::optional<double> at_load;
};

struct LogAnalysis {
    std::vector<ResolvedChannel> channels;
    std::vector<LogFinding> findings; // most-severe first, then event_count
    std::string summary;              // one-line headline
};

// Resolve every column's role from its header name. Case-insensitive,
// alias-based. Columns that match nothing are omitted (not returned as
// Unknown).
[[nodiscard]] std::vector<ResolvedChannel>
resolve_channels(datalog_csv::ParsedDatalog const &datalog);

// Full tuning analysis of a parsed datalog.
[[nodiscard]] LogAnalysis analyze(datalog_csv::ParsedDatalog const &datalog);

}  // namespace st::library::log_analysis

#endif  // ST_LIBRARY_LOG_ANALYSIS_HPP
