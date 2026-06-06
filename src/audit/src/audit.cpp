// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/audit.hpp"

#include "st/core/crc32.hpp"
#include "st/core/json_util.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace st::audit {

namespace {

// ---- Kind ↔ name table -------------------------------------------------
//
// Wire-format strings are dotted (subsystem.event) so a future grep is
// cheap. Order matches the EntryKind enum for compactness.

struct KindNamePair {
    EntryKind kind;
    std::string_view name;
};

constexpr KindNamePair kKindNames[] = {
    {EntryKind::Custom, "custom"},
    {EntryKind::EcuConnected, "ecu.connected"},
    {EntryKind::EcuDisconnected, "ecu.disconnected"},
    {EntryKind::SecurityAccessUnlocked, "ecu.security_access_unlocked"},
    {EntryKind::BackupCreated, "backup.created"},
    {EntryKind::BackupVerified, "backup.verified"},
    {EntryKind::FlashStarted, "flash.started"},
    {EntryKind::FlashSectorWritten, "flash.sector_written"},
    {EntryKind::FlashCompleted, "flash.completed"},
    {EntryKind::FlashFailed, "flash.failed"},
    {EntryKind::FlashCancelled, "flash.cancelled"},
    {EntryKind::DatalogStarted, "log.started"},
    {EntryKind::DatalogStopped, "log.stopped"},
    {EntryKind::ChecksumRecomputed, "checksum.recomputed"},
    {EntryKind::ProjectOpened, "project.opened"},
    {EntryKind::ProjectSaved, "project.saved"},
    {EntryKind::EditCommitted, "project.edit_committed"},
    {EntryKind::AutotuneCommitted, "project.autotune_committed"},
    {EntryKind::RomReadStarted, "rom.read_started"},
    {EntryKind::RomReadCompleted, "rom.read_completed"},
    {EntryKind::RomReadFailed, "rom.read_failed"},
    {EntryKind::RomReadCancelled, "rom.read_cancelled"},
};

using st::json_escape;

// Canonical serialization minus the checksum field AND minus the
// closing `}` — what CRC32 rolls over. serialize_entry() appends
// the checksum field + closes the brace afterwards. Keeping the
// closing brace OUT of this string means CRC32 doesn't need to
// "skip" anything; the read-side recomputes the same prefix.
std::string serialize_for_checksum(Entry const &e) {
    std::string out;
    out.reserve(256);
    out.append("{\"kind\":");
    json_escape(out, kind_name(e.kind));
    out.append(",\"ts\":");
    out.append(std::to_string(e.timestamp_ns));
    out.append(",\"source\":");
    json_escape(out, e.source);
    out.append(",\"desc\":");
    json_escape(out, e.description);
    // Empty cid/vin are skipped on the wire so legacy logs (written
    // before these fields existed) still round-trip with a matching
    // CRC32 after a re-read. New entries with empty cid produce the
    // same byte sequence the old serializer did.
    if (!e.cid.empty()) {
        out.append(",\"cid\":");
        json_escape(out, e.cid);
    }
    if (!e.vin.empty()) {
        out.append(",\"vin\":");
        json_escape(out, e.vin);
    }
    out.append(",\"fields\":[");
    for (std::size_t i = 0; i < e.fields.size(); ++i) {
        if (i != 0)
            out.append(",");
        out.append("[");
        json_escape(out, e.fields[i].first);
        out.append(",");
        json_escape(out, e.fields[i].second);
        out.append("]");
    }
    out.append("]");
    return out;
}

[[nodiscard]] std::int64_t now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

// Minimal hand-rolled NDJSON line parser specialized to the
// AuditLog shape. We don't pull in a full JSON parser because the
// on-wire format is fixed-key and append-only. A "real" parser
// would handle escapes more robustly; this one handles the subset
// our writer emits.
//
// Returns false on any parse error — caller skips that line.

bool skip_ws(std::string_view &s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    return !s.empty();
}

bool consume(std::string_view &s, char c) {
    if (s.empty() || s.front() != c)
        return false;
    s.remove_prefix(1);
    return true;
}

bool parse_quoted_string(std::string_view &s, std::string &out) {
    if (!consume(s, '"'))
        return false;
    out.clear();
    while (!s.empty() && s.front() != '"') {
        char c = s.front();
        s.remove_prefix(1);
        if (c == '\\' && !s.empty()) {
            char esc = s.front();
            s.remove_prefix(1);
            switch (esc) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                // \uXXXX — accept but pass through only ASCII range to
                // keep the parser tiny; control chars > 0x7F decode to
                // a placeholder. Our writer only emits \u00XX so this
                // is safe in practice.
                if (s.size() < 4)
                    return false;
                std::uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    char hex = s.front();
                    s.remove_prefix(1);
                    code <<= 4U;
                    if (hex >= '0' && hex <= '9')
                        code |= static_cast<std::uint32_t>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f')
                        code |= static_cast<std::uint32_t>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F')
                        code |= static_cast<std::uint32_t>(hex - 'A' + 10);
                    else
                        return false;
                }
                if (code < 0x80)
                    out.push_back(static_cast<char>(code));
                else
                    out.push_back('?');
                break;
            }
            default:
                return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return consume(s, '"');
}

bool parse_int64(std::string_view &s, std::int64_t &out) {
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    if (s.empty() || s.front() < '0' || s.front() > '9')
        return false;
    std::int64_t v = 0;
    while (!s.empty() && s.front() >= '0' && s.front() <= '9') {
        v = v * 10 + static_cast<std::int64_t>(s.front() - '0');
        s.remove_prefix(1);
    }
    out = negative ? -v : v;
    return true;
}

bool parse_uint32(std::string_view &s, std::uint32_t &out) {
    std::int64_t v = 0;
    if (!parse_int64(s, v))
        return false;
    if (v < 0)
        return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

bool parse_entry_line(std::string_view line, Entry &out) {
    if (!consume(line, '{'))
        return false;
    while (true) {
        skip_ws(line);
        if (consume(line, '}'))
            break;
        std::string key;
        if (!parse_quoted_string(line, key))
            return false;
        skip_ws(line);
        if (!consume(line, ':'))
            return false;
        skip_ws(line);

        if (key == "kind") {
            std::string v;
            if (!parse_quoted_string(line, v))
                return false;
            out.kind = kind_from_name(v);
        } else if (key == "ts") {
            std::int64_t v = 0;
            if (!parse_int64(line, v))
                return false;
            out.timestamp_ns = v;
        } else if (key == "source") {
            if (!parse_quoted_string(line, out.source))
                return false;
        } else if (key == "desc") {
            if (!parse_quoted_string(line, out.description))
                return false;
        } else if (key == "cid") {
            if (!parse_quoted_string(line, out.cid))
                return false;
        } else if (key == "vin") {
            if (!parse_quoted_string(line, out.vin))
                return false;
        } else if (key == "fields") {
            if (!consume(line, '['))
                return false;
            skip_ws(line);
            while (!consume(line, ']')) {
                if (!consume(line, '['))
                    return false;
                std::string fk;
                if (!parse_quoted_string(line, fk))
                    return false;
                skip_ws(line);
                if (!consume(line, ','))
                    return false;
                skip_ws(line);
                std::string fv;
                if (!parse_quoted_string(line, fv))
                    return false;
                if (!consume(line, ']'))
                    return false;
                out.fields.emplace_back(std::move(fk), std::move(fv));
                skip_ws(line);
                if (consume(line, ','))
                    skip_ws(line);
            }
        } else if (key == "checksum") {
            if (!parse_uint32(line, out.checksum_on_disk))
                return false;
        } else {
            // Unknown key — skip to next , or }. Forward-compat for
            // future writers that add fields.
            int depth = 0;
            while (!line.empty()) {
                char c = line.front();
                if (c == '"') {
                    std::string ignore;
                    if (!parse_quoted_string(line, ignore))
                        return false;
                    continue;
                }
                if (c == '[' || c == '{')
                    ++depth;
                if (c == ']' || c == '}') {
                    if (depth == 0)
                        break;
                    --depth;
                }
                if (c == ',' && depth == 0)
                    break;
                line.remove_prefix(1);
            }
        }
        skip_ws(line);
        if (consume(line, ','))
            continue;
    }
    return true;
}

} // namespace

std::string_view kind_name(EntryKind k) noexcept {
    for (auto const &p : kKindNames) {
        if (p.kind == k)
            return p.name;
    }
    return "unknown";
}

EntryKind kind_from_name(std::string_view name) noexcept {
    for (auto const &p : kKindNames) {
        if (p.name == name)
            return p.kind;
    }
    return EntryKind::Custom;
}

std::uint32_t entry_checksum(Entry const &e) noexcept {
    auto const canon = serialize_for_checksum(e);
    return st::crc32(std::span<std::uint8_t const>{
        reinterpret_cast<std::uint8_t const *>(canon.data()), canon.size()});
}

std::string serialize_entry(Entry const &e) {
    // serialize_for_checksum returns `{...}`-MINUS-trailing-brace
    // (ends at the fields `]`). Append the checksum field + close.
    auto out = serialize_for_checksum(e);
    out.append(",\"checksum\":");
    out.append(std::to_string(entry_checksum(e)));
    out.push_back('}');
    return out;
}

Result<AuditLog> AuditLog::open(std::filesystem::path const &path) {
    AuditLog log;
    log.path_ = path;
    log.out_.open(path, std::ios::app | std::ios::binary);
    if (!log.out_) {
        std::string msg{"AuditLog::open: cannot open "};
        msg.append(path.string());
        return failure(ErrorCode::IoFailure, std::move(msg));
    }
    return log;
}

Status AuditLog::append(Entry entry) {
    if (entry.timestamp_ns == 0)
        entry.timestamp_ns = now_ns();
    if (entry.cid.empty())
        entry.cid = default_cid_;
    if (entry.vin.empty())
        entry.vin = default_vin_;
    // Serialize once — both the project log and the per-CID sink
    // write the exact same canonical bytes, so the cross-session
    // reader can verify CRCs without re-canonicalizing.
    auto const line = serialize_entry(entry);
    out_ << line << '\n';
    out_.flush();
    if (!out_) {
        return failure(ErrorCode::IoFailure, "AuditLog::append: write failed");
    }
    // Per-CID tee. Project-log write is already durable; a sink
    // failure surfaces but doesn't unwind the local write.
    if (!per_cid_dir_.empty() && !entry.cid.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(per_cid_dir_, ec);
        if (ec) {
            std::string msg{"AuditLog::append: cannot create per-CID dir "};
            msg.append(per_cid_dir_.string());
            return failure(ErrorCode::IoFailure, std::move(msg));
        }
        auto const sink_path = per_cid_log_path(per_cid_dir_, entry.cid);
        std::ofstream sink{sink_path, std::ios::app | std::ios::binary};
        if (!sink) {
            std::string msg{"AuditLog::append: cannot open per-CID sink "};
            msg.append(sink_path.string());
            return failure(ErrorCode::IoFailure, std::move(msg));
        }
        sink << line << '\n';
        sink.flush();
        if (!sink) {
            return failure(ErrorCode::IoFailure,
                           "AuditLog::append: per-CID sink write failed");
        }
    }
    return ok();
}

Status AuditLog::log(EntryKind kind, std::string source, std::string description,
                     std::vector<std::pair<std::string, std::string>> fields) {
    Entry e;
    e.kind = kind;
    e.source = std::move(source);
    e.description = std::move(description);
    e.fields = std::move(fields);
    return append(std::move(e));
}

Result<std::vector<Entry>> AuditLog::read_all(std::filesystem::path const &path) {
    return st::audit::read_all(path);
}

namespace {

// Char-set for per-CID slug. Preserves case (CIDs like "LF79103P" are
// uppercase alphanumeric); chars outside [A-Za-z0-9._-] become '_'.
// Distinct from BackupStore's slugify, which lowercases — we want the
// on-disk filename to visually match the CID the user sees in the
// VehicleProfile panel.
[[nodiscard]] std::string sanitize_cid(std::string_view cid) {
    std::string out;
    out.reserve(cid.size());
    for (char raw_c : cid) {
        auto const c = static_cast<unsigned char>(raw_c);
        bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    return out;
}

} // namespace

std::filesystem::path
per_cid_log_path(std::filesystem::path const &per_cid_dir, std::string_view cid) {
    if (cid.empty())
        return {};
    return per_cid_dir / (sanitize_cid(cid) + ".log");
}

Status append_with_per_cid(AuditLog &project_log,
                           std::filesystem::path const &per_cid_dir,
                           Entry entry) {
    // One-off convenience for callers that don't want a long-lived
    // configured AuditLog handle. Temporarily borrows the per-CID
    // sink, restores the previous one. Steady-state code paths
    // (UI/protocol) prefer set_per_cid_sink() at session open.
    auto const saved = project_log.per_cid_sink();
    project_log.set_per_cid_sink(per_cid_dir);
    auto status = project_log.append(std::move(entry));
    project_log.set_per_cid_sink(saved);
    return status;
}

Result<std::vector<Entry>>
read_per_cid(std::filesystem::path const &per_cid_dir, std::string_view cid) {
    if (cid.empty())
        return std::vector<Entry>{};
    auto const path = per_cid_log_path(per_cid_dir, cid);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::vector<Entry>{};
    return read_all(path);
}

Result<std::vector<Entry>> read_all(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        std::string msg{"audit::read_all: cannot open "};
        msg.append(path.string());
        return failure(ErrorCode::IoFailure, std::move(msg));
    }
    std::vector<Entry> entries;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        Entry e;
        if (!parse_entry_line(line, e)) {
            // Treat unparseable lines as tampered Custom-kind entries
            // so the caller sees them without aborting the whole read.
            e.kind = EntryKind::Custom;
            e.description = std::string{"<<unparseable: "} + line + ">>";
            e.checksum_valid = false;
            entries.push_back(std::move(e));
            continue;
        }
        auto const computed = entry_checksum(e);
        e.checksum_valid = (computed == e.checksum_on_disk);
        entries.push_back(std::move(e));
    }
    return entries;
}

} // namespace st::audit
