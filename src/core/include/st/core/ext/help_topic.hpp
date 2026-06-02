// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_HELP_TOPIC_HPP
#define ST_CORE_EXT_HELP_TOPIC_HPP

#include "st/core/result.hpp"

#include <string>
#include <string_view>

namespace st::ext {

// HelpTopic — content provider for one in-app help page.
//
// Topics map 1:1 to panels / modals / commands. The host's help
// registry resolves a topic id (e.g. "editor.table",
// "modal.flash.preflight") to a HelpTopic implementation, which
// returns the markdown body to render.
//
// Built-in topics ship as embedded markdown sourced from `docs/`.
// Out-of-tree topics (e.g. installed-pack help) implement this
// interface to inject their own content.
class HelpTopic {
public:
    virtual ~HelpTopic() = default;

    // Stable topic id. Same convention as panel ids — a dotted name.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Title shown in the help window header.
    [[nodiscard]] virtual std::string_view title() const noexcept = 0;

    // The markdown content for this topic. Implementations may return
    // the body verbatim or compose it from multiple sources. The host
    // re-fetches on every help-window open; implementations should be
    // cheap (cache internally if needed).
    [[nodiscard]] virtual Result<std::string> markdown_body() const = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_HELP_TOPIC_HPP
