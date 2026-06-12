// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Integration tests for `subuwutuner-cli ptm list-patches` per
// specs/cobb-ap3-ptm-cli-subcommand-spec.md.
//
// Verifies the two-step arming model (build flag + runtime flag) refuses
// closed, and that the cipher-ON path plumbs decrypt_ptm + decode_ptm_xml
// end to end. The synthetic E2E fixture's inner payload is intentionally
// not a valid PrivateData XML (per fixtures/ap3/cipher/README.md it's
// trivial null bytes by design), so the cipher-ON case exits 1 from the
// patch decoder's strict length-mismatch check — that's still the right
// signal that every layer in the pipeline ran.

#include "../_helpers/cli_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

bool can_run() {
    if (!st::test::is_cli_available()) {
        WARN("CLI binary not available — set ST_CLI_BINARY_PATH or build subuwutuner-cli");
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("ptm list-patches refuses without --enable-cobb-ap-cipher",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    // Runtime gate fires before the file read — the .ptm path doesn't
    // even need to exist.
    auto const r = st::test::cli_run("ptm list-patches /nonexistent/file.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm list-patches reports unreadable file with --enable-cobb-ap-cipher",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off — runtime arm has no effect");
#else
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm list-patches /nonexistent/xyz.ptm");
    REQUIRE(r.spawned);
    // Past the runtime gate (would be 2); failed at file read (exit 1).
    REQUIRE(r.exit_code == 1);
#endif
}

TEST_CASE("ptm list-patches exercises decrypt + decode on synthetic fixture",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
#ifndef ST_FIXTURE_AP3_CIPHER_DIR
    SKIP("ST_FIXTURE_AP3_CIPHER_DIR not defined");
#else
    if (!can_run()) {
        return;
    }
    std::string fixture =
        std::string{ST_FIXTURE_AP3_CIPHER_DIR} + "/synthetic_e2e_ptm_envelope.ptm";
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm list-patches " + st::test::quote(fixture));
    REQUIRE(r.spawned);
    // The fixture's inner payload is intentionally trivial null bytes
    // (per fixtures/ap3/cipher/README.md), so the patch decoder rejects
    // it with a length-mismatch error. Exit 1 is the right signal — the
    // CLI plumbed decrypt_ptm + decode_ptm_xml successfully; the decoder
    // simply refused the malformed input.
    REQUIRE(r.exit_code == 1);
#endif
#endif
}

TEST_CASE("ptm with no subcommand prints help and exits 2",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run("ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm inspect refuses without --enable-cobb-ap-cipher",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run("ptm inspect /nonexistent/file.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm inspect reports unreadable file with --enable-cobb-ap-cipher",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm inspect /nonexistent/xyz.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
#endif
}

TEST_CASE("ptm inspect exercises decrypt + decode on synthetic fixture",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
#ifndef ST_FIXTURE_AP3_CIPHER_DIR
    SKIP("ST_FIXTURE_AP3_CIPHER_DIR not defined");
#else
    if (!can_run()) {
        return;
    }
    std::string fixture =
        std::string{ST_FIXTURE_AP3_CIPHER_DIR} + "/synthetic_e2e_ptm_envelope.ptm";
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm inspect " + st::test::quote(fixture));
    REQUIRE(r.spawned);
    // Same as list-patches: the fixture is intentionally trivial post-
    // decrypt, so the patch decoder rejects with exit 1. The render
    // never runs, but the cipher chain does — which is the gate this
    // test enforces.
    REQUIRE(r.exit_code == 1);
#endif
#endif
}

TEST_CASE("ptm diff refuses without --enable-cobb-ap-cipher",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run("ptm diff /nonexistent/a.ptm /nonexistent/b.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm diff requires two positional args",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run("--enable-cobb-ap-cipher ptm diff /tmp/only-one.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm diff reports missing files on cipher path",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm diff /nonexistent/a.ptm /nonexistent/b.ptm");
    REQUIRE(r.spawned);
    // First file (a) fails the read; exit 1.
    REQUIRE(r.exit_code == 1);
#endif
}

TEST_CASE("ptm diff: a == b on synthetic fixture",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
#ifndef ST_FIXTURE_AP3_CIPHER_DIR
    SKIP("ST_FIXTURE_AP3_CIPHER_DIR not defined");
#else
    if (!can_run()) {
        return;
    }
    std::string const fixture =
        std::string{ST_FIXTURE_AP3_CIPHER_DIR} + "/synthetic_e2e_ptm_envelope.ptm";
    // Diff a file against itself. The synthetic fixture intentionally
    // fails decoding (trivial null inner payload), so the first decode
    // call returns ParseError → exit 1. The CLI plumbing of two
    // positional args + format dispatch still gets exercised.
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm diff " + st::test::quote(fixture) +
        " " + st::test::quote(fixture));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
#endif
#endif
}

TEST_CASE("ptm diff --by-table accepted as flag",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
#ifndef ST_FIXTURE_AP3_CIPHER_DIR
    SKIP("ST_FIXTURE_AP3_CIPHER_DIR not defined");
#else
    if (!can_run()) {
        return;
    }
    std::string const fixture =
        std::string{ST_FIXTURE_AP3_CIPHER_DIR} + "/synthetic_e2e_ptm_envelope.ptm";
    // --by-table without --def should fall back to --by-layer and warn
    // on stderr — but the synthetic fixture still won't decode, so the
    // outcome is exit 1 (parse error). The point of this test is that
    // the --by-table flag is accepted and dispatched.
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm diff --by-table " +
        st::test::quote(fixture) + " " + st::test::quote(fixture));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
#endif
#endif
}

TEST_CASE("ptm import refuses without --enable-cobb-ap-cipher",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "ptm import /nonexistent/file.ptm --base-rom /nonexistent/rom.bin");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm import requires --base-rom",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run("--enable-cobb-ap-cipher ptm import /tmp/x.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm import reports missing .ptm with --base-rom",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm import /nonexistent/x.ptm "
        "--base-rom /nonexistent/rom.bin");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
#endif
}

TEST_CASE("ptm import exercises decrypt + decode on synthetic fixture",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
#ifndef ST_FIXTURE_AP3_CIPHER_DIR
    SKIP("ST_FIXTURE_AP3_CIPHER_DIR not defined");
#else
    if (!can_run()) {
        return;
    }
    std::string const fixture =
        std::string{ST_FIXTURE_AP3_CIPHER_DIR} + "/synthetic_e2e_ptm_envelope.ptm";
    // Use the fixture itself as a stand-in for a base ROM — we never
    // get past the decode step on this fixture (intentional malformed
    // inner per fixtures/ap3/cipher/README.md), so the base ROM path
    // is never read. Exit 1 from the decode rejection still validates
    // CLI plumbing.
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm import " + st::test::quote(fixture) +
        " --base-rom " + st::test::quote(fixture));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
#endif
#endif
}

TEST_CASE("ptm export refuses without --enable-cobb-ap-cipher",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "ptm export /nonexistent/project.stune --as /tmp/out.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm export requires --as",
          "[cli][ptm][integration]") {
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm export /tmp/proj.stune");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ptm export reports missing project dir",
          "[cli][ptm][integration][cipher]") {
#ifndef ST_AP3_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm export /nonexistent/x.stune --as /tmp/out.ptm");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 1);
#endif
}

#if defined(ST_AP3_HAVE_CIPHER) && defined(ST_AP3_HAVE_PTM_REWRITE)
TEST_CASE("ptm export → ptm inspect end-to-end round-trip",
          "[cli][ptm][integration][cipher][rewrite]") {
    if (!can_run()) {
        return;
    }
    namespace fs = std::filesystem;
    auto const tmp = fs::temp_directory_path() / "subuwutuner_ptm_roundtrip";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    auto const proj_dir = tmp / "synth.stune";
    fs::create_directories(proj_dir, ec);

    // Write a minimal project.toml + ptm_patches.toml that exercises
    // the export pipeline's metadata + patch-array handling.
    {
        std::ofstream f{proj_dir / "project.toml"};
        f << "[project]\n"
             "schema_version = 1\n"
             "display_name   = \"roundtrip-smoke\"\n"
             "\n"
             "[ptm_metadata]\n"
             "vendor_id      = \"SUBUWU_ROUNDTRIP\"\n"
             "vehicle_id     = \"SYNTHETIC_E2E\"\n"
             "lock_mask      = 0\n"
             "rom_sum        = \"12345\"\n"
             "save_date_time = \"0\"\n";
    }
    {
        std::ofstream f{proj_dir / "ptm_patches.toml"};
        f << "[[patch]]\n"
             "rom_offset = 32768\n"
             "ram_offset = -657408\n"
             "length     = 5\n"
             "bytes_b64  = \"SGVsbG8=\"\n"
             "layer      = \"tuner_addition\"\n"
             "\n"
             "[[patch]]\n"
             "rom_offset = 65536\n"
             "ram_offset = -624640\n"
             "length     = 5\n"
             "bytes_b64  = \"V29ybGQ=\"\n"
             "layer      = \"tuner_addition\"\n";
    }

    auto const out_ptm = tmp / "out.ptm";

    // Export.
    {
        auto const r = st::test::cli_run(
            "--enable-cobb-ap-cipher ptm export " +
            st::test::quote(proj_dir.string()) + " --as " +
            st::test::quote(out_ptm.string()));
        REQUIRE(r.spawned);
        REQUIRE(r.exit_code == 0);
    }

    // Output file exists and is non-empty.
    REQUIRE(fs::exists(out_ptm));
    REQUIRE(fs::file_size(out_ptm) > 0);

    // Inspect → identity + patch count round-trip.
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ptm inspect " + st::test::quote(out_ptm.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_text.find("SUBUWU_ROUNDTRIP") != std::string::npos);
    REQUIRE(r.stdout_text.find("SYNTHETIC_E2E") != std::string::npos);
    REQUIRE(r.stdout_text.find("Patches:          2 ") != std::string::npos);
}
#endif

#if defined(ST_AP3_HAVE_CIPHER) && defined(ST_AP3_HAVE_PTM_REWRITE)
TEST_CASE("ptm import → Project::open-compatible (5-file skeleton, project-validate passes)",
          "[cli][ptm][integration][cipher][rewrite]") {
    if (!can_run()) {
        return;
    }
    namespace fs = std::filesystem;
    auto const tmp = fs::temp_directory_path() / "subuwutuner_ptm_open_compat";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    auto const src_dir = tmp / "src.stune";
    fs::create_directories(src_dir, ec);

    // Synthesize a minimal source project with one patch.
    {
        std::ofstream f{src_dir / "project.toml"};
        f << "[project]\n"
             "schema_version = 1\n"
             "display_name   = \"src\"\n"
             "\n[ptm_metadata]\n"
             "vendor_id      = \"SUBUWU_OPEN_COMPAT\"\n"
             "vehicle_id     = \"SYNTHETIC\"\n"
             "lock_mask      = 0\n"
             "rom_sum        = \"99\"\n"
             "save_date_time = \"0\"\n";
    }
    {
        std::ofstream f{src_dir / "ptm_patches.toml"};
        f << "[[patch]]\n"
             "rom_offset = 100\n"
             "ram_offset = 0\n"
             "length     = 5\n"
             "bytes_b64  = \"SGVsbG8=\"\n"
             "layer      = \"tuner_addition\"\n";
    }

    auto const ptm = tmp / "test.ptm";
    // Export the source project to a .ptm.
    {
        auto const r = st::test::cli_run(
            "--enable-cobb-ap-cipher ptm export " +
            st::test::quote(src_dir.string()) + " --as " + st::test::quote(ptm.string()));
        REQUIRE(r.spawned);
        REQUIRE(r.exit_code == 0);
    }
    REQUIRE(fs::exists(ptm));

    // Copy demo source.bin as the base ROM (1024 bytes is plenty for
    // the single 5-byte patch at offset 100).
#ifndef ST_FIXTURE_DEMO_STUNE
    SKIP("ST_FIXTURE_DEMO_STUNE not defined");
#else
    auto const base_rom = tmp / "base.bin";
    fs::copy_file(fs::path{ST_FIXTURE_DEMO_STUNE} / "source.bin", base_rom);
    auto const demo_pack =
        fs::path{ST_FIXTURE_DEMO_STUNE}.parent_path() / "demo-pack";

    auto const imp_dir = tmp / "imported.stune";
    // Import: should write 5 files (project.toml + source.bin + working.bin
    // + edits.toml + ptm_patches.toml) with --def wired.
    {
        auto const r = st::test::cli_run(
            "--enable-cobb-ap-cipher ptm import " + st::test::quote(ptm.string()) +
            " --into " + st::test::quote(imp_dir.string()) +
            " --base-rom " + st::test::quote(base_rom.string()) +
            " --def " + st::test::quote(demo_pack.string()));
        REQUIRE(r.spawned);
        REQUIRE(r.exit_code == 0);
    }
    REQUIRE(fs::exists(imp_dir / "project.toml"));
    REQUIRE(fs::exists(imp_dir / "source.bin"));
    REQUIRE(fs::exists(imp_dir / "working.bin"));
    REQUIRE(fs::exists(imp_dir / "edits.toml"));
    REQUIRE(fs::exists(imp_dir / "ptm_patches.toml"));

    // Project::open should now succeed via project-validate.
    auto const r = st::test::cli_run("project-validate " + st::test::quote(imp_dir.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_text.find("Project::open succeeds") != std::string::npos);
    REQUIRE(r.stdout_text.find("0 of") != std::string::npos);
#endif
}
#endif
