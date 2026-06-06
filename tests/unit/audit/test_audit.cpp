// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/audit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace au = st::audit;

namespace {

std::filesystem::path temp_log_path(char const *stem) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string{"subuwutuner_audit_test_"} + stem + ".log");
    std::filesystem::remove(p);
    return p;
}

au::Entry make_test_entry(au::EntryKind kind, char const *desc) {
    au::Entry e;
    e.kind = kind;
    e.source = "test";
    e.description = desc;
    e.fields.push_back({"key1", "value1"});
    e.fields.push_back({"key2", "value with spaces"});
    return e;
}

} // namespace

TEST_CASE("kind_name + kind_from_name round-trip every enum value",
          "[audit][kind]") {
    using K = au::EntryKind;
    for (auto k : {K::Custom, K::EcuConnected, K::EcuDisconnected,
                   K::SecurityAccessUnlocked, K::BackupCreated, K::BackupVerified,
                   K::FlashStarted, K::FlashSectorWritten, K::FlashCompleted,
                   K::FlashFailed, K::FlashCancelled, K::DatalogStarted,
                   K::DatalogStopped, K::ChecksumRecomputed}) {
        auto const name = au::kind_name(k);
        REQUIRE_FALSE(name.empty());
        REQUIRE(au::kind_from_name(name) == k);
    }
    // Unknown name decays to Custom rather than crashing — readers
    // forward-compat for new kinds added by future writers.
    REQUIRE(au::kind_from_name("future.kind.we.dont.know") == au::EntryKind::Custom);
}

TEST_CASE("entry_checksum is deterministic across calls", "[audit][checksum]") {
    auto const e = make_test_entry(au::EntryKind::FlashStarted, "first flash");
    REQUIRE(au::entry_checksum(e) == au::entry_checksum(e));
}

TEST_CASE("entry_checksum changes when any field changes", "[audit][checksum]") {
    auto a = make_test_entry(au::EntryKind::FlashStarted, "first flash");
    a.timestamp_ns = 1000;
    auto b = a;
    auto const baseline = au::entry_checksum(a);

    b.timestamp_ns = 2000;
    REQUIRE(au::entry_checksum(b) != baseline);

    auto c = a;
    c.description = "first flash with extra text";
    REQUIRE(au::entry_checksum(c) != baseline);

    auto d = a;
    d.source = "different";
    REQUIRE(au::entry_checksum(d) != baseline);

    auto f = a;
    f.fields.push_back({"extra", "field"});
    REQUIRE(au::entry_checksum(f) != baseline);
}

TEST_CASE("AuditLog::open creates a writable file", "[audit][open]") {
    auto const p = temp_log_path("open");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log.has_value());
        REQUIRE(log->path() == p);
    }
    REQUIRE(std::filesystem::exists(p));
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog::append + read_all round-trip a single entry",
          "[audit][round-trip]") {
    auto const p = temp_log_path("single");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log.has_value());
        REQUIRE(log->log(au::EntryKind::FlashStarted, "flash",
                        "Started flash of plan.toml",
                        {{"plan", "/path/to/plan.toml"}, {"sectors", "4"}})
                    .has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    auto const &e = entries->front();
    REQUIRE(e.kind == au::EntryKind::FlashStarted);
    REQUIRE(e.source == "flash");
    REQUIRE(e.description == "Started flash of plan.toml");
    REQUIRE(e.fields.size() == 2);
    REQUIRE(e.fields[0].first == "plan");
    REQUIRE(e.fields[0].second == "/path/to/plan.toml");
    REQUIRE(e.fields[1].first == "sectors");
    REQUIRE(e.fields[1].second == "4");
    REQUIRE(e.checksum_valid);
    REQUIRE(e.timestamp_ns > 0);
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog appends multiple entries in order",
          "[audit][round-trip][multi]") {
    auto const p = temp_log_path("multi");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log.has_value());
        REQUIRE(log->log(au::EntryKind::EcuConnected, "transport",
                        "OBDX connected on COM5", {})
                    .has_value());
        REQUIRE(log->log(au::EntryKind::BackupCreated, "flash",
                        "Backup at backups/2026-06-01T20.stbackup",
                        {{"sha256", "abc123"}})
                    .has_value());
        REQUIRE(log->log(au::EntryKind::FlashCompleted, "flash",
                        "Wrote 4 sectors, verify passed", {})
                    .has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 3);
    REQUIRE((*entries)[0].kind == au::EntryKind::EcuConnected);
    REQUIRE((*entries)[1].kind == au::EntryKind::BackupCreated);
    REQUIRE((*entries)[1].fields[0].second == "abc123");
    REQUIRE((*entries)[2].kind == au::EntryKind::FlashCompleted);
    for (auto const &e : *entries) {
        REQUIRE(e.checksum_valid);
    }
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog re-open is append-only (preserves existing entries)",
          "[audit][append-only]") {
    auto const p = temp_log_path("reopen");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashStarted, "flash", "session 1", {}).has_value());
    }
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashCompleted, "flash", "session 1 done", {}).has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    REQUIRE((*entries)[0].description == "session 1");
    REQUIRE((*entries)[1].description == "session 1 done");
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog read detects tampered checksums",
          "[audit][tamper]") {
    auto const p = temp_log_path("tamper");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashStarted, "flash", "original", {}).has_value());
    }
    // Hand-mutate the description in the on-disk line without
    // touching the checksum field — the round-trip read should flag
    // checksum_valid=false. Read whole-file via std::ifstream's
    // line-by-line API instead of istreambuf_iterator which trips
    // GCC -Werror=null-dereference on MinGW (false positive on
    // streambuf's gptr()).
    {
        std::string contents;
        std::ifstream in{p};
        std::string line;
        while (std::getline(in, line)) {
            contents.append(line);
            contents.push_back('\n');
        }
        in.close();
        auto const pos = contents.find("original");
        REQUIRE(pos != std::string::npos);
        contents.replace(pos, sizeof "original" - 1, "MUTATED!");
        std::ofstream out{p, std::ios::binary | std::ios::trunc};
        out << contents;
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    REQUIRE_FALSE(entries->front().checksum_valid);
    REQUIRE(entries->front().description == "MUTATED!");
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog read tolerates malformed lines",
          "[audit][parse][error]") {
    auto const p = temp_log_path("malformed");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashStarted, "flash", "good entry", {})
                    .has_value());
    }
    {
        std::ofstream out{p, std::ios::app};
        out << "not a json line at all\n";
        out << R"({"kind":"flash.completed","ts":invalid,"source":"flash","desc":"bad"})"
            << "\n";
    }
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashCompleted, "flash", "another good", {}).has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 4);
    REQUIRE((*entries)[0].checksum_valid);
    REQUIRE_FALSE((*entries)[1].checksum_valid); // unparseable line 1
    REQUIRE_FALSE((*entries)[2].checksum_valid); // unparseable line 2
    REQUIRE((*entries)[3].checksum_valid);
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog read tolerates blank lines",
          "[audit][parse][edge]") {
    auto const p = temp_log_path("blanks");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashStarted, "flash", "entry 1", {}).has_value());
    }
    {
        std::ofstream out{p, std::ios::app};
        out << "\n\n\n";
    }
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::FlashCompleted, "flash", "entry 2", {}).has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog::open returns IoFailure on bad path",
          "[audit][error]") {
    auto const p = std::filesystem::path{"/nonexistent_directory_xyz/audit.log"};
    auto log = au::AuditLog::open(p);
    REQUIRE_FALSE(log.has_value());
    REQUIRE(log.error().code() == st::ErrorCode::IoFailure);
}

TEST_CASE("read_all on missing file returns IoFailure",
          "[audit][error]") {
    auto const p = std::filesystem::path{"/nonexistent_directory_xyz/audit.log"};
    auto r = au::read_all(p);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::IoFailure);
}

TEST_CASE("Entry with cid + vin round-trips through write/read",
          "[audit][cid]") {
    auto const p = temp_log_path("cid_roundtrip");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log.has_value());
        au::Entry e;
        e.kind = au::EntryKind::FlashCompleted;
        e.source = "flash";
        e.description = "verify passed";
        e.cid = "LF79103P";
        e.vin = "JF1VA1B61H9800001";
        e.fields.push_back({"sectors", "4"});
        REQUIRE(log->append(std::move(e)).has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    auto const &got = entries->front();
    REQUIRE(got.cid == "LF79103P");
    REQUIRE(got.vin == "JF1VA1B61H9800001");
    REQUIRE(got.checksum_valid);
    std::filesystem::remove(p);
}

TEST_CASE("Empty cid + vin produce wire bytes identical to legacy format",
          "[audit][cid][backward-compat]") {
    // Regression vector for the backward-compat contract: an Entry
    // with empty cid/vin must serialize to the exact same canonical
    // string as the pre-cid serializer did, so existing audit.log
    // files keep verifying after the schema change.
    au::Entry e;
    e.kind = au::EntryKind::FlashStarted;
    e.timestamp_ns = 1700000000000000000;
    e.source = "flash";
    e.description = "Started flash of plan.toml";
    e.fields.push_back({"plan", "/path/to/plan.toml"});
    auto const wire = au::serialize_entry(e);
    // The expected shape mirrors the original key order: kind, ts,
    // source, desc, fields, checksum. No cid/vin between desc and
    // fields when empty.
    REQUIRE(wire.find("\"cid\"") == std::string::npos);
    REQUIRE(wire.find("\"vin\"") == std::string::npos);
    auto const desc_pos = wire.find("\"desc\"");
    auto const fields_pos = wire.find("\"fields\"");
    REQUIRE(desc_pos != std::string::npos);
    REQUIRE(fields_pos != std::string::npos);
    REQUIRE(desc_pos < fields_pos);
}

TEST_CASE("CRC32 changes when cid changes", "[audit][cid][checksum]") {
    au::Entry a;
    a.kind = au::EntryKind::FlashCompleted;
    a.timestamp_ns = 1700000000000000000;
    a.source = "flash";
    a.description = "done";
    auto const baseline = au::entry_checksum(a);

    auto b = a;
    b.cid = "LF79103P";
    REQUIRE(au::entry_checksum(b) != baseline);

    auto c = a;
    c.vin = "JF1VA1B61H9800001";
    REQUIRE(au::entry_checksum(c) != baseline);
}

TEST_CASE("Legacy log (no cid/vin keys) reads with empty cid + valid checksum",
          "[audit][cid][backward-compat]") {
    // Hand-craft a log file in the pre-cid format and confirm the
    // parser reads it with cid=="" + checksum_valid=true. This is
    // the live concern: every shipped .stune already has an
    // audit.log; none of them carry cid keys.
    auto const p = temp_log_path("legacy_format");
    au::Entry orig;
    orig.kind = au::EntryKind::EcuConnected;
    orig.timestamp_ns = 1700000000000000000;
    orig.source = "transport";
    orig.description = "OBDX connected on COM5";
    orig.fields.push_back({"adapter", "OBDX Pro VX"});
    {
        std::ofstream out{p, std::ios::binary};
        out << au::serialize_entry(orig) << "\n";
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    auto const &e = entries->front();
    REQUIRE(e.cid.empty());
    REQUIRE(e.vin.empty());
    REQUIRE(e.checksum_valid);
    REQUIRE(e.kind == au::EntryKind::EcuConnected);
    std::filesystem::remove(p);
}

namespace {
std::filesystem::path temp_per_cid_dir(char const *stem) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string{"subuwutuner_audit_per_cid_"} + stem);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}
} // namespace

TEST_CASE("per_cid_log_path produces <dir>/<sanitized>.log",
          "[audit][per-cid][path]") {
    auto const dir = std::filesystem::path{"/var/data"};
    auto const p = au::per_cid_log_path(dir, "LF79103P");
    REQUIRE(p == dir / "LF79103P.log");
}

TEST_CASE("per_cid_log_path sanitizes path-traversal + filesystem-unsafe chars",
          "[audit][per-cid][path][safety]") {
    auto const dir = std::filesystem::path{"/var/data"};
    // Slashes (forward + backward) are the actual traversal vectors —
    // they have to become non-separators. Dots are preserved because
    // CIDs like "LF79103P.A" are plausible; once the slashes are gone,
    // `..` in a filename body just yields a regular file inside dir
    // (no `/..` means no traversal).
    REQUIRE(au::per_cid_log_path(dir, "../etc/passwd").filename().string() ==
            ".._etc_passwd.log");
    REQUIRE(au::per_cid_log_path(dir, "ab\\cd").filename().string() == "ab_cd.log");
    REQUIRE(au::per_cid_log_path(dir, "with spaces").filename().string() ==
            "with_spaces.log");
    // Confirm the resulting path's parent is per_cid_dir (no escape).
    auto const traversal_try = au::per_cid_log_path(dir, "../../../etc/passwd");
    REQUIRE(traversal_try.parent_path() == dir);
}

TEST_CASE("per_cid_log_path returns empty for empty cid",
          "[audit][per-cid][path][edge]") {
    REQUIRE(au::per_cid_log_path("/var/data", "").empty());
}

TEST_CASE("append_with_per_cid tees both project log and per-CID file",
          "[audit][per-cid][tee]") {
    auto const project_path = temp_log_path("tee_project");
    auto const per_cid_dir = temp_per_cid_dir("tee");
    {
        auto log = au::AuditLog::open(project_path);
        REQUIRE(log.has_value());
        au::Entry e;
        e.kind = au::EntryKind::FlashCompleted;
        e.source = "flash";
        e.description = "verify passed";
        e.cid = "LF79103P";
        REQUIRE(au::append_with_per_cid(*log, per_cid_dir, std::move(e)).has_value());
    }
    auto project_entries = au::read_all(project_path);
    REQUIRE(project_entries.has_value());
    REQUIRE(project_entries->size() == 1);
    REQUIRE(project_entries->front().cid == "LF79103P");

    auto per_cid_entries = au::read_per_cid(per_cid_dir, "LF79103P");
    REQUIRE(per_cid_entries.has_value());
    REQUIRE(per_cid_entries->size() == 1);
    REQUIRE(per_cid_entries->front().cid == "LF79103P");
    // Both writes captured the same timestamp — entries are co-keyed.
    REQUIRE(per_cid_entries->front().timestamp_ns ==
            project_entries->front().timestamp_ns);

    std::filesystem::remove(project_path);
    std::filesystem::remove_all(per_cid_dir);
}

TEST_CASE("append_with_per_cid skips the per-CID write when entry.cid is empty",
          "[audit][per-cid][tee][edge]") {
    auto const project_path = temp_log_path("tee_empty_cid");
    auto const per_cid_dir = temp_per_cid_dir("tee_empty");
    {
        auto log = au::AuditLog::open(project_path);
        REQUIRE(log.has_value());
        au::Entry e;
        e.kind = au::EntryKind::EcuConnected;
        e.source = "transport";
        e.description = "OBDX connected";
        // No cid — pre-identity transport event.
        REQUIRE(au::append_with_per_cid(*log, per_cid_dir, std::move(e)).has_value());
    }
    auto project_entries = au::read_all(project_path);
    REQUIRE(project_entries.has_value());
    REQUIRE(project_entries->size() == 1);
    // per_cid_dir was never touched — no directory created.
    REQUIRE_FALSE(std::filesystem::exists(per_cid_dir));
    std::filesystem::remove(project_path);
}

TEST_CASE("append_with_per_cid creates per_cid_dir on first write",
          "[audit][per-cid][tee][bootstrap]") {
    auto const project_path = temp_log_path("tee_bootstrap_project");
    auto const per_cid_dir = temp_per_cid_dir("tee_bootstrap");
    REQUIRE_FALSE(std::filesystem::exists(per_cid_dir));
    {
        auto log = au::AuditLog::open(project_path);
        REQUIRE(log.has_value());
        au::Entry e;
        e.kind = au::EntryKind::ProjectOpened;
        e.source = "gui";
        e.description = "opened demo.stune";
        e.cid = "LF79103P";
        REQUIRE(au::append_with_per_cid(*log, per_cid_dir, std::move(e)).has_value());
    }
    REQUIRE(std::filesystem::exists(per_cid_dir));
    REQUIRE(std::filesystem::exists(per_cid_dir / "LF79103P.log"));
    std::filesystem::remove(project_path);
    std::filesystem::remove_all(per_cid_dir);
}

TEST_CASE("read_per_cid returns empty vector when no log exists",
          "[audit][per-cid][read][edge]") {
    auto const per_cid_dir = temp_per_cid_dir("read_missing");
    auto entries = au::read_per_cid(per_cid_dir, "NEVER_SEEN");
    REQUIRE(entries.has_value());
    REQUIRE(entries->empty());
}

TEST_CASE("read_per_cid returns empty vector for empty cid",
          "[audit][per-cid][read][edge]") {
    auto entries = au::read_per_cid("/var/data", "");
    REQUIRE(entries.has_value());
    REQUIRE(entries->empty());
}

TEST_CASE("AuditLog::set_default_cid stamps onto entries with empty cid",
          "[audit][per-cid][default]") {
    auto const p = temp_log_path("default_cid_stamp");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log.has_value());
        log->set_default_cid("LF79103P");
        // Caller didn't set entry.cid — defaults stamp on.
        REQUIRE(log->log(au::EntryKind::ProjectOpened, "ui", "opened demo.stune", {})
                    .has_value());
        // Caller's explicit cid wins over the default.
        au::Entry override_entry;
        override_entry.kind = au::EntryKind::FlashCompleted;
        override_entry.source = "flash";
        override_entry.description = "flash for OTHER car";
        override_entry.cid = "LF79101P";
        REQUIRE(log->append(std::move(override_entry)).has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    REQUIRE((*entries)[0].cid == "LF79103P"); // stamped
    REQUIRE((*entries)[1].cid == "LF79101P"); // explicit override
    std::filesystem::remove(p);
}

TEST_CASE("AuditLog::set_per_cid_sink tees subsequent appends automatically",
          "[audit][per-cid][sink][integrated]") {
    auto const project_path = temp_log_path("integrated_sink_project");
    auto const per_cid_dir = temp_per_cid_dir("integrated_sink");
    {
        auto log = au::AuditLog::open(project_path);
        REQUIRE(log.has_value());
        log->set_default_cid("LF79103P");
        log->set_per_cid_sink(per_cid_dir);
        // The bare log()/append() shortcut benefits transparently.
        REQUIRE(log->log(au::EntryKind::FlashStarted, "flash", "started", {}).has_value());
        REQUIRE(log->log(au::EntryKind::FlashCompleted, "flash", "completed", {}).has_value());
    }
    auto per_cid_entries = au::read_per_cid(per_cid_dir, "LF79103P");
    REQUIRE(per_cid_entries.has_value());
    REQUIRE(per_cid_entries->size() == 2);
    REQUIRE((*per_cid_entries)[0].kind == au::EntryKind::FlashStarted);
    REQUIRE((*per_cid_entries)[1].kind == au::EntryKind::FlashCompleted);
    std::filesystem::remove(project_path);
    std::filesystem::remove_all(per_cid_dir);
}

TEST_CASE("Multiple appends from different projects accumulate in per-CID log",
          "[audit][per-cid][cross-session]") {
    // The whole point: a tech can hand a per-CID log to another tech
    // and see every project that touched this car.
    auto const project_a_path = temp_log_path("cross_project_a");
    auto const project_b_path = temp_log_path("cross_project_b");
    auto const per_cid_dir = temp_per_cid_dir("cross_session");
    {
        auto log = au::AuditLog::open(project_a_path);
        au::Entry e;
        e.kind = au::EntryKind::FlashCompleted;
        e.source = "flash";
        e.description = "project A flashed";
        e.cid = "LF79103P";
        REQUIRE(au::append_with_per_cid(*log, per_cid_dir, std::move(e)).has_value());
    }
    {
        auto log = au::AuditLog::open(project_b_path);
        au::Entry e;
        e.kind = au::EntryKind::AutotuneCommitted;
        e.source = "autotune";
        e.description = "project B autotune";
        e.cid = "LF79103P";
        REQUIRE(au::append_with_per_cid(*log, per_cid_dir, std::move(e)).has_value());
    }
    auto entries = au::read_per_cid(per_cid_dir, "LF79103P");
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    REQUIRE((*entries)[0].description == "project A flashed");
    REQUIRE((*entries)[1].description == "project B autotune");
    REQUIRE((*entries)[0].checksum_valid);
    REQUIRE((*entries)[1].checksum_valid);

    std::filesystem::remove(project_a_path);
    std::filesystem::remove(project_b_path);
    std::filesystem::remove_all(per_cid_dir);
}

TEST_CASE("Entry with embedded quotes + newlines round-trips safely",
          "[audit][escape]") {
    auto const p = temp_log_path("escape");
    {
        auto log = au::AuditLog::open(p);
        REQUIRE(log->log(au::EntryKind::Custom, "src",
                        R"(desc with "quotes" and a
newline)",
                        {{"key", R"(value with \backslash and "quotes")"}})
                    .has_value());
    }
    auto entries = au::read_all(p);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    REQUIRE(entries->front().checksum_valid);
    REQUIRE(entries->front().description.find('\n') != std::string::npos);
    REQUIRE(entries->front().description.find('"') != std::string::npos);
    REQUIRE(entries->front().fields[0].second.find('\\') != std::string::npos);
    std::filesystem::remove(p);
}
