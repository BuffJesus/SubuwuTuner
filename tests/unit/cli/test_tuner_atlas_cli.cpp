// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Integration tests for `subuwutuner-cli tuner-atlas`.
//
// Cases assert end-to-end wiring: the CLI binary loads the shipped
// fixtures/tuner_atlas/tuner_atlas.toml, parses it via st::library::Atlas,
// and renders the requested sub-resource in the requested format. We point
// every invocation at the real fixture via --atlas so the test is robust to
// whatever cwd the test harness picks.

#include "../_helpers/cli_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace {

bool can_run() {
    if (!st::test::is_cli_available()) {
        WARN("CLI binary not available — set ST_CLI_BINARY_PATH or build subuwutuner-cli");
        return false;
    }
    return true;
}

std::string atlas_fixture() {
#ifdef ST_REPO_ROOT
    auto p = std::filesystem::path{ST_REPO_ROOT} / "fixtures" / "tuner_atlas" /
             "tuner_atlas.toml";
    return p.generic_string();
#else
    return {};
#endif
}

bool has_fixture() {
    auto const p = atlas_fixture();
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// Count occurrences of `needle` in `hay`. Used by list-tables to check
// that the row count matches what the atlas advertises.
std::size_t count_substr(std::string const &hay, std::string const &needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t pos = 0;;) {
        auto const at = hay.find(needle, pos);
        if (at == std::string::npos) break;
        ++n;
        pos = at + needle.size();
    }
    return n;
}

} // namespace

TEST_CASE("tuner-atlas stats text mode reports schema + counts",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const args =
        std::string{"tuner-atlas --atlas "} + st::test::quote(atlas_fixture()) + " stats";
    auto const r = st::test::cli_run(args);
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    // Text mode uses `schema_version:` label rather than the bare JSON
    // key, so check for the label form.
    CHECK(r.stdout_text.find("schema_version") != std::string::npos);
    CHECK(r.stdout_text.find("LF79103P") != std::string::npos);
    CHECK(r.stdout_text.find("170") != std::string::npos);
}

TEST_CASE("tuner-atlas stats --format json emits the schema tag",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const args = std::string{"tuner-atlas --atlas "} +
                      st::test::quote(atlas_fixture()) + " --format json stats";
    auto const r = st::test::cli_run(args);
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    CHECK(r.stdout_text.find("\"schema\":\"subuwutuner.tuner-atlas.v1\"") !=
          std::string::npos);
    CHECK(r.stdout_text.find("\"table_count\":170") != std::string::npos);
    CHECK(r.stdout_text.find("\"tuner_count\":9") != std::string::npos);
}

TEST_CASE("tuner-atlas list-tables emits all 170 tables with no filter",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const args = std::string{"tuner-atlas --atlas "} +
                      st::test::quote(atlas_fixture()) + " list-tables";
    auto const r = st::test::cli_run(args);
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    // The trailer is "(170 tables)\n" — matches only on the no-filter
    // case.
    CHECK(r.stdout_text.find("(170 tables)") != std::string::npos);
}

TEST_CASE("tuner-atlas list-tables --needs-promotion narrows the set",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const baseline_args = std::string{"tuner-atlas --atlas "} +
                               st::test::quote(atlas_fixture()) + " list-tables";
    auto const filtered_args = baseline_args + " --needs-promotion";
    auto const baseline = st::test::cli_run(baseline_args);
    auto const filtered = st::test::cli_run(filtered_args);
    REQUIRE(baseline.spawned);
    REQUIRE(filtered.spawned);
    REQUIRE(baseline.exit_code == 0);
    REQUIRE(filtered.exit_code == 0);
    auto const base_lines = count_substr(baseline.stdout_text, "\n");
    auto const filt_lines = count_substr(filtered.stdout_text, "\n");
    // The filter must reduce (or at minimum not expand) the set. Most
    // atlas tables are still pending def promotion, so a strict subset
    // is the expected outcome.
    CHECK(filt_lines <= base_lines);
}

TEST_CASE("tuner-atlas show-table prints fields for a known id",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const args = std::string{"tuner-atlas --atlas "} +
                      st::test::quote(atlas_fixture()) +
                      " show-table airflow_idle_mass_airflow_minimum";
    auto const r = st::test::cli_run(args);
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    CHECK(r.stdout_text.find("airflow_idle_mass_airflow_minimum") != std::string::npos);
    CHECK(r.stdout_text.find("FA24 portability") != std::string::npos);
    CHECK(r.stdout_text.find("Clusters") != std::string::npos);
}

TEST_CASE("tuner-atlas show-table on unknown id exits 1",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const args = std::string{"tuner-atlas --atlas "} +
                      st::test::quote(atlas_fixture()) +
                      " show-table this_table_does_not_exist";
    auto const r = st::test::cli_run(args);
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
}

TEST_CASE("tuner-atlas anchor-for 0x49B98 reports the hpfp_cluster",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    if (!has_fixture()) {
        WARN("tuner_atlas fixture missing — skipping");
        return;
    }
    auto const args = std::string{"tuner-atlas --atlas "} +
                      st::test::quote(atlas_fixture()) + " anchor-for 0x49B98";
    auto const r = st::test::cli_run(args);
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    CHECK(r.stdout_text.find("hpfp_cluster") != std::string::npos);
}

TEST_CASE("tuner-atlas with no subcommand prints usage and exits 2",
          "[cli][tuner_atlas][integration]") {
    if (!can_run()) return;
    auto const r = st::test::cli_run("tuner-atlas");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}
