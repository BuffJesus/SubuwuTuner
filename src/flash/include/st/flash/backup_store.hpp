// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_FLASH_BACKUP_STORE_HPP
#define ST_FLASH_BACKUP_STORE_HPP

#include "st/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// =====================================================================
// st::flash::BackupStore — mandatory pre-write backup with verify gate
// =====================================================================
//
// A `Backup` is a typed, on-disk artifact that captures the full bytes of
// an ECU's flash at a specific moment, alongside enough metadata to
// reason about WHERE those bytes came from. Backups are produced by
// `BackupStore::create` immediately AFTER a successful pre-flash
// `read_full_rom*` and BEFORE the flash orchestrator is invoked.
//
// Safety contract:
//   * `BackupStore::create` returns a fully-verified handle, or an error.
//     "Verified" means: the bytes were written to disk and re-read back
//     match the bytes the caller handed in, CRC32-equal.
//   * Callers MUST refuse to proceed with a flash if `create` fails.
//   * `flash_preflight`'s `BackupStorePresent` validator checks for the
//     existence of a verified backup for the same ECU within a configured
//     freshness window.
//
// Persistence layout (relative to the store root directory):
//   <root>/
//     <utc_ts>-<ecu_id>[-<label>].stbackup        # TOML header
//     <utc_ts>-<ecu_id>[-<label>].bin             # binary payload
//
// The TOML header carries metadata (ECU ID, VIN if known, byte size,
// CRC32, ISO-8601 UTC timestamp, optional label, `pinned` flag, source
// profile name). The binary file holds the raw ROM bytes. Splitting the
// payload from the header keeps the header diff-friendly and lets verify
// re-hash only the binary without re-parsing TOML.
//
// Retention: `BackupStore::evict_old(max_unpinned)` removes the oldest
// non-pinned backups until at most `max_unpinned` remain. Pinned backups
// are never evicted. Eviction is opt-in — `create` never deletes.
//
// Threading: `BackupStore` is not thread-safe. Callers should serialize
// access to the same directory.

namespace st::flash {

// Metadata describing one persisted backup. Mirrored 1:1 in the TOML
// header file.
struct BackupMeta {
    int schema_version{1};
    // ISO-8601 UTC ("2026-06-01T20:15:30Z"). Opaque round-tripped string;
    // the in-memory representation does not parse it.
    std::string created_at{};
    // ECU identification at the moment the backup was captured. ECU ID
    // is the family/cal-ID string the ECU reports over UDS or SSM (e.g.
    // "A2WC520N"). VIN is optional — some older ECUs do not expose it,
    // and many SH-2A and earlier Subarus do not have a VIN field in
    // flash at all.
    std::string ecu_id{};
    std::optional<std::string> vin{};
    // Optional user-supplied label (e.g. "before-stage1", "shop-loaner").
    // Round-tripped as-is; used as a suffix in the filename if present.
    std::string label{};
    // The active jurisdiction profile name at backup time. Useful for
    // audit when a tune is restored from backup.
    std::string profile{};
    // Bytes are persisted in a sidecar binary file named identically to
    // the header but with `.bin` extension. `size` is the byte count;
    // `crc32` is the IEEE 802.3 CRC32 over the sidecar's contents.
    std::uint64_t size{0};
    std::uint32_t crc32{0};
    // When true, this backup is never auto-evicted by `evict_old`. The
    // flag is hand-toggled by the user or the host via `set_pinned`.
    bool pinned{false};
};

// A successful backup handle. Holds the paths to both files plus the
// metadata for in-memory inspection.
struct Backup {
    std::filesystem::path header_path{}; // <root>/<...>.stbackup (TOML)
    std::filesystem::path data_path{};   // <root>/<...>.bin
    BackupMeta meta{};
};

inline constexpr int kBackupSchemaVersion = 1;

// Default max_unpinned for `evict_old`. Chosen low enough to be useful
// (most tuners keep < 10 backups before pinning the keepers) and high
// enough that it does not feel destructive.
inline constexpr int kDefaultRetentionUnpinned = 10;

// =====================================================================
// BackupStore — directory-scoped collection of Backup entries
// =====================================================================

class BackupStore {
public:
    // Construct a store rooted at `dir`. The directory is NOT created
    // here; `create()` will create it on first write. Pass an existing
    // directory (e.g. `<project_dir>/backups/`) so the project owns the
    // location.
    explicit BackupStore(std::filesystem::path dir) noexcept : dir_{std::move(dir)} {}

    [[nodiscard]] std::filesystem::path const &dir() const noexcept {
        return dir_;
    }

    // Create + verify a new backup. The verify step:
    //   1. Writes `bytes` to `<dir>/<ts>-<ecu_id>[-<label>].bin`.
    //   2. Writes the TOML header alongside it.
    //   3. Re-reads the .bin and recomputes CRC32; rejects if the value
    //      differs from what was computed before the write.
    //
    // Errors surface as:
    //   * IoFailure          — failed to write either file, or failed to
    //                          create the directory.
    //   * BadChecksum        — re-read CRC32 did not match the expected.
    //   * InvalidArgument    — `bytes` empty or `ecu_id` empty.
    //
    // `now` is injectable so tests can pin the timestamp. Production
    // callers should pass `std::chrono::system_clock::now()`.
    [[nodiscard]] Result<Backup> create(std::span<std::uint8_t const> bytes, std::string ecu_id,
                                        std::optional<std::string> vin = std::nullopt,
                                        std::string label = {}, std::string profile = {},
                                        std::chrono::system_clock::time_point now =
                                            std::chrono::system_clock::now());

    // Re-verify an existing backup on disk. Reads the .bin, recomputes
    // CRC32, compares against the header. `BadChecksum` on mismatch.
    [[nodiscard]] Result<void> verify(Backup const &backup) const;

    // Toggle the `pinned` flag on disk. The header is rewritten in
    // place; the .bin file is untouched.
    [[nodiscard]] Result<void> set_pinned(Backup &backup, bool pinned);

    // Walk the directory and return every backup whose TOML header
    // parses cleanly. Malformed headers are silently skipped (best-
    // effort discovery; callers who care can use `read_meta` directly).
    // Sorted oldest-first by `created_at`.
    [[nodiscard]] std::vector<Backup> list() const;

    // Remove non-pinned backups until at most `max_unpinned` remain.
    // Returns the number of backups deleted. Errors during deletion
    // are NOT surfaced — eviction is best-effort and never blocks the
    // flash workflow.
    std::size_t evict_old(int max_unpinned = kDefaultRetentionUnpinned);

    // Free functions exposed for tests + recovery tooling.
    [[nodiscard]] static Result<BackupMeta> parse_meta(std::string_view toml_text);
    [[nodiscard]] static std::string format_meta(BackupMeta const &meta);
    [[nodiscard]] static Result<BackupMeta> read_meta(std::filesystem::path const &header_path);

    // The filename stem ("<ts>-<ecu_id>[-<label>]") used by both the
    // header and the data file. Exposed for tests that pre-create
    // backups by hand.
    [[nodiscard]] static std::string filename_stem(std::string_view created_at,
                                                   std::string_view ecu_id,
                                                   std::string_view label);

private:
    std::filesystem::path dir_;
};

} // namespace st::flash

#endif // ST_FLASH_BACKUP_STORE_HPP
