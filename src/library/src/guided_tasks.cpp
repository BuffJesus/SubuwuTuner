// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/guided_tasks.hpp"

#include <sstream>
#include <toml++/toml.hpp>

namespace st::library::guided_tasks {

Result<std::string> serialize(State const &state) {
    toml::table document{{"schema", std::string{kSchema}}};
    toml::array tasks;
    for (auto const &record : state.tasks) {
        if (record.id.empty()) {
            return failure(ErrorCode::InvalidArgument, "A guided task has an empty id.");
        }
        tasks.push_back(toml::table{{"id", record.id},
                                    {"complete", record.complete},
                                    {"note", record.note}});
    }
    document.insert("task", std::move(tasks));
    std::ostringstream out;
    out << document;
    return out.str();
}

Result<State> parse(std::string_view text) {
    toml::table document;
    try {
        document = toml::parse(text);
    } catch (toml::parse_error const &error) {
        return failure(ErrorCode::ParseError, std::string{error.description()});
    }
    auto const schema = document["schema"].value<std::string>();
    if (!schema.has_value()) {
        return failure(ErrorCode::ParseError, "Guided-task state is missing its schema.");
    }
    if (*schema != kSchema) {
        return failure(ErrorCode::UnsupportedVersion,
                       "Unsupported guided-task schema: " + *schema);
    }
    State result;
    if (auto const *tasks = document["task"].as_array(); tasks != nullptr) {
        for (auto const &node : *tasks) {
            auto const *table = node.as_table();
            if (table == nullptr) {
                return failure(ErrorCode::ParseError, "A guided-task entry is not a table.");
            }
            auto const id = (*table)["id"].value<std::string>();
            if (!id.has_value() || id->empty()) {
                return failure(ErrorCode::ParseError, "A guided-task entry has no id.");
            }
            result.tasks.push_back(
                {*id, (*table)["complete"].value_or(false),
                 (*table)["note"].value_or(std::string{})});
        }
    }
    return result;
}

} // namespace st::library::guided_tasks
