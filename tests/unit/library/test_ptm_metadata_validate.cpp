// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for st::library::validate_ptm_metadata_field.
//
// Defensive validator that rejects shell metacharacters + non-ASCII in
// .ptm metadata fields (vendorId / vehicleId / romSum / saveDateTime).
// Wired into both CLI and GUI export paths so a maliciously-crafted
// project can't ship a .ptm whose metadata triggers shell injection
// in the consuming firmware's INI-extract-then-eval pattern (see
// findings/tuning-knowledge-2026-06-13/cmd22-path-re/).

#include "st/library/ptm_xml_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("validate_ptm_metadata_field accepts canonical real-world IDs",
          "[library][ptm][validate]") {
    // Spot-check against actual vendor / vehicle IDs observed in the
    // corpus snapshot at findings/tuning-knowledge-2026-06-13/fingerprints/.
    // All within the allowed [A-Z a-z 0-9 . _ - space] set. Note that
    // "Stage1+SF" is the marketing label, not the underlying vendor /
    // vehicle ID — the actual on-wire vendor for that tune family is
    // "COBB", which is alphanumeric and fine.
    char const *ids[] = {"COBB", "W585", "SUBA_US_WRXM_CF_17_E",
                          "SUBA_US_WRXM_CF_17_F", "812800817",
                          "1060268288", "SUBARU WRX"};
    for (auto const *id : ids) {
        auto const r = st::library::validate_ptm_metadata_field("vendorId", id);
        REQUIRE(r.has_value());
    }
}

TEST_CASE("validate_ptm_metadata_field rejects shell metacharacters",
          "[library][ptm][validate]") {
    // Each of these is a single shell-meta that would let the AP-side
    // sed+eval pipeline reach arbitrary code execution if it landed in
    // a user-readable settings/config file.
    char const *bad[] = {
        "VENDOR`whoami`",       // backtick command sub
        "VENDOR$(touch /tmp)",  // $() command sub
        "VENDOR;ls",            // statement separator
        "VENDOR&disown",        // background + disown
        "VENDOR|cat",           // pipe
        "VENDOR\"quote",        // double quote — closes the eval string
        "VENDOR'apostrophe",    // apostrophe
        "VENDOR\\back",         // backslash — eval escape
        "VENDOR\nNEWLINE",      // newline — starts new command in eval
        "VENDOR{brace}",        // brace expansion
        "VENDOR>redirect",      // redirect
        "VENDOR<redirect",      // redirect
    };
    for (auto const *id : bad) {
        auto const r = st::library::validate_ptm_metadata_field("vendorId", id);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
    }
}

TEST_CASE("validate_ptm_metadata_field rejects non-ASCII",
          "[library][ptm][validate]") {
    char const *bad[] = {
        "VENDOR\xC3\xA9",         // UTF-8 é
        "VENDOR\xE4\xB8\xAD",     // UTF-8 中
        "VENDOR\xFF",             // 0xFF (invalid UTF-8)
        "VENDOR\x80",             // high-bit alone
    };
    for (auto const *id : bad) {
        auto const r = st::library::validate_ptm_metadata_field("vendorId", id);
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("validate_ptm_metadata_field rejects control characters",
          "[library][ptm][validate]") {
    // Build std::strings with explicit length so embedded nulls survive
    // — a C-string literal would truncate at the first \0.
    std::string bad[] = {
        std::string{"VENDOR", 6} + '\0' + "cut",  // null mid-string
        "VENDOR\t tab",                            // tab
        "VENDOR\rCR",                              // carriage return
        "VENDOR\x1B" "escape",                     // ESC
        "VENDOR\x7F" "del",                        // DEL
    };
    for (auto const &id : bad) {
        auto const r = st::library::validate_ptm_metadata_field("vendorId", id);
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("validate_ptm_metadata_field caps length at 128 chars",
          "[library][ptm][validate]") {
    std::string ok(128, 'A');
    REQUIRE(st::library::validate_ptm_metadata_field("vendorId", ok).has_value());
    std::string over(129, 'A');
    auto const r = st::library::validate_ptm_metadata_field("vendorId", over);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("validate_ptm_metadata_field accepts empty input",
          "[library][ptm][validate]") {
    // Empty is structurally fine — the AP-side parser will just read
    // empty string into the variable. Not a security issue. Caller
    // (e.g., ptm export) should reject empty separately if business
    // logic requires it.
    auto const r = st::library::validate_ptm_metadata_field("vendorId", "");
    REQUIRE(r.has_value());
}

TEST_CASE("validate_ptm_metadata_field error message names the bad char + offset",
          "[library][ptm][validate]") {
    auto const r = st::library::validate_ptm_metadata_field("vendorId",
                                                              "ABC$bad");
    REQUIRE_FALSE(r.has_value());
    // Error message should help a human debug what's wrong.
    auto const msg = r.error().to_string();
    CHECK(msg.find("vendorId") != std::string::npos);
    CHECK(msg.find("0x24") != std::string::npos); // '$' is 0x24
    CHECK(msg.find("3") != std::string::npos);   // offset
}
