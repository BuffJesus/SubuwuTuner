// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/defs/pack_registry.hpp"

#include "st/core/error.hpp"
#include "zip_archive.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace st::defs {

namespace {

constexpr std::string_view kTomlExt = ".toml";
constexpr std::string_view kZipExt = ".zip";

// Skip subdirs starting with "_" (e.g. `_archives` is build output, not a
// platform) and the Syncthing `.stfolder` marker.
[[nodiscard]] bool is_platform_dir_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    if (name.front() == '_' || name.front() == '.') {
        return false;
    }
    return true;
}

[[nodiscard]] bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) {
        return false;
    }
    auto const tail = s.substr(s.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        auto const a = static_cast<unsigned char>(tail[i]);
        auto const b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string stem_lower(std::filesystem::path const &p) {
    auto s = p.stem().string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

struct PackRegistry::Impl {
    std::filesystem::path root;
    std::map<std::string, PackEntry, std::less<>> entries;
    // platform -> zip archive (lazily opened during scan).
    std::map<std::string, std::shared_ptr<detail::ZipArchive>, std::less<>> zips;
    std::vector<std::string> warnings;

    void try_insert(PackEntry const &e) {
        auto const it = entries.find(e.id);
        if (it == entries.end()) {
            entries.emplace(e.id, e);
            return;
        }
        // Existing entry wins if it has higher priority (lower enum value).
        if (static_cast<int>(it->second.source) <= static_cast<int>(e.source)) {
            warnings.push_back(
                "duplicate pack id '" + e.id + "': keeping " +
                std::string{source_name(it->second.source)} + " (" +
                it->second.container.string() + "), ignoring " +
                std::string{source_name(e.source)} + " (" +
                e.container.string() + ")");
            return;
        }
        warnings.push_back(
            "duplicate pack id '" + e.id + "': preferring " +
            std::string{source_name(e.source)} + " (" + e.container.string() +
            ") over previously seen " +
            std::string{source_name(it->second.source)} + " (" +
            it->second.container.string() + ")");
        it->second = e;
    }

    static constexpr std::string_view source_name(PackEntry::Source s) noexcept {
        switch (s) {
        case PackEntry::Source::LooseTopLevel:
            return "loose top-level";
        case PackEntry::Source::LoosePlatform:
            return "loose platform";
        case PackEntry::Source::Zipped:
            return "zipped";
        }
        return "?";
    }
};

PackRegistry::PackRegistry(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

PackRegistry::PackRegistry(PackRegistry &&) noexcept = default;
PackRegistry &PackRegistry::operator=(PackRegistry &&) noexcept = default;
PackRegistry::~PackRegistry() = default;

Result<PackRegistry>
PackRegistry::from_directory(std::filesystem::path const &root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"pack_registry: not a directory: "} +
                           root.string());
    }
    auto impl = std::make_unique<Impl>();
    impl->root = root;

    // Pass 1: top-level loose .toml + per-platform .zip discovery.
    for (auto const &child : std::filesystem::directory_iterator{root, ec}) {
        if (ec) {
            break;
        }
        auto const &p = child.path();
        if (child.is_regular_file(ec)) {
            auto const name = p.filename().string();
            if (ends_with_ci(name, kTomlExt)) {
                PackEntry e;
                e.id = stem_lower(p);
                e.platform = "";
                e.source = PackEntry::Source::LooseTopLevel;
                e.container = p;
                impl->try_insert(e);
            } else if (ends_with_ci(name, kZipExt)) {
                auto const platform = stem_lower(p);
                auto open_r = detail::ZipArchive::open(p);
                if (!open_r.has_value()) {
                    impl->warnings.push_back(
                        "could not open archive '" + p.string() +
                        "': " + std::string{open_r.error().message()});
                    continue;
                }
                auto arch = std::make_shared<detail::ZipArchive>(
                    std::move(*open_r));
                for (auto const &entry_name : arch->entries()) {
                    if (!ends_with_ci(entry_name, kTomlExt)) {
                        continue;
                    }
                    PackEntry e;
                    e.id = stem_lower(std::filesystem::path{entry_name});
                    e.platform = platform;
                    e.source = PackEntry::Source::Zipped;
                    e.container = p;
                    impl->try_insert(e);
                }
                impl->zips.emplace(platform, std::move(arch));
            }
        }
    }

    // Pass 2: per-platform loose .toml (overrides Zipped from Pass 1).
    for (auto const &child : std::filesystem::directory_iterator{root, ec}) {
        if (ec) {
            break;
        }
        if (!child.is_directory(ec)) {
            continue;
        }
        auto const platform = child.path().filename().string();
        if (!is_platform_dir_name(platform)) {
            continue;
        }
        for (auto const &grand : std::filesystem::directory_iterator{
                 child.path(), ec}) {
            if (ec) {
                break;
            }
            if (!grand.is_regular_file(ec)) {
                continue;
            }
            auto const &gp = grand.path();
            if (!ends_with_ci(gp.filename().string(), kTomlExt)) {
                continue;
            }
            PackEntry e;
            e.id = stem_lower(gp);
            e.platform = platform;
            e.source = PackEntry::Source::LoosePlatform;
            e.container = gp;
            impl->try_insert(e);
        }
    }

    return PackRegistry{std::move(impl)};
}

std::vector<PackEntry> PackRegistry::list_packs() const {
    std::vector<PackEntry> out;
    out.reserve(impl_->entries.size());
    for (auto const &kv : impl_->entries) {
        out.push_back(kv.second);
    }
    return out;
}

std::vector<std::string> PackRegistry::list_platforms() const {
    std::vector<std::string> out;
    for (auto const &kv : impl_->entries) {
        auto const &p = kv.second.platform;
        if (p.empty()) {
            continue;
        }
        if (std::find(out.begin(), out.end(), p) == out.end()) {
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string>
PackRegistry::list_pack_ids_for_platform(std::string_view platform) const {
    std::vector<std::string> out;
    for (auto const &kv : impl_->entries) {
        if (kv.second.platform == platform) {
            out.push_back(kv.second.id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

PackEntry const *PackRegistry::find_pack(std::string_view id) const noexcept {
    auto const it = impl_->entries.find(id);
    if (it == impl_->entries.end()) {
        return nullptr;
    }
    return &it->second;
}

Result<Definition> PackRegistry::load_pack(std::string_view id) const {
    auto const *entry = find_pack(id);
    if (entry == nullptr) {
        return failure(ErrorCode::FileNotFound,
                       std::string{"pack_registry: no pack with id '"} +
                           std::string{id} + "'");
    }
    switch (entry->source) {
    case PackEntry::Source::LooseTopLevel:
    case PackEntry::Source::LoosePlatform:
        return Definition::from_file(entry->container);
    case PackEntry::Source::Zipped: {
        auto const zit = impl_->zips.find(entry->platform);
        if (zit == impl_->zips.end()) {
            return failure(ErrorCode::Unknown,
                           "pack_registry: archive index lost for platform '" +
                               entry->platform + "'");
        }
        std::string const archived_name = entry->id + std::string{kTomlExt};
        auto bytes_r = zit->second->read_entry_string(archived_name);
        if (!bytes_r.has_value()) {
            return failure(bytes_r.error().code(),
                           "pack_registry: archive entry read failed: " +
                               std::string{bytes_r.error().message()});
        }
        return Definition::from_toml_string(*bytes_r);
    }
    }
    return failure(ErrorCode::Unknown, "pack_registry: unreachable source");
}

std::vector<std::string> const &PackRegistry::warnings() const noexcept {
    return impl_->warnings;
}

} // namespace st::defs
