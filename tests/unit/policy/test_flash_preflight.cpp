// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include <catch2/catch_test_macros.hpp>

#include <st/core/diagnostic.hpp>
#include <st/policy/flash_preflight.hpp>

#include <string>

using st::Diagnostic;
using st::DiagnosticReport;
using st::Severity;
using st::policy::default_pipeline;
using st::policy::make_backup_present;
using st::policy::make_battery_voltage_ok;
using st::policy::make_checksum_known;
using st::policy::make_ecu_communication_safe;
using st::policy::make_ecu_id_match;
using st::policy::make_ecu_identity_known;
using st::policy::make_definition_match_verified;
using st::policy::make_ignition_on;
using st::policy::make_vin_match;
using st::policy::make_write_extent_sane;
using st::policy::make_journal_safe;
using st::policy::make_recovery_image_present;
using st::policy::make_source_image_verified;
using st::policy::Pipeline;
using st::policy::PreflightContext;
using st::policy::EcuCommunicationState;

namespace {

PreflightContext happy_path_ctx() {
    PreflightContext ctx;
    ctx.expected_ecu_id = "A2WC520N";
    ctx.observed_ecu_id = "A2WC520N";
    ctx.expected_vin = "JF1VA1H68G9831234";
    ctx.observed_vin = "JF1VA1H68G9831234";
    ctx.battery_voltage_v = 12.6;
    ctx.ignition_on = true;
    ctx.checksum_strategy_known = true;
    ctx.backup_present = true;
    ctx.definition_match_verified = true;
    ctx.source_image_verified = true;
    ctx.recovery_image_present = true;
    ctx.journal_safe = true;
    ctx.approved_write_regions = {{0x1000, 0x1000}};
    ctx.planned_write_ranges = {{0x1100, 0x100}};
    ctx.source_rom_size = std::uint64_t{2 * 1024 * 1024};
    ctx.bytes_to_write = std::uint64_t{64 * 1024};
    return ctx;
}

bool has_blocker_in(DiagnosticReport const &r, std::string_view category) {
    for (auto const &d : r.items()) {
        if (d.severity() == Severity::Blocker && d.category() == category) {
            return true;
        }
    }
    return false;
}

bool has_warning_in(DiagnosticReport const &r, std::string_view category) {
    for (auto const &d : r.items()) {
        if (d.severity() == Severity::Warning && d.category() == category) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("default_pipeline yields no blockers on the happy path",
          "[policy][preflight]") {
    auto const r = default_pipeline().run(happy_path_ctx());
    REQUIRE(r.ok());
    REQUIRE(r.blockers().empty());
    REQUIRE(r.warnings().empty());
}

TEST_CASE("EcuIdMatch blocks on mismatch", "[policy][preflight][ecu_id]") {
    auto ctx = happy_path_ctx();
    ctx.observed_ecu_id = "A2YC520N"; // off by one
    auto const r = Pipeline{}.add(make_ecu_id_match()).run(ctx);
    REQUIRE_FALSE(r.ok());
    REQUIRE(has_blocker_in(r, "ecu_id_match"));
}

TEST_CASE("EcuIdMatch is a no-op when either side is unset",
          "[policy][preflight][ecu_id]") {
    auto ctx = happy_path_ctx();
    ctx.expected_ecu_id = std::nullopt;
    REQUIRE(Pipeline{}.add(make_ecu_id_match()).run(ctx).items().empty());

    ctx = happy_path_ctx();
    ctx.observed_ecu_id = std::nullopt;
    REQUIRE(Pipeline{}.add(make_ecu_id_match()).run(ctx).items().empty());
}

TEST_CASE("VinMatch warns when source has VIN but ECU did not report one",
          "[policy][preflight][vin]") {
    auto ctx = happy_path_ctx();
    ctx.observed_vin = std::nullopt;
    auto const r = Pipeline{}.add(make_vin_match()).run(ctx);
    REQUIRE(r.ok()); // Warning, not Blocker.
    REQUIRE(has_warning_in(r, "vin_match"));
}

TEST_CASE("VinMatch blocks on mismatch", "[policy][preflight][vin]") {
    auto ctx = happy_path_ctx();
    ctx.observed_vin = "JF1VA1H68G9999999";
    auto const r = Pipeline{}.add(make_vin_match()).run(ctx);
    REQUIRE_FALSE(r.ok());
    REQUIRE(has_blocker_in(r, "vin_match"));
}

TEST_CASE("EcuCommunicationSafe allows reachable and unknown states",
          "[policy][preflight][ecu_communication]") {
    auto ctx = happy_path_ctx();
    ctx.ecu_communication_state = EcuCommunicationState::Reachable;
    REQUIRE(Pipeline{}.add(make_ecu_communication_safe()).run(ctx).items().empty());

    ctx.ecu_communication_state = EcuCommunicationState::Unknown;
    REQUIRE(Pipeline{}.add(make_ecu_communication_safe()).run(ctx).items().empty());

    ctx.ecu_communication_state = std::nullopt;
    REQUIRE(Pipeline{}.add(make_ecu_communication_safe()).run(ctx).items().empty());
}

TEST_CASE("EcuCommunicationSafe blocks states that require external recovery",
          "[policy][preflight][ecu_communication]") {
    auto ctx = happy_path_ctx();
    ctx.ecu_communication_state = EcuCommunicationState::IdentifiedButWriteRejected;
    {
        auto const r = Pipeline{}.add(make_ecu_communication_safe()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "ecu_communication"));
    }

    ctx.ecu_communication_state = EcuCommunicationState::SilentBootFailure;
    {
        auto const r = Pipeline{}.add(make_ecu_communication_safe()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "ecu_communication"));
    }
}

TEST_CASE("EcuIdentityKnown blocks missing identity and allows a confirmed match",
          "[policy][preflight][ecu_identity]") {
    auto ctx = happy_path_ctx();
    ctx.observed_ecu_id = std::nullopt;
    {
        auto const r = Pipeline{}.add(make_ecu_identity_known()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "ecu_identity_known"));
    }

    ctx = happy_path_ctx();
    REQUIRE(Pipeline{}.add(make_ecu_identity_known()).run(ctx).items().empty());
}

TEST_CASE("Required write proofs block when absent and pass when verified",
          "[policy][preflight][write_proofs]") {
    auto ctx = happy_path_ctx();
    ctx.definition_match_verified = false;
    REQUIRE_FALSE(Pipeline{}.add(make_definition_match_verified()).run(ctx).ok());

    ctx = happy_path_ctx();
    ctx.source_image_verified = std::nullopt;
    REQUIRE_FALSE(Pipeline{}.add(make_source_image_verified()).run(ctx).ok());

    ctx = happy_path_ctx();
    ctx.recovery_image_present = false;
    REQUIRE_FALSE(Pipeline{}.add(make_recovery_image_present()).run(ctx).ok());

    ctx = happy_path_ctx();
    ctx.journal_safe = false;
    REQUIRE_FALSE(Pipeline{}.add(make_journal_safe()).run(ctx).ok());

    ctx = happy_path_ctx();
    REQUIRE(Pipeline{}.add(make_definition_match_verified())
                .add(make_source_image_verified())
                .add(make_recovery_image_present())
                .add(make_journal_safe())
                .run(ctx)
                .items()
                .empty());
}

TEST_CASE("WriteRegionsSafe requires every planned range to fit one declared region",
          "[policy][preflight][write_regions]") {
    auto ctx = happy_path_ctx();
    REQUIRE(Pipeline{}.add(st::policy::make_write_regions_safe()).run(ctx).items().empty());

    ctx = happy_path_ctx();
    ctx.approved_write_regions.clear();
    {
        auto const r = Pipeline{}.add(st::policy::make_write_regions_safe()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "write_regions"));
    }

    ctx = happy_path_ctx();
    ctx.planned_write_ranges = {{0x1F00, 0x200}}; // crosses the region end
    {
        auto const r = Pipeline{}.add(st::policy::make_write_regions_safe()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "write_regions"));
    }

    ctx = happy_path_ctx();
    ctx.planned_write_ranges = {{0x5000, 0x10}}; // outside the allow-list
    {
        auto const r = Pipeline{}.add(st::policy::make_write_regions_safe()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "write_regions"));
    }
}

TEST_CASE("BatteryVoltageOk tiers warn/block correctly",
          "[policy][preflight][battery]") {
    auto ctx = happy_path_ctx();
    ctx.battery_voltage_v = 11.4; // below default block threshold (11.5)
    {
        auto const r = Pipeline{}.add(make_battery_voltage_ok()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "battery_voltage"));
    }

    ctx.battery_voltage_v = 11.9; // between block (11.5) and warn (12.0)
    {
        auto const r = Pipeline{}.add(make_battery_voltage_ok()).run(ctx);
        REQUIRE(r.ok());
        REQUIRE(has_warning_in(r, "battery_voltage"));
    }

    ctx.battery_voltage_v = 12.6; // healthy
    {
        auto const r = Pipeline{}.add(make_battery_voltage_ok()).run(ctx);
        REQUIRE(r.items().empty());
    }
}

TEST_CASE("BatteryVoltageOk threshold boundaries -- exact-threshold voltage is OK",
          "[policy][preflight][battery][boundary]") {
    // The 2026-06-04 mutation lane found that swapping `<` to `<=` on
    // either threshold survived because no test exercised a voltage
    // exactly equal to block_below or warn_below. Pin both: at the
    // exact block threshold the voltage is OK (the predicate is
    // strictly less-than); the warn threshold is the same.
    auto ctx = happy_path_ctx();

    // Voltage exactly at the block threshold — NOT a blocker.
    // Mutating `v < block_below` to `v <= block_below` would
    // erroneously block at the exact threshold.
    ctx.battery_voltage_v = 11.5;
    {
        auto const r = Pipeline{}.add(make_battery_voltage_ok()).run(ctx);
        REQUIRE(r.ok());
        REQUIRE_FALSE(has_blocker_in(r, "battery_voltage"));
        // Still raises a warning since 11.5 is below the comfortable
        // 12.0 threshold — that's the existing semantic.
        REQUIRE(has_warning_in(r, "battery_voltage"));
    }

    // Voltage exactly at the warn threshold — NOT a warning either.
    // Mutating `v < warn_below` to `v <= warn_below` would
    // erroneously warn at the exact threshold.
    ctx.battery_voltage_v = 12.0;
    {
        auto const r = Pipeline{}.add(make_battery_voltage_ok()).run(ctx);
        REQUIRE(r.ok());
        REQUIRE(r.items().empty());
    }
}

TEST_CASE("IgnitionOn blocks when reported off", "[policy][preflight][ignition]") {
    auto ctx = happy_path_ctx();
    ctx.ignition_on = false;
    auto const r = Pipeline{}.add(make_ignition_on()).run(ctx);
    REQUIRE_FALSE(r.ok());
    REQUIRE(has_blocker_in(r, "ignition_on"));
}

TEST_CASE("ChecksumKnown blocks when unset or false",
          "[policy][preflight][checksum]") {
    auto ctx = happy_path_ctx();
    ctx.checksum_strategy_known = std::nullopt;
    {
        auto const r = Pipeline{}.add(make_checksum_known()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "checksum_known"));
    }

    ctx.checksum_strategy_known = false;
    {
        auto const r = Pipeline{}.add(make_checksum_known()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "checksum_known"));
    }
}

TEST_CASE("BackupPresent blocks when no verified backup exists",
          "[policy][preflight][backup]") {
    auto ctx = happy_path_ctx();
    ctx.backup_present = false;
    auto const r = Pipeline{}.add(make_backup_present()).run(ctx);
    REQUIRE_FALSE(r.ok());
    REQUIRE(has_blocker_in(r, "backup_present"));
}

TEST_CASE("WriteExtentSane warns at high fraction and blocks past 100%",
          "[policy][preflight][write_extent]") {
    auto ctx = happy_path_ctx();
    ctx.source_rom_size = std::uint64_t{1024};

    ctx.bytes_to_write = std::uint64_t{900}; // ~88% — warn
    {
        auto const r = Pipeline{}.add(make_write_extent_sane()).run(ctx);
        REQUIRE(r.ok());
        REQUIRE(has_warning_in(r, "write_extent"));
    }

    ctx.bytes_to_write = std::uint64_t{2000}; // > ROM — block
    {
        auto const r = Pipeline{}.add(make_write_extent_sane()).run(ctx);
        REQUIRE_FALSE(r.ok());
        REQUIRE(has_blocker_in(r, "write_extent"));
    }

    ctx.bytes_to_write = std::uint64_t{200}; // ~20% — no diagnostic
    {
        auto const r = Pipeline{}.add(make_write_extent_sane()).run(ctx);
        REQUIRE(r.items().empty());
    }
}

TEST_CASE("Pipeline collects every validator's diagnostics in one run",
          "[policy][preflight][pipeline]") {
    // Worst-case context: every validator should fire.
    PreflightContext ctx;
    ctx.expected_ecu_id = "A2WC520N";
    ctx.observed_ecu_id = "A2YC520N"; // ecu_id_match blocker
    ctx.expected_vin = "JF1VA1H68G9831234";
    ctx.observed_vin = "JF1VA1H68G9999999"; // vin_match blocker
    ctx.battery_voltage_v = 10.5;            // battery_voltage blocker
    ctx.ignition_on = false;                  // ignition_on blocker
    ctx.checksum_strategy_known = false;      // checksum_known blocker
    ctx.backup_present = false;               // backup_present blocker
    ctx.source_rom_size = 1024;
    ctx.bytes_to_write = 2048; // write_extent blocker

    auto const r = default_pipeline().run(ctx);
    REQUIRE_FALSE(r.ok());

    auto const blockers = r.blockers();
    // Every one of the seven validators should have contributed a blocker.
    REQUIRE(blockers.size() >= 7);
    REQUIRE(has_blocker_in(r, "ecu_id_match"));
    REQUIRE(has_blocker_in(r, "vin_match"));
    REQUIRE(has_blocker_in(r, "battery_voltage"));
    REQUIRE(has_blocker_in(r, "ignition_on"));
    REQUIRE(has_blocker_in(r, "checksum_known"));
    REQUIRE(has_blocker_in(r, "backup_present"));
    REQUIRE(has_blocker_in(r, "write_extent"));
}

TEST_CASE("Pipeline can be built incrementally with chained add",
          "[policy][preflight][pipeline]") {
    Pipeline p;
    p.add(make_ecu_id_match()).add(make_ignition_on());
    REQUIRE(p.size() == 2);
    REQUIRE_FALSE(p.empty());

    auto const r = p.run(happy_path_ctx());
    REQUIRE(r.ok());
}
