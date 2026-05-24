// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for st::defs::PackRegistry — the overlay-load discovery layer
// that lets ship-time per-platform .zip archives coexist with loose
// .toml overrides at runtime.

#include "st/defs/pack_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace defs = st::defs;

namespace {

// Tiny valid pack TOML — just enough to parse cleanly. The platform
// field is the one we use to confirm overrides came through.
std::string sample_pack_toml(std::string_view id, std::string_view platform) {
    return std::string{R"(
[pack]
schema_version = 1
id             = ")"} + std::string{id} + R"("
display_name   = "Sample"
platform       = ")" + std::string{platform} + R"("
endianness     = "big"
rom_size_bytes = 1024
license        = "Apache-2.0"

[[identification]]
name        = "sample"
cid_address = 0
cid_length  = 8
cid_match   = "SAMPLEID"
)";
}

void write_file(std::filesystem::path const &p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::filesystem::path fixture_archive_path() {
    // Tests run with CWD = build dir on some configurations; check both.
    auto const a = std::filesystem::path{"tests/unit/defs/fixtures/test_archive.zip"};
    if (std::filesystem::exists(a)) {
        return a;
    }
    auto const b = std::filesystem::path{
        "../../tests/unit/defs/fixtures/test_archive.zip"};
    if (std::filesystem::exists(b)) {
        return b;
    }
    auto const c = std::filesystem::path{
        "../../../tests/unit/defs/fixtures/test_archive.zip"};
    if (std::filesystem::exists(c)) {
        return c;
    }
    // Last resort: absolute from a known repo root marker.
    return a; // tests will skip if non-existent
}

} // namespace

TEST_CASE("PackRegistry rejects non-directory roots", "[defs][pack_registry]") {
    auto r = defs::PackRegistry::from_directory("nonexistent/path/at/all");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("PackRegistry on empty directory yields zero packs",
          "[defs][pack_registry]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_empty";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto r = defs::PackRegistry::from_directory(tmp);
    REQUIRE(r.has_value());
    REQUIRE(r->list_packs().empty());
    REQUIRE(r->list_platforms().empty());
    REQUIRE(r->warnings().empty());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry discovers top-level loose .toml",
          "[defs][pack_registry]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_toplevel";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    write_file(tmp / "scalar_pack.toml",
               sample_pack_toml("scalar_pack", "test"));

    auto r = defs::PackRegistry::from_directory(tmp);
    REQUIRE(r.has_value());
    auto const packs = r->list_packs();
    REQUIRE(packs.size() == 1);
    REQUIRE(packs[0].id == "scalar_pack");
    REQUIRE(packs[0].platform == "");
    REQUIRE(packs[0].source == defs::PackEntry::Source::LooseTopLevel);

    auto def = r->load_pack("scalar_pack");
    REQUIRE(def.has_value());
    REQUIRE(def->pack().id == "scalar_pack");

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry discovers per-platform loose .toml",
          "[defs][pack_registry]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_platdir";
    std::filesystem::remove_all(tmp);
    write_file(tmp / "impreza" / "a1.toml", sample_pack_toml("a1", "impreza"));
    write_file(tmp / "impreza" / "a2.toml", sample_pack_toml("a2", "impreza"));
    write_file(tmp / "legacy" / "b1.toml", sample_pack_toml("b1", "legacy"));

    auto r = defs::PackRegistry::from_directory(tmp);
    REQUIRE(r.has_value());
    auto const packs = r->list_packs();
    REQUIRE(packs.size() == 3);
    for (auto const &p : packs) {
        REQUIRE(p.source == defs::PackEntry::Source::LoosePlatform);
    }
    auto plats = r->list_platforms();
    std::sort(plats.begin(), plats.end());
    REQUIRE(plats == std::vector<std::string>{"impreza", "legacy"});

    auto imp = r->list_pack_ids_for_platform("impreza");
    std::sort(imp.begin(), imp.end());
    REQUIRE(imp == std::vector<std::string>{"a1", "a2"});

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry skips _archives and dotfile directories",
          "[defs][pack_registry]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_skipdirs";
    std::filesystem::remove_all(tmp);
    write_file(tmp / "_archives" / "x.toml", sample_pack_toml("x", "_archives"));
    write_file(tmp / ".stfolder" / "marker.txt", "");
    write_file(tmp / "impreza" / "real.toml", sample_pack_toml("real", "impreza"));

    auto r = defs::PackRegistry::from_directory(tmp);
    REQUIRE(r.has_value());
    auto plats = r->list_platforms();
    REQUIRE(plats == std::vector<std::string>{"impreza"});

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry reads packs from per-platform .zip archive",
          "[defs][pack_registry][zip]") {
    auto const archive = fixture_archive_path();
    if (!std::filesystem::exists(archive)) {
        SKIP("test fixture archive missing: " + archive.string());
    }
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_zip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    std::filesystem::copy_file(archive, tmp / "test_platform.zip");

    {
        auto r = defs::PackRegistry::from_directory(tmp);
        REQUIRE(r.has_value());

        auto packs = r->list_packs();
        std::sort(packs.begin(), packs.end(),
                  [](auto const &a, auto const &b) { return a.id < b.id; });
        REQUIRE(packs.size() == 2);
        REQUIRE(packs[0].id == "also_archived");
        REQUIRE(packs[0].source == defs::PackEntry::Source::Zipped);
        REQUIRE(packs[0].platform == "test_platform");
        REQUIRE(packs[1].id == "archived_pack");
        REQUIRE(packs[1].source == defs::PackEntry::Source::Zipped);

        auto def = r->load_pack("archived_pack");
        REQUIRE(def.has_value());
        REQUIRE(def->pack().id == "archived_pack");
        REQUIRE(def->pack().platform == "test_platform");
    } // PackRegistry destructs here, releasing the zip file handle

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry: loose top-level overrides zipped on same id",
          "[defs][pack_registry][overlay]") {
    auto const archive = fixture_archive_path();
    if (!std::filesystem::exists(archive)) {
        SKIP("test fixture archive missing: " + archive.string());
    }
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_override";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    std::filesystem::copy_file(archive, tmp / "test_platform.zip");
    // archived_pack also exists at the top level — loose wins.
    write_file(tmp / "archived_pack.toml",
               sample_pack_toml("archived_pack", "OVERRIDE_PLATFORM"));

    {
        auto r = defs::PackRegistry::from_directory(tmp);
        REQUIRE(r.has_value());

        auto const *entry = r->find_pack("archived_pack");
        REQUIRE(entry != nullptr);
        REQUIRE(entry->source == defs::PackEntry::Source::LooseTopLevel);

        auto def = r->load_pack("archived_pack");
        REQUIRE(def.has_value());
        // The loose top-level version's platform field is "OVERRIDE_PLATFORM",
        // not the zipped's "test_platform".
        REQUIRE(def->pack().platform == "OVERRIDE_PLATFORM");

        // A warning should be recorded about the duplicate.
        auto const &ws = r->warnings();
        REQUIRE_FALSE(ws.empty());
        bool found = false;
        for (auto const &w : ws) {
            if (w.find("archived_pack") != std::string::npos &&
                w.find("duplicate") != std::string::npos) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    } // release zip handle

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry: missing pack id returns FileNotFound",
          "[defs][pack_registry][error]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_missing";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto r = defs::PackRegistry::from_directory(tmp);
    REQUIRE(r.has_value());
    auto d = r->load_pack("nope_not_here");
    REQUIRE_FALSE(d.has_value());
    REQUIRE(d.error().code() == st::ErrorCode::FileNotFound);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("PackRegistry: corrupt zip emits warning, does not crash",
          "[defs][pack_registry][zip][error]") {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "subuwu_pack_registry_badzip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    // Write a "zip" that isn't actually a zip.
    write_file(tmp / "corrupt.zip", "this is not a zip file at all");

    auto r = defs::PackRegistry::from_directory(tmp);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->warnings().empty());
    REQUIRE(r->list_packs().empty());

    std::filesystem::remove_all(tmp);
}
