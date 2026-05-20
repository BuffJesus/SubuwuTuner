// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Jurisdiction profiles + emissions/engine-safety policy. The matrix here
// is the implementation of the EmissionsLinter table in
// `docs/06-legal-ethics.md`: each profile decides what happens when a
// flagged edit is saved or flashed. The data layer (Table.emissions_relevant,
// Table.engine_safety_critical) supplies the flag; the GUI / flash
// orchestrator consults this module to decide whether to badge, warn,
// confirm, require a reason, or block.

#ifndef ST_POLICY_HPP
#define ST_POLICY_HPP

#include <optional>
#include <string_view>

namespace st::policy {

// Bundled profiles. New ones can be added without breaking ABI as long as
// they come after the existing values. `MotorsportOnly` is the first-run
// default (least restrictive) per docs/06.
enum class Profile {
    MotorsportOnly = 0,
    AlbertaCa = 1,
    EuRoadworthy = 2,
    CaliforniaUs = 3,
};

// What the linter wants the host to do when a flagged edit is saved or
// queued for flash. Ordered loosely from least to most restrictive; callers
// can compare with `<` to escalate a per-flag decision under a stricter
// profile.
enum class Action {
    Silent = 0,            // do nothing
    Badge = 1,             // show a passive indicator (yellow corner)
    Warn = 2,              // emit a warning message, no confirmation
    Confirm = 3,           // require a one-click confirmation
    ConfirmWithReason = 4, // require confirmation + free-text reason
    Block = 5,             // refuse the action outright
};

// Decision for a single concern, split by lifecycle stage.
struct Decision {
    Action on_save;
    Action on_flash;
};

// Action to take when an emissions-flagged edit (Table.emissions_relevant
// = true) is saved or queued for flash, under the given profile.
[[nodiscard]] Decision emissions_action(Profile p) noexcept;

// Action to take when an engine-safety-flagged edit
// (Table.engine_safety_critical = true) is queued for flash. Per docs/06,
// engine-safety violations stay blocking by default in every profile —
// they're a different category from emissions and the linter can't relax
// them just because the user is in a permissive jurisdiction.
[[nodiscard]] Action engine_safety_on_flash(Profile p) noexcept;

// Stable string IDs used in config persistence (project.toml /
// settings.toml). Round-trip: `parse_profile(profile_name(p)) == p`.
[[nodiscard]] std::string_view profile_name(Profile p) noexcept;
[[nodiscard]] std::optional<Profile> parse_profile(std::string_view s) noexcept;

} // namespace st::policy

#endif // ST_POLICY_HPP
