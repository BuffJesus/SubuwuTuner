// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

// Headless acceptance coverage for the primary hardware-independent journey.
// This intentionally crosses module boundaries: it catches fixture/schema
// drift that isolated parser, project, Atlas, and policy tests cannot see.

#include "st/library/atlas.hpp"
#include "st/library/datalog_csv.hpp"
#include "st/library/datalog_session.hpp"
#include "st/policy/flash_preflight.hpp"
#include "st/project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string_view>

namespace {

bool has_blocker(st::DiagnosticReport const &report, std::string_view category) {
    for (auto const &item : report.items()) {
        if (item.is_blocker() && item.category() == category) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("offline application workflow remains usable without an ECU",
          "[ui][smoke][offline_workflow]") {
    namespace fs = std::filesystem;

    auto project = st::Project::open(fs::path{ST_FIXTURE_DEMO_STUNE});
    REQUIRE(project.has_value());
    CHECK(project->display_name() == "Demo (synthetic)");
    REQUIRE(project->definition().validate().has_value());
    CHECK(project->source_rom().size() == project->working_rom().size());
    CHECK(project->source_rom().crc32() == project->working_rom().crc32());
    auto const identity = project->definition().match_info(project->source_rom());
    REQUIRE(identity.has_value());
    CHECK_FALSE(identity->cid.empty());
    REQUIRE_FALSE(project->definition().tables().empty());
    auto const &first_table = project->definition().tables().front();
    auto table_data = project->definition().read_table_values(project->working_rom(), first_table);
    REQUIRE(table_data.has_value());

    auto log = st::library::datalog_csv::parse(
        "Time (s),RPM,Engine Load\n0,1200,0.5\n1,2800,1.4\n2,4200,2.1\n");
    REQUIRE(log.time_column.has_value());
    CHECK(log.row_count == 3);
    st::library::datalog_session::Session session;
    session.source_path = "fixture-pull.csv";
    session.visible_headers = {"RPM", "Engine Load"};
    session.x_axis = "Time (s)";
    session.range_first = 1;
    session.range_last = 3;
    session.notes = "Offline smoke fixture";
    auto encoded_session = st::library::datalog_session::serialize(session);
    REQUIRE(encoded_session.has_value());
    auto decoded_session = st::library::datalog_session::parse(*encoded_session);
    REQUIRE(decoded_session.has_value());
    CHECK(decoded_session->visible_headers == session.visible_headers);
    CHECK(decoded_session->range_first == 1);

    auto const atlas_path = fs::path{ST_REPO_ROOT} / "fixtures" / "tuner_atlas" /
                            "tuner_atlas.toml";
    auto atlas = st::library::Atlas::load_from_file(atlas_path);
    REQUIRE(atlas.has_value());
    CHECK_FALSE(atlas->tables().empty());
    CHECK_FALSE(atlas->safety_pairs_for_table("Boost Target Main").empty());

    st::policy::PreflightContext offline;
    offline.expected_ecu_id = identity->cid;
    offline.definition_match_verified = true;
    offline.checksum_strategy_known = !project->definition().pack().checksum_type.empty();
    offline.source_rom_size = project->source_rom().size();
    offline.bytes_to_write = 0;
    auto const report = st::policy::default_pipeline().run(offline);
    CHECK_FALSE(report.ok());
    CHECK(has_blocker(report, st::policy::kCatEcuIdentityKnown));
    CHECK(has_blocker(report, st::policy::kCatSourceImage));
    CHECK(has_blocker(report, st::policy::kCatRecoveryImage));
    CHECK(has_blocker(report, st::policy::kCatBackupPresent));
}
