// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// AuditLog — append-only NDJSON log of every operation that touches
// an ECU. Per-project (lives at `<project_dir>/audit.log`). Per-
// entry CRC32 checksum so post-hoc tampering is detectable. Closes
// analyst Issue #8 (cross-session ECU-touch audit log; the existing
// per-flash journal is its own thing).
//
// File format: one JSON object per line, no trailing comma, UTF-8.
// Each line shape (single line on disk; pretty here for docs):
//
//   {
//     "kind": "flash.started",
//     "ts": 1700000000000000000,
//     "source": "flash",
//     "desc": "Flash of plan.toml started",
//     "fields": [["plan_path","/path/to/plan.toml"], ["sectors","4"]],
//     "checksum": 3243443414
//   }
//
// The checksum is CRC32 over the canonical field encoding minus the
// checksum field itself, so flipping any byte (kind / timestamp /
// source / desc / a fields entry) surfaces as a tampered entry on
// re-read.
//
// v1 ships the library + read/append API + CRC verify. Per-module
// auto-subscriber wiring (transport / ecu / flash / log all post
// entries automatically) is incremental; the API is shaped so each
// module can append its own events without coordination.

#ifndef ST_AUDIT_HPP
#define ST_AUDIT_HPP

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace st::audit {

// Enum tag for the entry kind. Wire format uses dotted string IDs
// (kind_name()) so the on-disk representation is stable even when
// the enum shape evolves; readers ignore unknown kinds rather than
// failing.
enum class EntryKind : std::uint8_t {
    Custom = 0,
    EcuConnected,
    EcuDisconnected,
    SecurityAccessUnlocked,
    BackupCreated,
    BackupVerified,
    FlashStarted,
    FlashSectorWritten,
    FlashCompleted,
    FlashFailed,
    FlashCancelled,
    DatalogStarted,
    DatalogStopped,
    ChecksumRecomputed,
    // Project-lifecycle hooks (analyst Issue #8). Appended by the GUI
    // (and CLI verbs that operate on projects) so audit.log captures
    // user-initiated mutations as they happen, not just transport-level
    // events. AutotuneCommitted is the highest-signal one: it's the
    // moment a knock-pull / MAF run lands in the project's edit
    // history, irreversibly changing the working ROM bytes.
    ProjectOpened,
    ProjectSaved,
    EditCommitted,
    AutotuneCommitted,
    // ROM-read-from-car lifecycle (analyst Issue #8, GUI slice). The
    // counterpart to Flash* — captures every read operation against
    // the connected ECU. RomReadCompleted records the read size +
    // CRC32 so post-hoc verification of "did we ever pull this ROM"
    // doesn't depend on disk artifacts.
    RomReadStarted,
    RomReadCompleted,
    RomReadFailed,
    RomReadCancelled,
};

[[nodiscard]] std::string_view kind_name(EntryKind k) noexcept;
[[nodiscard]] EntryKind kind_from_name(std::string_view name) noexcept;

// One audit entry. `fields` is a flat list of (key, value) pairs —
// not a map — so the wire format is order-stable and the checksum is
// reproducible. `timestamp_ns` is nanoseconds since UNIX epoch (the
// audit log's clock; not the LiveBuffer's relative-monotonic clock).
struct Entry {
    EntryKind kind{EntryKind::Custom};
    std::int64_t timestamp_ns{0};
    std::string source;
    std::string description;
    // ECU calibration ID (e.g. "LF79103P") this entry is about. Empty
    // when not applicable (transport-level events with no active ECU
    // identity yet). Drives the per-CID cross-session sink at
    // `<config>/audit-by-cid/<CID>.log` so a tech can pull the full
    // cross-project history for one car. Empty cid is NOT written to
    // the wire so legacy (pre-cid) logs still verify their checksums.
    std::string cid;
    // Vehicle identification number, when known. Many tunes are
    // ECU-only (no VIN exchange) so this is frequently empty. Same
    // empty-skip rule as cid for backward-compat.
    std::string vin;
    std::vector<std::pair<std::string, std::string>> fields;

    // Populated only on read — true when the on-disk CRC32 doesn't
    // match the computed value (indicates tampering / corruption).
    bool checksum_valid{true};
    // Read-side: the CRC32 as it appeared on disk. Append-side
    // ignores this — append() computes a fresh CRC32 on serialize.
    std::uint32_t checksum_on_disk{0};
};

// Compute the canonical CRC32 for an Entry — covers every field
// except the checksum itself. Exposed so callers can do extra
// verification (e.g. re-checksum entries fetched from cold storage).
[[nodiscard]] std::uint32_t entry_checksum(Entry const &e) noexcept;

// Serialize an Entry to its single-line NDJSON wire form. Does NOT
// include a trailing newline; append() handles that.
[[nodiscard]] std::string serialize_entry(Entry const &e);

// Append-only audit log. Holds a file handle; append() flushes after
// each write so a crash mid-session loses at most the in-flight
// entry. Not thread-safe for concurrent append from multiple
// threads — callers serialize via their own mutex (or per-subsystem
// AuditLog handles, one per writer).
class AuditLog {
public:
    // Open / create the log file at `path`. The directory must already
    // exist; AuditLog does not mkdir. Appends are O_APPEND-equivalent
    // (std::ios::app).
    [[nodiscard]] static Result<AuditLog> open(std::filesystem::path const &path);

    AuditLog(AuditLog const &) = delete;
    AuditLog &operator=(AuditLog const &) = delete;
    AuditLog(AuditLog &&) noexcept = default;
    AuditLog &operator=(AuditLog &&) noexcept = default;

    [[nodiscard]] std::filesystem::path const &path() const noexcept {
        return path_;
    }

    // Append a single entry. Computes the CRC32, serializes, writes
    // one line + trailing '\n', flushes. Sets entry.timestamp_ns
    // automatically if the caller left it at 0 (= "now"). Stamps
    // default_cid / default_vin onto the entry when its own cid/vin
    // are empty (configure via set_default_cid / set_default_vin).
    // When a per-CID sink dir is configured (set_per_cid_sink), the
    // entry is also teed into per_cid_log_path(dir, entry.cid).
    [[nodiscard]] Status append(Entry entry);

    // Convenience: append with just a kind + description + optional
    // source + fields. Timestamps to "now".
    [[nodiscard]] Status log(EntryKind kind, std::string source, std::string description,
                             std::vector<std::pair<std::string, std::string>> fields = {});

    // Default CID stamped onto every appended entry whose own
    // entry.cid is empty. Set once at project-open / profile-switch
    // time so every downstream subsystem (UDS / Flasher / GUI panels)
    // benefits without threading the CID through their call sites.
    // Empty string clears the default.
    void set_default_cid(std::string cid) noexcept {
        default_cid_ = std::move(cid);
    }
    void set_default_vin(std::string vin) noexcept {
        default_vin_ = std::move(vin);
    }
    [[nodiscard]] std::string const &default_cid() const noexcept {
        return default_cid_;
    }
    [[nodiscard]] std::string const &default_vin() const noexcept {
        return default_vin_;
    }

    // Configure a per-CID cross-session sink. When set, append() also
    // writes each entry to per_cid_log_path(dir, entry.cid) — the
    // tee that powers the "Audit (per-vehicle history)" GUI panel.
    // Empty path clears the sink (project-log-only mode).
    void set_per_cid_sink(std::filesystem::path dir) {
        per_cid_dir_ = std::move(dir);
    }
    [[nodiscard]] std::filesystem::path const &per_cid_sink() const noexcept {
        return per_cid_dir_;
    }

    // Read every entry from disk. Per-entry checksum is verified;
    // entries with bad checksums come back with checksum_valid=false
    // rather than rejecting the whole read — the caller decides
    // whether to surface or skip them.
    [[nodiscard]] Result<std::vector<Entry>>
    read_all(std::filesystem::path const &path);

private:
    AuditLog() = default;
    std::filesystem::path path_;
    std::ofstream out_;
    std::string default_cid_;
    std::string default_vin_;
    std::filesystem::path per_cid_dir_;
};

// Free-function reader for tools that don't want to open the log
// for append. Same semantics as AuditLog::read_all.
[[nodiscard]] Result<std::vector<Entry>>
read_all(std::filesystem::path const &path);

// ---- Per-CID cross-session sink ----------------------------------------
//
// Closes analyst Issue #8 (cross-session AuditLog). Each project still
// has its own `audit.log`, but entries also tee into a centralized
// per-CID file under a caller-provided directory (typically
// `<config>/audit-by-cid/`) so a tech can pull the full history for one
// car across every project it appeared in.

// Compute the per-CID sink path under `per_cid_dir`. Sanitizes the CID
// to a filesystem-safe slug: chars outside [A-Za-z0-9._-] become '_'.
// Empty cid returns an empty path. The per_cid_dir is NOT created here.
[[nodiscard]] std::filesystem::path
per_cid_log_path(std::filesystem::path const &per_cid_dir, std::string_view cid);

// Tee-append. Always writes `entry` to `project_log`; additionally
// appends to per_cid_log_path(per_cid_dir, entry.cid) when entry.cid is
// non-empty. Creates per_cid_dir if missing. The project-log write is
// attempted first so a per-CID hiccup never loses the local entry.
// Returns the first failure encountered, or ok() on success.
[[nodiscard]] Status
append_with_per_cid(AuditLog &project_log,
                    std::filesystem::path const &per_cid_dir,
                    Entry entry);

// Read every entry for `cid` from the per-CID sink. Returns an empty
// vector (not an error) when the per-CID file does not yet exist —
// callers iterating fresh-install state need that distinction.
// Empty cid returns an empty vector.
[[nodiscard]] Result<std::vector<Entry>>
read_per_cid(std::filesystem::path const &per_cid_dir, std::string_view cid);

} // namespace st::audit

#endif // ST_AUDIT_HPP
