// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/datalog_session.hpp"

#include <cstdint>
#include <sstream>
#include <toml++/toml.hpp>

namespace st::library::datalog_session {
namespace {

std::string_view operation_name(datalog_csv::DerivedOperation operation) {
    switch (operation) {
    case datalog_csv::DerivedOperation::Subtract:
        return "subtract";
    case datalog_csv::DerivedOperation::Ratio:
        return "ratio";
    case datalog_csv::DerivedOperation::PercentError:
        return "percent_error";
    }
    return "subtract";
}

Result<datalog_csv::DerivedOperation> parse_operation(std::string_view value) {
    if (value == "subtract") {
        return datalog_csv::DerivedOperation::Subtract;
    }
    if (value == "ratio") {
        return datalog_csv::DerivedOperation::Ratio;
    }
    if (value == "percent_error") {
        return datalog_csv::DerivedOperation::PercentError;
    }
    return failure(ErrorCode::ParseError, "Unknown derived-channel operation: " +
                                              std::string{value});
}

} // namespace

Result<std::string> serialize(Session const &session) {
    toml::table doc{{"schema", std::string{kSchema}},
                    {"source_path", session.source_path},
                    {"x_axis", session.x_axis},
                    {"notes", session.notes},
                    {"range_first", static_cast<std::int64_t>(session.range_first)},
                    {"range_last", static_cast<std::int64_t>(session.range_last)}};

    toml::array visible;
    for (auto const &header : session.visible_headers) {
        visible.push_back(header);
    }
    doc.insert("visible_headers", std::move(visible));

    toml::array markers;
    for (auto const &marker : session.markers) {
        markers.push_back(toml::table{{"row", static_cast<std::int64_t>(marker.row)},
                                      {"label", marker.label}});
    }
    doc.insert("marker", std::move(markers));

    toml::array derived;
    for (auto const &channel : session.derived_channels) {
        derived.push_back(toml::table{{"name", channel.name},
                                      {"unit", channel.unit},
                                      {"lhs_header", channel.lhs_header},
                                      {"rhs_header", channel.rhs_header},
                                      {"operation", std::string{operation_name(channel.operation)}}});
    }
    doc.insert("derived_channel", std::move(derived));

    std::ostringstream out;
    out << doc;
    return out.str();
}

Result<Session> parse(std::string_view text) {
    toml::table doc;
    try {
        doc = toml::parse(text);
    } catch (toml::parse_error const &e) {
        return failure(ErrorCode::ParseError, std::string{e.description()});
    }

    auto const schema = doc["schema"].value<std::string>();
    if (!schema.has_value()) {
        return failure(ErrorCode::ParseError, "Log session is missing its schema.");
    }
    if (*schema != kSchema) {
        return failure(ErrorCode::UnsupportedVersion, "Unsupported log-session schema: " + *schema);
    }

    Session result;
    result.source_path = doc["source_path"].value_or(std::string{});
    result.x_axis = doc["x_axis"].value_or(std::string{kAutoAxis});
    result.notes = doc["notes"].value_or(std::string{});
    auto const range_first = doc["range_first"].value_or(std::int64_t{0});
    auto const range_last = doc["range_last"].value_or(std::int64_t{0});
    if (range_first < 0 || range_last < 0) {
        return failure(ErrorCode::ParseError, "Log-session range cannot be negative.");
    }
    result.range_first = static_cast<std::size_t>(range_first);
    result.range_last = static_cast<std::size_t>(range_last);

    if (auto const *visible = doc["visible_headers"].as_array(); visible != nullptr) {
        for (auto const &entry : *visible) {
            if (auto value = entry.value<std::string>(); value.has_value()) {
                result.visible_headers.push_back(std::move(*value));
            }
        }
    }

    if (auto const *markers = doc["marker"].as_array(); markers != nullptr) {
        for (auto const &entry : *markers) {
            auto const *table = entry.as_table();
            if (table == nullptr) {
                return failure(ErrorCode::ParseError, "A marker entry is not a table.");
            }
            auto const row = (*table)["row"].value<std::int64_t>();
            if (!row.has_value() || *row < 0) {
                return failure(ErrorCode::ParseError, "A marker has an invalid row.");
            }
            result.markers.push_back(
                {static_cast<std::size_t>(*row), (*table)["label"].value_or(std::string{})});
        }
    }

    if (auto const *derived = doc["derived_channel"].as_array(); derived != nullptr) {
        for (auto const &entry : *derived) {
            auto const *table = entry.as_table();
            if (table == nullptr) {
                return failure(ErrorCode::ParseError, "A derived-channel entry is not a table.");
            }
            auto const name = (*table)["name"].value<std::string>();
            auto const lhs = (*table)["lhs_header"].value<std::string>();
            auto const rhs = (*table)["rhs_header"].value<std::string>();
            auto const operation_text = (*table)["operation"].value<std::string>();
            if (!name.has_value() || name->empty() || !lhs.has_value() || !rhs.has_value() ||
                !operation_text.has_value()) {
                return failure(ErrorCode::ParseError,
                               "A derived channel is missing a required field.");
            }
            auto operation = parse_operation(*operation_text);
            if (!operation.has_value()) {
                return failure(operation.error());
            }
            result.derived_channels.push_back({*name, (*table)["unit"].value_or(std::string{}),
                                               *lhs, *rhs, *operation});
        }
    }
    return result;
}

Result<std::string> serialize_profiles(std::vector<SignalProfile> const &profiles) {
    toml::table document{{"schema", "subuwutuner.log-profiles.v1"}};
    toml::array rows;
    for (auto const &profile : profiles) {
        if (profile.name.empty()) {
            return failure(ErrorCode::InvalidArgument, "A log profile has an empty name.");
        }
        toml::array headers;
        for (auto const &header : profile.visible_headers) {
            headers.push_back(header);
        }
        rows.push_back(
            toml::table{{"name", profile.name}, {"visible_headers", std::move(headers)}});
    }
    document.insert("profile", std::move(rows));
    std::ostringstream out;
    out << document;
    return out.str();
}

Result<std::vector<SignalProfile>> parse_profiles(std::string_view text) {
    toml::table document;
    try {
        document = toml::parse(text);
    } catch (toml::parse_error const &error) {
        return failure(ErrorCode::ParseError, std::string{error.description()});
    }
    if (document["schema"].value_or(std::string{}) != "subuwutuner.log-profiles.v1") {
        return failure(ErrorCode::UnsupportedVersion, "Unsupported log-profile schema.");
    }
    std::vector<SignalProfile> result;
    if (auto const *profiles = document["profile"].as_array(); profiles != nullptr) {
        for (auto const &node : *profiles) {
            auto const *table = node.as_table();
            auto const name = table == nullptr ? std::optional<std::string>{}
                                               : (*table)["name"].value<std::string>();
            if (!name.has_value() || name->empty()) {
                return failure(ErrorCode::ParseError, "A log profile has no name.");
            }
            SignalProfile profile;
            profile.name = *name;
            if (auto const *headers = (*table)["visible_headers"].as_array();
                headers != nullptr) {
                for (auto const &header : *headers) {
                    if (auto value = header.value<std::string>(); value.has_value()) {
                        profile.visible_headers.push_back(*value);
                    }
                }
            }
            result.push_back(std::move(profile));
        }
    }
    return result;
}

} // namespace st::library::datalog_session
