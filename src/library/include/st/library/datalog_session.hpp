// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LIBRARY_DATALOG_SESSION_HPP
#define ST_LIBRARY_DATALOG_SESSION_HPP

#include "st/core/result.hpp"
#include "st/library/datalog_csv.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace st::library::datalog_session {

inline constexpr std::string_view kSchema = "subuwutuner.log-session.v1";
inline constexpr std::string_view kAutoAxis = "@auto";
inline constexpr std::string_view kSampleAxis = "@sample";

struct Marker {
    std::size_t row{0};
    std::string label;
};

struct DerivedChannel {
    std::string name;
    std::string unit;
    std::string lhs_header;
    std::string rhs_header;
    datalog_csv::DerivedOperation operation{datalog_csv::DerivedOperation::Subtract};
};

struct Session {
    std::string source_path;
    std::vector<std::string> visible_headers;
    std::string x_axis{std::string{kAutoAxis}};
    std::string notes;
    std::size_t range_first{0};
    std::size_t range_last{0}; // exclusive; zero means the full log
    std::vector<Marker> markers;
    std::vector<DerivedChannel> derived_channels;
};

struct SignalProfile {
    std::string name;
    std::vector<std::string> visible_headers;
};

[[nodiscard]] Result<std::string> serialize(Session const &session);
[[nodiscard]] Result<Session> parse(std::string_view text);
[[nodiscard]] Result<std::string> serialize_profiles(std::vector<SignalProfile> const &profiles);
[[nodiscard]] Result<std::vector<SignalProfile>> parse_profiles(std::string_view text);

} // namespace st::library::datalog_session

#endif // ST_LIBRARY_DATALOG_SESSION_HPP
