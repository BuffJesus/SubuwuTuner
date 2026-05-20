// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <st/policy.hpp>

namespace st::policy {

Decision emissions_action(Profile p) noexcept {
    // Mirrors the table in docs/06-legal-ethics.md:
    //
    //   motorsport-only : silent everywhere       (default)
    //   alberta-ca      : badge on save, no flash prompt
    //   eu-roadworthy   : warn on save, confirm on flash
    //   california-us   : confirm + reason on save AND on flash
    switch (p) {
    case Profile::MotorsportOnly:
        return {Action::Silent, Action::Silent};
    case Profile::AlbertaCa:
        return {Action::Badge, Action::Silent};
    case Profile::EuRoadworthy:
        return {Action::Warn, Action::Confirm};
    case Profile::CaliforniaUs:
        return {Action::ConfirmWithReason, Action::ConfirmWithReason};
    }
    return {Action::Silent, Action::Silent};
}

Action engine_safety_on_flash(Profile /*p*/) noexcept {
    // Engine safety is profile-invariant — the linter can't waive a
    // physically-unsafe calibration just because the user is in a
    // permissive jurisdiction. docs/06 calls this out explicitly.
    return Action::Block;
}

std::string_view profile_name(Profile p) noexcept {
    switch (p) {
    case Profile::MotorsportOnly:
        return "motorsport-only";
    case Profile::AlbertaCa:
        return "alberta-ca";
    case Profile::EuRoadworthy:
        return "eu-roadworthy";
    case Profile::CaliforniaUs:
        return "california-us";
    }
    return "motorsport-only";
}

std::optional<Profile> parse_profile(std::string_view s) noexcept {
    if (s == "motorsport-only")
        return Profile::MotorsportOnly;
    if (s == "alberta-ca")
        return Profile::AlbertaCa;
    if (s == "eu-roadworthy")
        return Profile::EuRoadworthy;
    if (s == "california-us")
        return Profile::CaliforniaUs;
    return std::nullopt;
}

} // namespace st::policy
