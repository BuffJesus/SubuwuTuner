// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <st/flash/backup_store.hpp>

#include <st/core/crc32.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace st::flash {

namespace {

// Slugify a label into something safe for a filename. Lowercases ASCII,
// replaces anything outside [a-z0-9_-] with '_', and trims runs of '_'.
// Pure, deterministic; tests rely on the output.
std::string slugify(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    char last = 0;
    for (char raw_c : raw) {
        auto const c = static_cast<unsigned char>(raw_c);
        char m{};
        if (c >= 'A' && c <= 'Z') {
            m = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            m = static_cast<char>(c);
        } else {
            m = '_';
        }
        if (m == '_' && last == '_') {
            continue;
        }
        out.push_back(m);
        last = m;
    }
    // Trim trailing underscores.
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

std::string format_iso8601_utc(std::chrono::system_clock::time_point tp) {
    // Convert to time_t and emit YYYY-MM-DDThh:mm:ssZ. We deliberately
    // avoid <chrono>'s fmt support because Apple Clang 16 / libc++ still
    // ships incomplete chrono formatters at the time of writing.
    auto const tt = std::chrono::system_clock::to_time_t(tp);
#if defined(_WIN32)
    std::tm tm_buf{};
    gmtime_s(&tm_buf, &tt);
#else
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string{buf};
}

// Read an entire file as bytes. Returns IoFailure on any error.
Result<std::vector<std::uint8_t>> read_all_bytes(std::filesystem::path const &p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) {
        return failure(ErrorCode::IoFailure,
                       "BackupStore: failed to open '" + p.string() + "' for reading");
    }
    in.seekg(0, std::ios::end);
    auto const sz = in.tellg();
    if (sz < 0) {
        return failure(ErrorCode::IoFailure, "BackupStore: tellg failed on '" + p.string() + "'");
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz));
    if (sz > 0 && !in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(sz))) {
        return failure(ErrorCode::IoFailure, "BackupStore: short read on '" + p.string() + "'");
    }
    return out;
}

Status write_all_bytes(std::filesystem::path const &p, std::span<std::uint8_t const> bytes) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "BackupStore: failed to open '" + p.string() + "' for writing");
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<char const *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        return failure(ErrorCode::IoFailure, "BackupStore: write failed on '" + p.string() + "'");
    }
    return ok();
}

Status write_all_text(std::filesystem::path const &p, std::string_view text) {
    std::ofstream out{p, std::ios::trunc};
    if (!out) {
        return failure(ErrorCode::IoFailure,
                       "BackupStore: failed to open '" + p.string() + "' for writing");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return failure(ErrorCode::IoFailure, "BackupStore: write failed on '" + p.string() + "'");
    }
    return ok();
}

} // namespace

std::string BackupStore::filename_stem(std::string_view created_at, std::string_view ecu_id,
                                       std::string_view label) {
    // Replace ':' in the timestamp with '-' so the filename is portable
    // across Windows + POSIX (Windows disallows ':' in filenames).
    std::string ts;
    ts.reserve(created_at.size());
    for (char c : created_at) {
        ts.push_back(c == ':' ? '-' : c);
    }
    std::string stem = ts + "-" + std::string{ecu_id};
    if (!label.empty()) {
        auto const slug = slugify(label);
        if (!slug.empty()) {
            stem += "-" + slug;
        }
    }
    return stem;
}

std::string BackupStore::format_meta(BackupMeta const &meta) {
    toml::table root;
    root.insert("schema_version", meta.schema_version);
    root.insert("created_at", meta.created_at);
    root.insert("ecu_id", meta.ecu_id);
    if (meta.vin.has_value()) {
        root.insert("vin", *meta.vin);
    }
    if (!meta.label.empty()) {
        root.insert("label", meta.label);
    }
    if (!meta.profile.empty()) {
        root.insert("profile", meta.profile);
    }
    root.insert("size", static_cast<std::int64_t>(meta.size));
    root.insert("crc32", static_cast<std::int64_t>(meta.crc32));
    root.insert("pinned", meta.pinned);

    std::ostringstream oss;
    oss << root;
    return oss.str();
}

Result<BackupMeta> BackupStore::parse_meta(std::string_view toml_text) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml_text);
    } catch (toml::parse_error const &e) {
        return failure(ErrorCode::ParseError,
                       std::string{"BackupStore: TOML parse: "} + e.description().data());
    }

    BackupMeta meta;
    if (auto const v = tbl["schema_version"].value<std::int64_t>(); v) {
        meta.schema_version = static_cast<int>(*v);
    } else {
        return failure(ErrorCode::ParseError, "BackupStore: missing schema_version");
    }
    if (meta.schema_version != kBackupSchemaVersion) {
        return failure(ErrorCode::UnsupportedVersion,
                       "BackupStore: unsupported schema_version " +
                           std::to_string(meta.schema_version));
    }

    auto const required_string = [&](char const *key) -> Result<std::string> {
        if (auto const v = tbl[key].value<std::string>(); v) {
            return *v;
        }
        return failure(ErrorCode::ParseError, std::string{"BackupStore: missing "} + key);
    };

    auto created_at = required_string("created_at");
    if (!created_at) {
        return failure(created_at.error());
    }
    meta.created_at = std::move(*created_at);

    auto ecu_id = required_string("ecu_id");
    if (!ecu_id) {
        return failure(ecu_id.error());
    }
    meta.ecu_id = std::move(*ecu_id);

    if (auto const v = tbl["vin"].value<std::string>(); v) {
        meta.vin = *v;
    }
    if (auto const v = tbl["label"].value<std::string>(); v) {
        meta.label = *v;
    }
    if (auto const v = tbl["profile"].value<std::string>(); v) {
        meta.profile = *v;
    }
    if (auto const v = tbl["size"].value<std::int64_t>(); v) {
        meta.size = static_cast<std::uint64_t>(*v);
    } else {
        return failure(ErrorCode::ParseError, "BackupStore: missing size");
    }
    if (auto const v = tbl["crc32"].value<std::int64_t>(); v) {
        meta.crc32 = static_cast<std::uint32_t>(*v);
    } else {
        return failure(ErrorCode::ParseError, "BackupStore: missing crc32");
    }
    if (auto const v = tbl["pinned"].value<bool>(); v) {
        meta.pinned = *v;
    }
    return meta;
}

Result<BackupMeta> BackupStore::read_meta(std::filesystem::path const &header_path) {
    std::ifstream in{header_path};
    if (!in) {
        return failure(ErrorCode::FileNotFound,
                       "BackupStore: header not found: " + header_path.string());
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return parse_meta(oss.str());
}

Result<Backup> BackupStore::create(std::span<std::uint8_t const> bytes, std::string ecu_id,
                                   std::optional<std::string> vin, std::string label,
                                   std::string profile, std::chrono::system_clock::time_point now) {
    if (bytes.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "BackupStore::create: refuse to back up an empty buffer");
    }
    if (ecu_id.empty()) {
        return failure(ErrorCode::InvalidArgument,
                       "BackupStore::create: ecu_id is required for filename + audit trail");
    }

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return failure(ErrorCode::IoFailure, "BackupStore: create_directories('" + dir_.string() +
                                                 "'): " + ec.message());
    }

    BackupMeta meta;
    meta.schema_version = kBackupSchemaVersion;
    meta.created_at = format_iso8601_utc(now);
    meta.ecu_id = ecu_id;
    meta.vin = std::move(vin);
    meta.label = label;
    meta.profile = std::move(profile);
    meta.size = bytes.size();
    meta.crc32 = crc32(bytes);
    meta.pinned = false;

    auto const stem = filename_stem(meta.created_at, meta.ecu_id, meta.label);
    auto const header_path = dir_ / (stem + ".stbackup");
    auto const data_path = dir_ / (stem + ".bin");

    if (auto s = write_all_bytes(data_path, bytes); !s) {
        return failure(s.error());
    }

    // Re-read and re-hash the persisted .bin BEFORE writing the header.
    // Writing the header first would leave a half-baked backup on disk
    // if the readback fails.
    auto const re = read_all_bytes(data_path);
    if (!re) {
        return failure(re.error());
    }
    if (re->size() != meta.size || crc32(*re) != meta.crc32) {
        // Best-effort cleanup of the bad .bin so we don't litter.
        std::error_code rm_ec;
        std::filesystem::remove(data_path, rm_ec);
        return failure(ErrorCode::BadChecksum,
                       "BackupStore::create: post-write verify mismatch for " + data_path.string());
    }

    if (auto s = write_all_text(header_path, format_meta(meta)); !s) {
        // Remove the .bin so callers don't see an orphan.
        std::error_code rm_ec;
        std::filesystem::remove(data_path, rm_ec);
        return failure(s.error());
    }

    return Backup{header_path, data_path, std::move(meta)};
}

Result<void> BackupStore::verify(Backup const &backup) const {
    auto const bytes = read_all_bytes(backup.data_path);
    if (!bytes) {
        return failure(bytes.error());
    }
    if (bytes->size() != backup.meta.size) {
        return failure(ErrorCode::BadChecksum,
                       "BackupStore::verify: size mismatch (header " +
                           std::to_string(backup.meta.size) + ", actual " +
                           std::to_string(bytes->size()) + ")");
    }
    auto const observed = crc32(*bytes);
    if (observed != backup.meta.crc32) {
        return failure(ErrorCode::BadChecksum, "BackupStore::verify: CRC32 mismatch");
    }
    return ok();
}

Result<void> BackupStore::set_pinned(Backup &backup, bool pinned) {
    backup.meta.pinned = pinned;
    if (auto s = write_all_text(backup.header_path, format_meta(backup.meta)); !s) {
        return failure(s.error());
    }
    return ok();
}

std::vector<Backup> BackupStore::list() const {
    std::vector<Backup> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec) || ec) {
        return out;
    }
    for (auto const &entry : std::filesystem::directory_iterator{dir_, ec}) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        auto const &p = entry.path();
        if (p.extension() != ".stbackup") {
            continue;
        }
        auto meta = read_meta(p);
        if (!meta) {
            continue;
        }
        auto data_path = p;
        data_path.replace_extension(".bin");
        out.push_back(Backup{p, std::move(data_path), std::move(*meta)});
    }
    std::sort(out.begin(), out.end(),
              [](Backup const &a, Backup const &b) { return a.meta.created_at < b.meta.created_at; });
    return out;
}

std::size_t BackupStore::evict_old(int max_unpinned) {
    if (max_unpinned < 0) {
        max_unpinned = 0;
    }
    auto entries = list();
    // Count unpinned; remove the oldest while we have more than allowed.
    std::size_t deleted = 0;
    std::vector<Backup *> unpinned_oldest_first;
    unpinned_oldest_first.reserve(entries.size());
    for (auto &b : entries) {
        if (!b.meta.pinned) {
            unpinned_oldest_first.push_back(&b);
        }
    }
    while (static_cast<int>(unpinned_oldest_first.size()) > max_unpinned) {
        auto *victim = unpinned_oldest_first.front();
        std::error_code ec;
        std::filesystem::remove(victim->header_path, ec);
        std::filesystem::remove(victim->data_path, ec);
        unpinned_oldest_first.erase(unpinned_oldest_first.begin());
        ++deleted;
    }
    return deleted;
}

} // namespace st::flash
