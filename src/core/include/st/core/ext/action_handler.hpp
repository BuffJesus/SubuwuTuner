// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_ACTION_HANDLER_HPP
#define ST_CORE_EXT_ACTION_HANDLER_HPP

#include "st/core/result.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace st::ext {

// One named, side-effecting operation triggered by a user gesture,
// hotkey, CLI subcommand, or scripted invocation.
//
// Action ids are stable strings ("project.save", "rom.compare.open",
// "flash.preflight"). Args are a string→string map; richer payloads
// live in side channels (the active project, the current selection,
// etc.) not in args.
//
// One handler per action id. The host's action registry routes a
// dispatch to the matching handler.
class ActionHandler {
public:
    virtual ~ActionHandler() = default;

    // Stable identifier the registry matches against (e.g.
    // "project.save", "flash.preflight").
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Human-readable label (UI: "Save Project").
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;

    // Whether the action can be invoked in the current state. The
    // registry calls this before showing a menu entry as enabled.
    // Default implementations return true; override for stateful
    // actions ("Save" needs a dirty project).
    [[nodiscard]] virtual bool is_available() const noexcept {
        return true;
    }

    // Execute the action. Args carry caller-supplied parameters. The
    // return value distinguishes success (`Status` ok), expected
    // failures the user should see (`PolicyDenied`, `Cancelled`), and
    // internal failures (everything else).
    [[nodiscard]] virtual Status execute(std::unordered_map<std::string, std::string> args) = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_ACTION_HANDLER_HPP
