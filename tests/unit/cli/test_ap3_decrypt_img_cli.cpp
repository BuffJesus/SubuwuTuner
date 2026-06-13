// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// CLI integration tests for `subuwutuner-cli ap3 decrypt-img` — the
// standalone (no-device) OTA .img decryption surface.

#include "../_helpers/cli_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

bool can_run() {
    if (!st::test::is_cli_available()) {
        WARN("CLI binary not available — set ST_CLI_BINARY_PATH or build subuwutuner-cli");
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("ap3 decrypt-img refuses without --enable-cobb-ap-cipher",
          "[cli][ap3][decrypt-img]") {
    if (!can_run()) {
        return;
    }
    // Runtime gate fires before the input file is read — path doesn't
    // need to exist.
    auto const r = st::test::cli_run("ap3 decrypt-img /nonexistent/file.img");
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("ap3 decrypt-img reports missing input with --enable-cobb-ap-cipher",
          "[cli][ap3][decrypt-img][cipher]") {
#ifndef ST_ETS_HAVE_CIPHER
    SKIP("Cipher gating off — runtime arm has no effect");
#else
    if (!can_run()) {
        return;
    }
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ap3 decrypt-img /nonexistent/file.img");
    REQUIRE(r.spawned);
    // Past the runtime gate (would be 2); failed at input open (exit 1).
    REQUIRE(r.exit_code == 1);
#endif
}

TEST_CASE("ap3 decrypt-img round-trips the OTA .img fixture",
          "[cli][ap3][decrypt-img][cipher][fixtures]") {
#ifndef ST_ETS_HAVE_CIPHER
    SKIP("Cipher gating off");
#else
#ifndef ST_FIXTURE_ETS_CIPHER_DIR
    SKIP("ST_FIXTURE_ETS_CIPHER_DIR not defined");
#else
    if (!can_run()) {
        return;
    }
    namespace fs = std::filesystem;
    std::string const input =
        std::string{ST_FIXTURE_ETS_CIPHER_DIR} + "/blowfish_output_full.bin";
    std::string const expected =
        std::string{ST_FIXTURE_ETS_CIPHER_DIR} + "/blowfish_input.bin";
    if (!fs::exists(input) || !fs::exists(expected)) {
        SKIP("blowfish fixtures not staged");
    }
    auto const out = fs::temp_directory_path() / "subuwutuner_ota_cli_test.bin";
    std::error_code ec;
    fs::remove(out, ec);
    auto const r = st::test::cli_run(
        "--enable-cobb-ap-cipher ap3 decrypt-img " + st::test::quote(input) +
        " -o " + st::test::quote(out.string()));
    REQUIRE(r.spawned);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(out));
    // Byte-identity vs the analyst-staged plaintext.
    std::ifstream got{out, std::ios::binary | std::ios::ate};
    REQUIRE(got);
    auto const got_sz = got.tellg();
    got.seekg(0);
    std::vector<char> got_bytes(static_cast<std::size_t>(got_sz));
    got.read(got_bytes.data(), got_sz);

    std::ifstream want{expected, std::ios::binary | std::ios::ate};
    REQUIRE(want);
    auto const want_sz = want.tellg();
    want.seekg(0);
    std::vector<char> want_bytes(static_cast<std::size_t>(want_sz));
    want.read(want_bytes.data(), want_sz);

    REQUIRE(got_bytes.size() == want_bytes.size());
    REQUIRE(got_bytes == want_bytes);
#endif
#endif
}
