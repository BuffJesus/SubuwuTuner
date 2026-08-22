// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_LIBRARY_GUIDED_TASKS_HPP
#define ST_LIBRARY_GUIDED_TASKS_HPP

#include "st/core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace st::library::guided_tasks {

inline constexpr std::string_view kSchema = "subuwutuner.guided-tasks.v1";

struct Record {
    std::string id;
    bool complete{false};
    std::string note;
};

struct State {
    std::vector<Record> tasks;
};

[[nodiscard]] Result<std::string> serialize(State const &state);
[[nodiscard]] Result<State> parse(std::string_view text);

} // namespace st::library::guided_tasks

#endif // ST_LIBRARY_GUIDED_TASKS_HPP
