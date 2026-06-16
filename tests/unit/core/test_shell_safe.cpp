// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for st::validate_shell_safe_path and st::validate_api_key.
//
// Belt-+-suspenders companion to validate_ptm_metadata_field: where
// that validator hardens the .ptm metadata path, these two harden the
// three other places SubuwuTuner shells out (curl in AI backend,
// `explorer /select`/`open -R`/`xdg-open` reveal in audit panel and
// settings modal). See `findings/tuning-knowledge-2026-06-13/cmd22-path-re/
// shell_injection_sinks.md` for the architectural rationale.

#include "st/core/shell_safe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

// =============================================================================
// validate_shell_safe_path
// =============================================================================

TEST_CASE("validate_shell_safe_path accepts realistic project paths",
          "[core][shell_safe]") {
    char const *ok[] = {
        // Windows
        "C:\\Users\\Cornelio\\Documents\\subuwutuner\\projects\\daily.stune",
        "C:\\Program Files (x86)\\Foo\\bar.exe",
        "D:/Subuwu/code/build/audit.log",
        // macOS
        "/Users/cornelio/Library/Application Support/SubuwuTuner/audit.log",
        // Linux
        "/home/cornelio/.config/subuwutuner/projects/wrk3.stune",
        // Paths with spaces (Windows + macOS common case)
        "C:\\My Documents\\proj.stune",
        "/Volumes/External Drive/Tunes/wrk3.stune",
        // Empty is accepted — caller decides whether it's meaningful
        "",
    };
    for (auto const *p : ok) {
        auto const r = st::validate_shell_safe_path("test path", p);
        REQUIRE(r.has_value());
    }
}

TEST_CASE("validate_shell_safe_path rejects shell metacharacters",
          "[core][shell_safe]") {
    // Each of these would break out of the `"…"` quoting on at least
    // one supported platform when interpolated into the reveal command.
    char const *bad[] = {
        "C:\\Users\\Cornelio\\proj\"oops.stune",      // closes the quote
        "C:\\Users\\Cornelio\\proj`whoami`.stune",    // backtick command sub
        "C:\\Users\\Cornelio\\proj$VAR.stune",        // variable expansion
        "/home/user/proj|cat.stune",                  // pipe
        "/home/user/proj;ls.stune",                   // statement separator
        "/home/user/proj&disown.stune",               // background + disown
        "/home/user/proj<redirect.stune",             // input redirect
        "/home/user/proj>redirect.stune",             // output redirect
        "C:\\Users\\Cornelio\\proj\nNEWLINE.stune",  // newline → new command
        "C:\\Users\\Cornelio\\proj\rCR.stune",       // cmd.exe statement sep
    };
    for (auto const *p : bad) {
        auto const r = st::validate_shell_safe_path("test path", p);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("validate_shell_safe_path rejects embedded NUL",
          "[core][shell_safe]") {
    // NUL would truncate the C-string before system() runs (no RCE
    // possible because of that truncation alone), but we reject anyway
    // so the failure mode is observable: the button refuses to act
    // instead of doing nothing-with-no-explanation.
    std::string const bad = std::string{"C:\\foo", 6} + '\0' + "bar";
    auto const r = st::validate_shell_safe_path("test path", bad);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("validate_shell_safe_path accepts single quote", "[core][shell_safe]") {
    // The reveal commands wrap their argument in `"…"`, not `'…'`, so
    // a literal `'` in the path doesn't escape anything. This is a
    // legitimate Apple HOME-directory character.
    auto const r =
        st::validate_shell_safe_path("test path", "/Users/cornelio's mac/proj.stune");
    REQUIRE(r.has_value());
}

TEST_CASE("validate_shell_safe_path error message names field + offset + byte",
          "[core][shell_safe]") {
    auto const r =
        st::validate_shell_safe_path("audit reveal path", "/home/foo`bad");
    REQUIRE_FALSE(r.has_value());
    auto const msg = r.error().to_string();
    CHECK(msg.find("audit reveal path") != std::string::npos);
    CHECK(msg.find("0x60") != std::string::npos); // ` is 0x60
    CHECK(msg.find("9") != std::string::npos);    // offset of `
}

// =============================================================================
// validate_api_key
// =============================================================================

TEST_CASE("validate_api_key accepts realistic provider keys",
          "[core][shell_safe][api_key]") {
    // Synthetic but structurally-realistic keys. NOT real keys —
    // these are the canonical shape the providers document.
    char const *ok[] = {
        "sk-ant-api03-AbCd1234-_EfGh5678",          // Anthropic shape
        "sk-proj-AbCdEfGhIjKlMnOpQrStUvWx",         // OpenAI new shape
        "sk-AbCdEfGhIjKlMnOpQrStUvWx12345678",      // OpenAI legacy shape
        "0123456789abcdef0123456789abcdef",         // hex-ish
        "test_key_for_local_stub_backend",          // unit-test fixture
    };
    for (auto const *k : ok) {
        auto const r = st::validate_api_key("test", k);
        REQUIRE(r.has_value());
    }
}

TEST_CASE("validate_api_key rejects shell metacharacters",
          "[core][shell_safe][api_key]") {
    char const *bad[] = {
        "sk-ant-`whoami`",
        "sk-ant-$(touch /tmp)",
        "sk-ant-;ls",
        "sk-ant-\"quote",
        "sk-ant-'apostrophe",   // closes the `-H "…"` arg via `'` inside `"`? No,
                                  // but `'` isn't a real provider char and we'd
                                  // rather not gamble — provider keys are A-Za-z0-9._-.
        "sk-ant-\\back",
        "sk-ant-|pipe",
        "sk-ant-&background",
        "sk-ant- space",        // space — would split the curl arg
        "sk-ant-\nnewline",
        "sk-ant-\ttab",
        "sk-ant-:colon",        // : is the header separator
        "sk-ant-/slash",
        "sk-ant-=equals",
    };
    for (auto const *k : bad) {
        auto const r = st::validate_api_key("test", k);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("validate_api_key rejects non-ASCII",
          "[core][shell_safe][api_key]") {
    char const *bad[] = {
        "sk-ant-\xC3\xA9",        // UTF-8 é
        "sk-ant-\xE4\xB8\xAD",    // UTF-8 中
        "sk-ant-\xFF",            // 0xFF
        "sk-ant-\x80",            // high-bit alone
    };
    for (auto const *k : bad) {
        auto const r = st::validate_api_key("test", k);
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("validate_api_key rejects control characters",
          "[core][shell_safe][api_key]") {
    std::string bad[] = {
        std::string{"sk-ant", 6} + '\0' + "cut",
        "sk-ant-\x01" "soh",
        "sk-ant-\x1B" "esc",
        "sk-ant-\x7F" "del",
    };
    for (auto const &k : bad) {
        auto const r = st::validate_api_key("test", k);
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("validate_api_key caps length at 512 chars",
          "[core][shell_safe][api_key]") {
    std::string ok(512, 'a');
    REQUIRE(st::validate_api_key("test", ok).has_value());
    std::string over(513, 'a');
    auto const r = st::validate_api_key("test", over);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("validate_api_key accepts empty input",
          "[core][shell_safe][api_key]") {
    // The AI backend rejects empty keys separately as a "you haven't
    // configured a key" error — this validator's job is the
    // shell-safety check, not the empty-string check. Keep them
    // orthogonal so callers can compose them in whatever order.
    auto const r = st::validate_api_key("test", "");
    REQUIRE(r.has_value());
}

TEST_CASE("validate_api_key error message names provider + byte + offset",
          "[core][shell_safe][api_key]") {
    auto const r = st::validate_api_key("anthropic", "sk-ant-$bad");
    REQUIRE_FALSE(r.has_value());
    auto const msg = r.error().to_string();
    CHECK(msg.find("anthropic") != std::string::npos);
    CHECK(msg.find("0x24") != std::string::npos); // $ is 0x24
    CHECK(msg.find("7") != std::string::npos);    // offset of $
}
