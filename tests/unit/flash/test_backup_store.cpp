// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <catch2/catch_test_macros.hpp>

#include <st/core/crc32.hpp>
#include <st/flash/backup_store.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using st::flash::Backup;
using st::flash::BackupMeta;
using st::flash::BackupStore;
using st::flash::kBackupSchemaVersion;

namespace {

std::filesystem::path make_temp_dir(std::string const &suffix) {
    auto const tmp = std::filesystem::temp_directory_path() /
                     ("st_backup_store_" + suffix + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp);
    return tmp;
}

std::vector<std::uint8_t> make_bytes(std::size_t n, std::uint8_t seed = 0) {
    std::vector<std::uint8_t> out(n);
    std::mt19937 rng{static_cast<std::uint32_t>(seed) + 0xDEAD'BEEFU};
    for (auto &b : out) {
        b = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    return out;
}

std::chrono::system_clock::time_point fixed_now() {
    // 2026-06-01T12:34:56Z — deliberately picked to test ISO-8601 emission.
    return std::chrono::system_clock::from_time_t(1'748'882'096);
}

} // namespace

TEST_CASE("BackupStore::create round-trips bytes + meta", "[flash][backup_store]") {
    auto const dir = make_temp_dir("roundtrip");
    BackupStore store{dir};

    auto const bytes = make_bytes(4096, 1);
    auto created = store.create(bytes, "A2WC520N", std::string{"JF1VA1H68G9831234"},
                                "before-stage1", "motorsport-only", fixed_now());
    REQUIRE(created.has_value());

    auto const &backup = *created;
    REQUIRE(std::filesystem::exists(backup.header_path));
    REQUIRE(std::filesystem::exists(backup.data_path));
    REQUIRE(backup.meta.schema_version == kBackupSchemaVersion);
    REQUIRE(backup.meta.ecu_id == "A2WC520N");
    REQUIRE(backup.meta.vin.has_value());
    REQUIRE(*backup.meta.vin == "JF1VA1H68G9831234");
    REQUIRE(backup.meta.label == "before-stage1");
    REQUIRE(backup.meta.profile == "motorsport-only");
    REQUIRE(backup.meta.size == bytes.size());
    REQUIRE(backup.meta.crc32 == st::crc32(bytes));
    REQUIRE_FALSE(backup.meta.pinned);

    // verify() should pass on a fresh backup.
    REQUIRE(store.verify(backup).has_value());

    // The data file should match the bytes we wrote. Scope the ifstream
    // so Windows releases the file handle before remove_all() runs.
    {
        std::ifstream in{backup.data_path, std::ios::binary};
        std::vector<std::uint8_t> read_back((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>{});
        REQUIRE(read_back == bytes);
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("BackupStore::create refuses empty bytes and empty ecu_id",
          "[flash][backup_store]") {
    auto const dir = make_temp_dir("refuse");
    BackupStore store{dir};

    auto const empty = std::vector<std::uint8_t>{};
    auto r1 = store.create(empty, "A2WC520N");
    REQUIRE_FALSE(r1.has_value());
    REQUIRE(r1.error().code() == st::ErrorCode::InvalidArgument);

    auto r2 = store.create(make_bytes(128), "");
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().code() == st::ErrorCode::InvalidArgument);

    std::filesystem::remove_all(dir);
}

TEST_CASE("BackupStore::verify detects bit-flip in the data file",
          "[flash][backup_store]") {
    auto const dir = make_temp_dir("flip");
    BackupStore store{dir};

    auto created = store.create(make_bytes(2048, 2), "A2WC520N", std::nullopt, {}, {},
                                fixed_now());
    REQUIRE(created.has_value());

    // Flip a byte in the data file underneath us.
    {
        std::fstream f{created->data_path, std::ios::in | std::ios::out | std::ios::binary};
        REQUIRE(f.is_open());
        f.seekp(7);
        char before = 0;
        f.seekg(7);
        f.read(&before, 1);
        f.seekp(7);
        char after = static_cast<char>(static_cast<std::uint8_t>(before) ^ 0x01U);
        f.write(&after, 1);
    }

    auto v = store.verify(*created);
    REQUIRE_FALSE(v.has_value());
    REQUIRE(v.error().code() == st::ErrorCode::BadChecksum);

    std::filesystem::remove_all(dir);
}

TEST_CASE("BackupStore::set_pinned persists across re-read",
          "[flash][backup_store]") {
    auto const dir = make_temp_dir("pinned");
    BackupStore store{dir};

    auto created = store.create(make_bytes(512, 3), "A2WC520N", std::nullopt, {}, {},
                                fixed_now());
    REQUIRE(created.has_value());
    REQUIRE_FALSE(created->meta.pinned);

    REQUIRE(store.set_pinned(*created, true).has_value());

    auto on_disk = BackupStore::read_meta(created->header_path);
    REQUIRE(on_disk.has_value());
    REQUIRE(on_disk->pinned);

    std::filesystem::remove_all(dir);
}

TEST_CASE("BackupStore::list returns entries sorted oldest-first",
          "[flash][backup_store]") {
    auto const dir = make_temp_dir("list");
    BackupStore store{dir};

    auto const tp_a =
        std::chrono::system_clock::from_time_t(1'700'000'000); // 2023-11-14
    auto const tp_b =
        std::chrono::system_clock::from_time_t(1'730'000'000); // 2024-10-26
    auto const tp_c =
        std::chrono::system_clock::from_time_t(1'750'000'000); // 2025-06-15

    REQUIRE(store.create(make_bytes(64, 1), "X", std::nullopt, "c", {}, tp_c).has_value());
    REQUIRE(store.create(make_bytes(64, 2), "X", std::nullopt, "a", {}, tp_a).has_value());
    REQUIRE(store.create(make_bytes(64, 3), "X", std::nullopt, "b", {}, tp_b).has_value());

    auto const entries = store.list();
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].meta.label == "a");
    REQUIRE(entries[1].meta.label == "b");
    REQUIRE(entries[2].meta.label == "c");

    std::filesystem::remove_all(dir);
}

TEST_CASE("BackupStore::evict_old keeps newest + all pinned",
          "[flash][backup_store]") {
    auto const dir = make_temp_dir("evict");
    BackupStore store{dir};

    // Create five backups with monotonic timestamps.
    for (int i = 0; i < 5; ++i) {
        auto const tp = std::chrono::system_clock::from_time_t(1'700'000'000 + i * 1'000'000);
        auto created =
            store.create(make_bytes(128, static_cast<std::uint8_t>(i)), "X", std::nullopt,
                         "n" + std::to_string(i), {}, tp);
        REQUIRE(created.has_value());
        // Pin the OLDEST so we can prove pinned survives.
        if (i == 0) {
            REQUIRE(store.set_pinned(*created, true).has_value());
        }
    }

    // Allow at most 2 unpinned to remain. We should delete 2 (the oldest
    // two unpinned: n1, n2). n0 (pinned) and n3, n4 (newest two) survive.
    auto const removed = store.evict_old(2);
    REQUIRE(removed == 2);

    auto const after = store.list();
    REQUIRE(after.size() == 3);
    // Pinned must still be present.
    bool found_pinned = false;
    for (auto const &b : after) {
        if (b.meta.label == "n0") {
            REQUIRE(b.meta.pinned);
            found_pinned = true;
        }
    }
    REQUIRE(found_pinned);

    std::filesystem::remove_all(dir);
}

TEST_CASE("BackupStore::format_meta round-trips through parse_meta",
          "[flash][backup_store]") {
    BackupMeta in;
    in.schema_version = kBackupSchemaVersion;
    in.created_at = "2026-06-01T12-34-56Z"; // post-slugify form
    in.ecu_id = "A2WC520N";
    in.vin = "JF1VA1H68G9831234";
    in.label = "before-stage1";
    in.profile = "alberta-ca";
    in.size = 524288;
    in.crc32 = 0xDEAD'BEEFU;
    in.pinned = true;

    auto const text = BackupStore::format_meta(in);
    auto out = BackupStore::parse_meta(text);
    REQUIRE(out.has_value());
    REQUIRE(out->schema_version == in.schema_version);
    REQUIRE(out->created_at == in.created_at);
    REQUIRE(out->ecu_id == in.ecu_id);
    REQUIRE(out->vin == in.vin);
    REQUIRE(out->label == in.label);
    REQUIRE(out->profile == in.profile);
    REQUIRE(out->size == in.size);
    REQUIRE(out->crc32 == in.crc32);
    REQUIRE(out->pinned == in.pinned);
}

TEST_CASE("BackupStore::filename_stem replaces ':' with '-' for Windows portability",
          "[flash][backup_store]") {
    auto const stem = BackupStore::filename_stem("2026-06-01T12:34:56Z", "A2WC520N", "");
    REQUIRE(stem == "2026-06-01T12-34-56Z-A2WC520N");

    auto const with_label =
        BackupStore::filename_stem("2026-06-01T12:34:56Z", "A2WC520N", "Before Stage 1!");
    // Slugify lowercases ASCII and replaces non-alnum with '_'.
    REQUIRE(with_label == "2026-06-01T12-34-56Z-A2WC520N-before_stage_1");
}

TEST_CASE("BackupStore::filename_stem slugify covers ASCII range boundaries",
          "[flash][backup_store][boundary]") {
    // The slug-character classifier in backup_store.cpp checks the
    // range endpoints ('A', 'Z', 'a', 'z', '0', '9') with `<=`. The
    // 2026-06-04 mutation lane found that swapping `<=` to `<` on any
    // of the upper bounds — 'Z', 'z', '9' — leaves the test suite
    // green because the existing tests don't exercise those exact
    // characters in label input. Pin them explicitly.
    auto const slug_caps_Z =
        BackupStore::filename_stem("2026-06-01T12:00:00Z", "A2WC520N", "AZ");
    REQUIRE(slug_caps_Z == "2026-06-01T12-00-00Z-A2WC520N-az");

    auto const slug_lower_z =
        BackupStore::filename_stem("2026-06-01T12:00:00Z", "A2WC520N", "az");
    REQUIRE(slug_lower_z == "2026-06-01T12-00-00Z-A2WC520N-az");

    auto const slug_digit_9 =
        BackupStore::filename_stem("2026-06-01T12:00:00Z", "A2WC520N", "09");
    REQUIRE(slug_digit_9 == "2026-06-01T12-00-00Z-A2WC520N-09");

    // Sanity: the upper-bound exclusion still kicks in for the
    // first char past each range. '{' is one past 'z'; mutating
    // `<=` to `<` would have kept 'z' working but added '{'.
    auto const slug_brace =
        BackupStore::filename_stem("2026-06-01T12:00:00Z", "A2WC520N", "z{");
    REQUIRE(slug_brace == "2026-06-01T12-00-00Z-A2WC520N-z");
}
