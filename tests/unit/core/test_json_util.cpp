// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Coverage for the hand-rolled JSON escaper shared by st::audit,
// st::diff, and st::ai. Pins the on-wire contract — any byte change
// here breaks audit.log + diff envelopes + Anthropic/OpenAI request
// bodies in lock-step.

#include "st/core/json_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("json_escape wraps plain ASCII in quotes", "[core][json][basic]") {
    std::string out;
    st::json_escape(out, "hello");
    REQUIRE(out == "\"hello\"");
}

TEST_CASE("json_escape escapes the required JSON specials",
          "[core][json][escape]") {
    std::string out;
    st::json_escape(out, "a\"b\\c\nd\re\tf");
    REQUIRE(out == "\"a\\\"b\\\\c\\nd\\re\\tf\"");
}

TEST_CASE("json_escape emits \\u00XX for control chars below 0x20",
          "[core][json][escape][control]") {
    std::string out;
    st::json_escape(out, std::string{"x\x01y\x1fz"});
    REQUIRE(out == "\"x\\u0001y\\u001Fz\"");
}

TEST_CASE("json_escape passes UTF-8 continuation bytes through verbatim",
          "[core][json][utf8]") {
    // The escaper is bytewise — it does NOT decode UTF-8, so multibyte
    // sequences (≥ 0x80) survive intact on the wire. Audit + AI wire
    // formats are UTF-8 end-to-end so this is the right behavior.
    std::string out;
    st::json_escape(out, std::string{"caf\xC3\xA9"}); // "café"
    REQUIRE(out == "\"caf\xC3\xA9\"");
}

TEST_CASE("json_escape on empty input emits the empty string",
          "[core][json][edge]") {
    std::string out;
    st::json_escape(out, "");
    REQUIRE(out == "\"\"");
}

TEST_CASE("json_escape appends -- does not overwrite prefix",
          "[core][json][append-semantics]") {
    std::string out{"prefix:"};
    st::json_escape(out, "x");
    REQUIRE(out == "prefix:\"x\"");
}
