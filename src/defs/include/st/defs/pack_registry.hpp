// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::defs::PackRegistry — overlay-load definition packs from a directory
// that contains a mix of loose .toml files and per-platform ZIP archives.
//
// Layout the registry understands:
//
//   <root>/
//     <id>.toml                  (top-level loose; platform = "")
//     <platform>/                (per-platform subdir)
//       <id>.toml                (per-platform loose)
//     <platform>.zip             (per-platform archive of <id>.toml entries)
//
// Lookup priority for an id (loose always wins over zipped):
//   1. <root>/<id>.toml
//   2. <root>/<platform>/<id>.toml  (any platform — first match wins;
//                                     conflicts emit a warning)
//   3. <root>/<platform>.zip entry for <id>.toml
//
// This lets end-users ship the install with `<platform>.zip` bundles
// (~6 MB total instead of ~71 MB loose; see docs/17 §7 and
// tools/build_definition_archives.py) AND drop a loose `<id>.toml`
// alongside to override the bundled pack — no rebuild required.
//
// Construction is eager: scanning the root + opening every zip happens
// in `from_directory`. Loading a pack is lazy: the TOML is parsed on
// demand inside `load_pack` (no caching today; the caller is responsible
// for holding onto the returned Definition if they want it).

#ifndef ST_DEFS_PACK_REGISTRY_HPP
#define ST_DEFS_PACK_REGISTRY_HPP

#include "st/core/result.hpp"
#include "st/defs.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::defs {

struct PackEntry {
    enum class Source {
        LooseTopLevel,   // <root>/<id>.toml
        LoosePlatform,   // <root>/<platform>/<id>.toml
        Zipped,          // <root>/<platform>.zip entry <id>.toml
    };

    std::string id;
    std::string platform;             // "" for LooseTopLevel
    Source source{Source::Zipped};
    std::filesystem::path container;  // loose: file path; zip: archive path
};

class PackRegistry {
public:
    [[nodiscard]] static Result<PackRegistry>
    from_directory(std::filesystem::path const &definitions_root);

    [[nodiscard]] std::vector<PackEntry> list_packs() const;
    [[nodiscard]] std::vector<std::string> list_platforms() const;
    [[nodiscard]] std::vector<std::string>
    list_pack_ids_for_platform(std::string_view platform) const;

    // Lookup by id without parsing.
    [[nodiscard]] PackEntry const *find_pack(std::string_view id) const noexcept;

    // Load + parse a pack by id, applying the overlay rule. Returns
    // failure(NotFound) if no source has the id.
    [[nodiscard]] Result<Definition> load_pack(std::string_view id) const;

    // Warnings collected during from_directory (e.g. duplicate ids across
    // platforms, unreadable zips). Non-fatal — registry is still usable.
    [[nodiscard]] std::vector<std::string> const &warnings() const noexcept;

    PackRegistry(PackRegistry const &) = delete;
    PackRegistry &operator=(PackRegistry const &) = delete;
    PackRegistry(PackRegistry &&) noexcept;
    PackRegistry &operator=(PackRegistry &&) noexcept;
    ~PackRegistry();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PackRegistry(std::unique_ptr<Impl>) noexcept;
};

} // namespace st::defs

#endif // ST_DEFS_PACK_REGISTRY_HPP
