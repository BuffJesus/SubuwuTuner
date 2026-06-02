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
