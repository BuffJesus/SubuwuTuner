// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_DEFINITION_SOURCE_HPP
#define ST_CORE_EXT_DEFINITION_SOURCE_HPP

#include "st/core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace st::ext {

// One entry in a definition source's catalog. Carries enough metadata
// to display + select a pack without fetching its body.
struct DefinitionPackSummary {
    // Stable identifier (e.g. "subaru.va.wrx.mt.va2017", "subaru.ssm.ecuparams.a2tb000l").
    std::string id;
    // Display name (UI: "VA WRX MT — 2017").
    std::string display_name;
    // Pack version (e.g. "1.0.3"). Free-form; implementations are free
    // to use semver, date, git short SHA.
    std::string version;
    // Optional author / maintainer string.
    std::string author;
};

// DefinitionSource — produces definition pack TOML by id.
//
// A source can be:
//   * the application's bundled directory (filesystem),
//   * a user-local pack directory,
//   * a remote registry over HTTP (with caching),
//   * a directory inside an archive (.zip) embedded in the installer.
//
// The host's PackRegistry composes a chain of sources with a precedence
// order (project local > user > installation > bundled defaults).
// Sources do not need to know about each other.
//
// Implementations should be cheap to construct and cheap on `list()`
// since the host calls list() on every PackRegistry refresh. `fetch()`
// is allowed to be slow (network); callers offload it to a worker.
class DefinitionSource {
public:
    virtual ~DefinitionSource() = default;

    // Stable identifier (e.g. "filesystem:user", "filesystem:bundled",
    // "http:public-registry"). Used in logs + UI to disambiguate the
    // source of a pack.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Enumerate every pack this source can provide. Order is
    // implementation-defined; the host sorts as needed. Empty vector
    // means the source is reachable but has no packs.
    [[nodiscard]] virtual Result<std::vector<DefinitionPackSummary>> list() = 0;

    // Fetch one pack's TOML body. The returned string is the raw
    // TOML text; parsing into a `st::defs::Pack` happens in the host.
    // Returns ErrorCode::FileNotFound (or IoFailure, etc.) if the pack
    // can no longer be retrieved.
    [[nodiscard]] virtual Result<std::string> fetch(std::string_view id) = 0;
};

} // namespace st::ext

#endif // ST_CORE_EXT_DEFINITION_SOURCE_HPP
