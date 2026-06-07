// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/profile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace pf = st::profile;

namespace {

std::filesystem::path temp_profile_path(char const *stem) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string{"subuwutuner_profile_test_"} + stem + ".stprofile");
    std::filesystem::remove(p);
    return p;
}

pf::VehicleProfile make_test_profile() {
    pf::VehicleProfile p;
    p.id = "ferrari-the-cat";
    p.display_name = "My 2017 WRX";
    p.vin = "JF1VA1F65G9000000";
    p.year = "2017";
    p.make = "Subaru";
    p.model = "WRX";
    p.transmission = "manual";
    p.transport_hint = "obdx COM5";
    p.created_iso = "2026-06-01T20:00:00Z";
    p.updated_iso = "2026-06-01T20:30:00Z";
    p.notes = "Daily driver; aftermarket cal; LF79103P after aftermarket-uninstall";

    pf::EcuProfile engine;
    engine.role = "engine";
    engine.cal_id = "LF79103P";
    engine.ecu_part = "22765-AS80U";
    engine.pack_id = "subaru-va-wrx-mt";
    engine.notes = "Post-aftermarket-uninstall state";
    p.ecus.push_back(std::move(engine));

    pf::LastFlash lf;
    lf.timestamp_iso = "2026-05-30T18:00:00Z";
    lf.source_rom_path = "/abs/source.bin";
    lf.source_rom_crc32 = 0xC0DECAFE;
    lf.target_rom_path = "/abs/target.bin";
    lf.target_rom_crc32 = 0xDEADBEEF;
    lf.backup_path = "/abs/backups/pre_flash.stbackup";
    lf.operator_notes = "Stage 1 + decat";
    p.last_flash = lf;

    return p;
}

} // namespace

TEST_CASE("profile save/load round-trip preserves all fields",
          "[profile][round-trip]") {
    auto const original = make_test_profile();
    auto const path = temp_profile_path("roundtrip");
    REQUIRE(pf::save(original, path).has_value());

    auto loaded = pf::load(path);
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->schema_version == original.schema_version);
    REQUIRE(loaded->id == original.id);
    REQUIRE(loaded->display_name == original.display_name);
    REQUIRE(loaded->vin == original.vin);
    REQUIRE(loaded->year == original.year);
    REQUIRE(loaded->make == original.make);
    REQUIRE(loaded->model == original.model);
    REQUIRE(loaded->transmission == original.transmission);
    REQUIRE(loaded->transport_hint == original.transport_hint);
    REQUIRE(loaded->created_iso == original.created_iso);
    REQUIRE(loaded->updated_iso == original.updated_iso);
    REQUIRE(loaded->notes == original.notes);

    REQUIRE(loaded->ecus.size() == 1);
    REQUIRE(loaded->ecus[0].role == "engine");
    REQUIRE(loaded->ecus[0].cal_id == "LF79103P");
    REQUIRE(loaded->ecus[0].ecu_part == "22765-AS80U");
    REQUIRE(loaded->ecus[0].pack_id == "subaru-va-wrx-mt");

    REQUIRE(loaded->last_flash.has_value());
    REQUIRE(loaded->last_flash->timestamp_iso == "2026-05-30T18:00:00Z");
    REQUIRE(loaded->last_flash->source_rom_crc32 == 0xC0DECAFE);
    REQUIRE(loaded->last_flash->target_rom_crc32 == 0xDEADBEEF);

    std::filesystem::remove(path);
}

TEST_CASE("profile with minimum fields (just id) round-trips",
          "[profile][round-trip][minimal]") {
    pf::VehicleProfile p;
    p.id = "minimal";
    auto const path = temp_profile_path("minimal");
    REQUIRE(pf::save(p, path).has_value());

    auto loaded = pf::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->id == "minimal");
    REQUIRE(loaded->ecus.empty());
    REQUIRE_FALSE(loaded->last_flash.has_value());
    std::filesystem::remove(path);
}

TEST_CASE("profile::load rejects missing [vehicle_profile] table",
          "[profile][error]") {
    auto const path = temp_profile_path("no_table");
    {
        std::ofstream out{path};
        out << "[other]\nkey = 1\n";
    }
    auto r = pf::load(path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("[vehicle_profile]") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("profile::load rejects missing required id",
          "[profile][error]") {
    auto const path = temp_profile_path("no_id");
    {
        std::ofstream out{path};
        out << "[vehicle_profile]\ndisplay_name = \"no id\"\n";
    }
    auto r = pf::load(path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("required `id`") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("profile::load rejects future schema_version",
          "[profile][error]") {
    auto const path = temp_profile_path("future");
    {
        std::ofstream out{path};
        out << "[vehicle_profile]\nschema_version = 99\nid = \"x\"\n";
    }
    auto r = pf::load(path);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().to_string().find("newer than supported") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("profile::list scans a directory and returns sorted by id",
          "[profile][list]") {
    auto const dir = std::filesystem::temp_directory_path() /
                     "subuwutuner_profile_test_list_dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    pf::VehicleProfile p1;
    p1.id = "zebra";
    p1.display_name = "Z";
    REQUIRE(pf::save(p1, dir / "z.stprofile").has_value());

    pf::VehicleProfile p2;
    p2.id = "alpha";
    p2.display_name = "A";
    REQUIRE(pf::save(p2, dir / "a.stprofile").has_value());

    // Non-profile file alongside — should be ignored.
    std::ofstream{dir / "notes.txt"} << "garbage\n";
    // Malformed .stprofile — should be skipped, not fail the list.
    std::ofstream{dir / "broken.stprofile"} << "not a real toml = = =\n";

    auto out = pf::list(dir);
    REQUIRE(out.has_value());
    REQUIRE(out->size() == 2);
    REQUIRE((*out)[0].id == "alpha");
    REQUIRE((*out)[1].id == "zebra");

    std::filesystem::remove_all(dir);
}

TEST_CASE("profile::list on missing directory returns empty (not error)",
          "[profile][list][edge]") {
    auto const dir = std::filesystem::temp_directory_path() /
                     "subuwutuner_profile_test_nonexistent_xyz";
    std::filesystem::remove_all(dir);
    auto out = pf::list(dir);
    REQUIRE(out.has_value());
    REQUIRE(out->empty());
}

TEST_CASE("default_profile_dir is non-empty + path-like",
          "[profile][default-dir]") {
    auto const d = pf::default_profile_dir();
    REQUIRE_FALSE(d.empty());
    // Smoke: should end in "profiles".
    REQUIRE(d.filename().string() == "profiles");
}
