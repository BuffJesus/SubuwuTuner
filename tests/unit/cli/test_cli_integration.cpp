// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Integration smoke tests for the new CLI verbs shipped in the
// 2026-06-02 session (handoff §test-coverage-gaps). Each test invokes
// the real subuwutuner-cli binary against fixtures/demo.stune (or a
// scratch tmp dir for verbs that mutate disk). Covers the contract
// these verbs commit to — exit code + stdout shape — without taking a
// dependency on the internal dispatch table.

#include "../_helpers/cli_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Project the tests run against. Resolved from the source tree at
// compile time via ST_FIXTURE_DEMO_STUNE (set in tests/CMakeLists.txt
// alongside ST_CLI_BINARY_PATH). This keeps the path correct
// regardless of which build directory the tests fire from.
std::string demo_project() {
#ifdef ST_FIXTURE_DEMO_STUNE
    return std::string{ST_FIXTURE_DEMO_STUNE};
#else
    return {};
#endif
}

// Tag the whole test file with [cli] [integration]. The CLI binary
// must exist (built before tests run) AND the demo fixture must be
// present; otherwise we surface a WARN and skip — tests that can't run
// in the current configuration shouldn't fail the suite.
bool can_run_cli_tests() {
    if (!st::test::is_cli_available()) {
        WARN("CLI binary not available — set ST_CLI_BINARY_PATH or build subuwutuner-cli");
        return false;
    }
    if (demo_project().empty() || !fs::exists(demo_project())) {
        WARN("fixtures/demo.stune not found — set ST_FIXTURE_DEMO_STUNE");
        return false;
    }
    return true;
}

bool stdout_contains(st::test::CliResult const &r, char const *needle) {
    return r.stdout_text.find(needle) != std::string::npos;
}

fs::path scratch_dir(char const *stem) {
    auto p = fs::temp_directory_path() / (std::string{"subuwutuner_cli_test_"} + stem);
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

} // namespace

TEST_CASE("CLI project-validate exits 0 on healthy demo project",
          "[cli][integration][project-validate]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("project-validate " + st::test::quote(demo_project()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "project.toml exists"));
    REQUIRE(stdout_contains(r, "Project::open succeeds"));
    REQUIRE(stdout_contains(r, "0 of"));
}

TEST_CASE("CLI project-list-roms prints text format for demo project",
          "[cli][integration][project-list-roms]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("project-list-roms " + st::test::quote(demo_project()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "source"));
    REQUIRE(stdout_contains(r, "working"));
}

TEST_CASE("CLI project-list-roms --json emits subuwutuner.project-roms.v1 schema",
          "[cli][integration][project-list-roms][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("project-list-roms --json " + st::test::quote(demo_project()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    // Loose JSON shape check — we don't pull in a JSON parser for the
    // tests. The contract is that the schema marker + role labels show
    // up; a stricter check would re-invent json parsing.
    REQUIRE(stdout_contains(r, "\"schema\":"));
    REQUIRE(stdout_contains(r, "\"working\""));
    REQUIRE(stdout_contains(r, "\"source\""));
}

TEST_CASE("CLI stats --json on demo table emits stats.v1 schema",
          "[cli][integration][stats]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("stats " + st::test::quote(demo_project()) +
                                     " --table boost_target_high_octane --json");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.stats.v1"));
    REQUIRE(stdout_contains(r, "\"table\":\"boost_target_high_octane\""));
    REQUIRE(stdout_contains(r, "\"min\":"));
    REQUIRE(stdout_contains(r, "\"max\":"));
    REQUIRE(stdout_contains(r, "\"mean\":"));
    REQUIRE(stdout_contains(r, "\"p50\":"));
}

TEST_CASE("CLI stats rejects unknown table",
          "[cli][integration][stats][error]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("stats " + st::test::quote(demo_project()) +
                                     " --table this_table_does_not_exist");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("CLI table-grep matches boost table by substring",
          "[cli][integration][table-grep]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("table-grep " + st::test::quote(demo_project()) + " boost");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "boost_target_high_octane"));
}

TEST_CASE("CLI table-grep is case-insensitive",
          "[cli][integration][table-grep]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("table-grep " + st::test::quote(demo_project()) + " BOOST");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "boost_target_high_octane"));
}

TEST_CASE("CLI table-grep no-match exits non-zero (grep convention)",
          "[cli][integration][table-grep]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("table-grep " + st::test::quote(demo_project()) +
                                     " zzz_this_table_does_not_exist");
    REQUIRE(r.spawned);
    // Mirrors Unix grep: no match → exit 1, not a "failure" but a
    // distinguishable signal. The "(no matches…)" message lands on
    // stderr (cli_run only captures stdout) so we assert on the exit
    // code alone — sufficient for the contract.
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("CLI audit stats prints per-kind counts on demo project",
          "[cli][integration][audit][stats]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r =
        st::test::cli_run("audit stats " + st::test::quote(demo_project()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "Audit log:"));
    REQUIRE(stdout_contains(r, "By kind:"));
}

TEST_CASE("CLI audit stats --json emits subuwutuner.audit-stats.v1 schema",
          "[cli][integration][audit][stats][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r =
        st::test::cli_run("audit stats --json " + st::test::quote(demo_project()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.audit-stats.v1"));
    REQUIRE(stdout_contains(r, "\"present\":true"));
    REQUIRE(stdout_contains(r, "\"by_kind\":"));
}

TEST_CASE("CLI audit stats handles a project with no audit log",
          "[cli][integration][audit][stats]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const scratch = scratch_dir("audit_stats_no_log");
    fs::create_directories(scratch);
    auto const r =
        st::test::cli_run("audit stats " + st::test::quote(scratch.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    std::error_code ec;
    fs::remove_all(scratch, ec);
}

TEST_CASE("CLI audit verify exits 0 on intact demo audit log",
          "[cli][integration][audit][verify]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r =
        st::test::cli_run("audit verify " + st::test::quote(demo_project()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
}

TEST_CASE("CLI audit append writes a custom-kind entry into project audit.log",
          "[cli][integration][audit][append]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const scratch = scratch_dir("audit_append");
    fs::create_directories(scratch);
    auto const clone = scratch / "demo.stune";
    // project-clone gives us an isolated copy so the append doesn't
    // pollute the in-tree fixture.
    auto const clone_r = st::test::cli_run("project-clone " + st::test::quote(demo_project()) +
                                           " " + st::test::quote(clone.string()));
    REQUIRE(clone_r.spawned);
    REQUIRE(clone_r.exit_code == 0);

    auto const before = fs::exists(clone / "audit.log")
                            ? fs::file_size(clone / "audit.log")
                            : std::uintmax_t{0};

    auto const append_r = st::test::cli_run(
        "audit append " + st::test::quote(clone.string()) +
        " --kind custom --desc \"integration test custom entry\"");
    REQUIRE(append_r.spawned);
    REQUIRE(append_r.exit_code == 0);

    REQUIRE(fs::exists(clone / "audit.log"));
    auto const after = fs::file_size(clone / "audit.log");
    REQUIRE(after > before);

    auto const verify_r =
        st::test::cli_run("audit verify " + st::test::quote(clone.string()));
    REQUIRE(verify_r.spawned);
    REQUIRE(verify_r.exit_code == 0);

    std::error_code ec;
    fs::remove_all(scratch, ec);
}

TEST_CASE("CLI project-clone duplicates project to new dir",
          "[cli][integration][project-clone]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const scratch = scratch_dir("project_clone");
    fs::create_directories(scratch);
    auto const dst = scratch / "cloned.stune";

    auto const r = st::test::cli_run("project-clone " + st::test::quote(demo_project()) + " " +
                                     st::test::quote(dst.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(dst / "project.toml"));
    REQUIRE(fs::exists(dst / "source.bin"));
    REQUIRE(fs::exists(dst / "working.bin"));

    // Validate the clone is self-consistent.
    auto const v = st::test::cli_run("project-validate " + st::test::quote(dst.string()));
    REQUIRE(v.exit_code == 0);

    std::error_code ec;
    fs::remove_all(scratch, ec);
}

TEST_CASE("CLI profile create + delete round-trip",
          "[cli][integration][profile][create][delete]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const scratch = scratch_dir("profile_roundtrip");
    fs::create_directories(scratch);
    std::string const id = "cli_integration_test_profile";

    auto const create_r = st::test::cli_run(
        "profile create --id " + id + " --dir " + st::test::quote(scratch.string()) +
        " --make Subaru --model WRX --year 2017");
    REQUIRE(create_r.spawned);
    REQUIRE(create_r.exit_code == 0);
    REQUIRE(fs::exists(scratch / (id + ".stprofile")));

    // profile delete uses POSITIONAL id (not --id), matching the
    // contract in src/cli/main.cpp:13908-13951.
    auto const delete_r = st::test::cli_run(
        "profile delete " + id + " --dir " + st::test::quote(scratch.string()) + " --yes");
    REQUIRE(delete_r.spawned);
    REQUIRE(delete_r.exit_code == 0);
    REQUIRE_FALSE(fs::exists(scratch / (id + ".stprofile")));

    std::error_code ec;
    fs::remove_all(scratch, ec);
}

TEST_CASE("CLI profile delete refuses without --yes",
          "[cli][integration][profile][delete][safety]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const scratch = scratch_dir("profile_no_yes");
    fs::create_directories(scratch);
    std::string const id = "cli_integration_no_yes_profile";

    REQUIRE(st::test::cli_run("profile create --id " + id + " --dir " +
                              st::test::quote(scratch.string()))
                .exit_code == 0);

    // Positional id; no --yes.
    auto const r = st::test::cli_run("profile delete " + id + " --dir " +
                                     st::test::quote(scratch.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code != 0);
    // Profile should still exist.
    REQUIRE(fs::exists(scratch / (id + ".stprofile")));

    std::error_code ec;
    fs::remove_all(scratch, ec);
}

TEST_CASE("CLI completion bash emits shell-loadable script",
          "[cli][integration][completion]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("completion bash");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "_subuwutuner_cli"));
    REQUIRE(stdout_contains(r, "complete"));
}

TEST_CASE("CLI completion zsh emits a shell-loadable script",
          "[cli][integration][completion]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("completion zsh");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "#compdef"));
}

TEST_CASE("CLI changelog show prints [Unreleased] by default",
          "[cli][integration][changelog]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("changelog show");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "[Unreleased]"));
}

TEST_CASE("CLI knock-snapshot --json emits subuwutuner.knock-snapshot.v1 schema",
          "[cli][integration][knock-snapshot][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-knock-log.csv";
    auto const r = st::test::cli_run(
        "knock-snapshot --log " + st::test::quote(csv.string()) +
        " --flkc-cols FLKC1,FLKC2,FLKC3,FLKC4 --rpm-col RPM --load-col Load --json");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.knock-snapshot.v1"));
    REQUIRE(stdout_contains(r, "\"per_cyl\":["));
    REQUIRE(stdout_contains(r, "\"cylinder_count\":"));
}

TEST_CASE("CLI coldstart-analyze --json emits subuwutuner.coldstart-analyze.v1 schema",
          "[cli][integration][coldstart-analyze][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-coldstart-log.csv";
    auto const r = st::test::cli_run(
        "coldstart-analyze --log " + st::test::quote(csv.string()) +
        " --timestamp-col ts --ect-col ect --observed-lambda-col obs --rpm-col rpm --json");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.coldstart-analyze.v1"));
    REQUIRE(stdout_contains(r, "\"phases\":["));
    REQUIRE(stdout_contains(r, "\"ect_bins\":["));
    REQUIRE(stdout_contains(r, "\"target_curve_set\":"));
}

TEST_CASE("CLI knock-snapshot --csv emits comment metadata + per-cyl table",
          "[cli][integration][knock-snapshot][csv]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-knock-log.csv";
    auto const r = st::test::cli_run(
        "knock-snapshot --log " + st::test::quote(csv.string()) +
        " --flkc-cols FLKC1,FLKC2,FLKC3,FLKC4 --rpm-col RPM --load-col Load --csv");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "# schema=subuwutuner.knock-snapshot.v1"));
    REQUIRE(stdout_contains(r, "# cylinder_count="));
    REQUIRE(stdout_contains(r, "cyl,has_flkc,has_fbkc,flkc_current"));
}

TEST_CASE("CLI coldstart-analyze --csv emits comment metadata + per-bin table",
          "[cli][integration][coldstart-analyze][csv]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-coldstart-log.csv";
    auto const r = st::test::cli_run(
        "coldstart-analyze --log " + st::test::quote(csv.string()) +
        " --timestamp-col ts --ect-col ect --observed-lambda-col obs --rpm-col rpm --csv");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "# schema=subuwutuner.coldstart-analyze.v1"));
    REQUIRE(stdout_contains(r, "# phase_PreCrank_samples="));
    REQUIRE(stdout_contains(r, "ect_center_c,count,observed_lambda_mean"));
}

TEST_CASE("CLI dump-table --json emits subuwutuner.dump-table.v1 schema",
          "[cli][integration][dump-table][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const pack = fs::path{demo_project()} / ".." / "demo-pack" / "pack.toml";
    auto const rom = fs::path{demo_project()} / "source.bin";
    auto const r = st::test::cli_run(
        "dump-table --def " + st::test::quote(pack.string()) +
        " --table boost_target_high_octane --json " +
        st::test::quote(rom.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.dump-table.v1"));
    REQUIRE(stdout_contains(r, "\"axis_x\":"));
    REQUIRE(stdout_contains(r, "\"values\":"));
    REQUIRE(stdout_contains(r, "\"dimensions\":2"));
}

TEST_CASE("CLI dump-table rejects --json + --csv combo",
          "[cli][integration][dump-table][json][csv]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const pack = fs::path{demo_project()} / ".." / "demo-pack" / "pack.toml";
    auto const rom = fs::path{demo_project()} / "source.bin";
    auto const r = st::test::cli_run(
        "dump-table --def " + st::test::quote(pack.string()) +
        " --table boost_target_high_octane --json --csv " +
        st::test::quote(rom.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("CLI rom-diff --json emits subuwutuner.rom-diff.v1 schema",
          "[cli][integration][rom-diff][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    // Compare the demo project's source.bin against itself. The schema
    // shape (header keys + empty changes array) is what we want to
    // pin — actual diff counts get exercised by the st::diff unit
    // tests, not the CLI integration suite.
    auto const pack = fs::path{demo_project()} / ".." / "demo-pack" / "pack.toml";
    auto const rom = fs::path{demo_project()} / "source.bin";
    auto const r = st::test::cli_run(
        "rom-diff --def " + st::test::quote(pack.string()) +
        " --json " + st::test::quote(rom.string()) + " " +
        st::test::quote(rom.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.rom-diff.v1"));
    REQUIRE(stdout_contains(r, "\"rom_a\":{"));
    REQUIRE(stdout_contains(r, "\"rom_b\":{"));
    REQUIRE(stdout_contains(r, "\"pack\":{\"id\":"));
    REQUIRE(stdout_contains(r, "\"tables_compared\":"));
    REQUIRE(stdout_contains(r, "\"changes\":["));
}

TEST_CASE("CLI ai-drift classifies the demo adaptive-history CSV",
          "[cli][integration][ai-drift]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-adaptive-history.csv";
    auto const r = st::test::cli_run(
        "ai-drift --log " + st::test::quote(csv.string()) +
        " --timestamp-col ts --ltft-col ltft --dam-col dam --idle-adapt-col iac");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "Drift diagnosis"));
    REQUIRE(stdout_contains(r, "Confidence:"));
}

TEST_CASE("CLI ai-drift --json emits subuwutuner.ai-drift.v1 schema",
          "[cli][integration][ai-drift][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-adaptive-history.csv";
    auto const r = st::test::cli_run(
        "ai-drift --json --log " + st::test::quote(csv.string()) +
        " --timestamp-col ts --ltft-col ltft --dam-col dam --idle-adapt-col iac");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.ai-drift.v1"));
    REQUIRE(stdout_contains(r, "\"cause\":"));
    REQUIRE(stdout_contains(r, "\"confidence\":"));
    REQUIRE(stdout_contains(r, "\"evidence\":["));
}

TEST_CASE("CLI ai-drift rejects unknown column",
          "[cli][integration][ai-drift][error]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const csv = fs::path{demo_project()} / ".." / "demo-adaptive-history.csv";
    auto const r = st::test::cli_run(
        "ai-drift --log " + st::test::quote(csv.string()) +
        " --timestamp-col ts --ltft-col not_a_real_column");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code != 0);
}

TEST_CASE("CLI ai-drift exits 2 on missing required args",
          "[cli][integration][ai-drift][error]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("ai-drift");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("CLI pack-lint exits 0 on the demo pack",
          "[cli][integration][pack-lint]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const pack = fs::path{demo_project()} / ".." / "demo-pack" / "pack.toml";
    auto const r = st::test::cli_run("pack-lint " + st::test::quote(pack.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "Validation: OK"));
}

TEST_CASE("CLI pack-lint --json emits subuwutuner.pack-lint.v1 schema",
          "[cli][integration][pack-lint][json]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const pack = fs::path{demo_project()} / ".." / "demo-pack" / "pack.toml";
    auto const r = st::test::cli_run("pack-lint --json " + st::test::quote(pack.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "subuwutuner.pack-lint.v1"));
    REQUIRE(stdout_contains(r, "\"ok\":true"));
}

TEST_CASE("CLI pack-lint exits 1 when path doesn't exist",
          "[cli][integration][pack-lint][error]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("pack-lint /tmp/does_not_exist_pack.toml");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
}

TEST_CASE("CLI pack-lint exits 2 on missing arg",
          "[cli][integration][pack-lint][error]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("pack-lint");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("CLI project-info --audit-summary surfaces audit bucket counts",
          "[cli][integration][project-info][audit-summary]") {
    if (!can_run_cli_tests()) {
        return;
    }
    auto const r = st::test::cli_run("project-info " + st::test::quote(demo_project()) +
                                     " --audit-summary");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(stdout_contains(r, "Audit"));
}
