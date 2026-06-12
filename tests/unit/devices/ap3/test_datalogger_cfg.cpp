// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tests for the `/presets/*.cfg` datalogger preset format —
// st::devices::ap3::decode_datalogger_cfg / encode_datalogger_cfg.
// Format per RE4 (findings/re-2026-06-12-pm/).

#include "st/devices/ap3/datalogger_cfg.hpp"
#include "st/core/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("datalogger_cfg: decode canonical CRLF preset",
          "[devices][ap3][datalogger_cfg]") {
    // AP-canonical wire format: all three keys, CRLF line endings.
    std::string const cfg =
        "preset_name=Boost & Knock\r\n"
        "preset_description=watch the wastegate\r\n"
        "log_mon_list=rpm,iam,manifold_pressure,knock_sum\r\n";
    auto r = st::devices::ap3::decode_datalogger_cfg(cfg);
    REQUIRE(r.has_value());
    CHECK(r->name == "Boost & Knock");
    CHECK(r->description == "watch the wastegate");
    REQUIRE(r->log_mon_list.size() == 4);
    CHECK(r->log_mon_list[0] == "rpm");
    CHECK(r->log_mon_list[1] == "iam");
    CHECK(r->log_mon_list[2] == "manifold_pressure");
    CHECK(r->log_mon_list[3] == "knock_sum");
}

TEST_CASE("datalogger_cfg: decode tolerates bare LF + leading/trailing whitespace",
          "[devices][ap3][datalogger_cfg]") {
    // A user editing the file on a POSIX host may drop the \r.
    // Whitespace around '=' is trimmed; surrounding blank lines are
    // skipped.
    std::string const cfg =
        "\n"
        "preset_name = Quiet Mode\n"
        "  preset_description=  daily driver  \n"
        "log_mon_list = ect , iat\n";
    auto r = st::devices::ap3::decode_datalogger_cfg(cfg);
    REQUIRE(r.has_value());
    CHECK(r->name == "Quiet Mode");
    CHECK(r->description == "daily driver");
    REQUIRE(r->log_mon_list.size() == 2);
    CHECK(r->log_mon_list[0] == "ect");
    CHECK(r->log_mon_list[1] == "iat");
}

TEST_CASE("datalogger_cfg: decode rejects missing preset_name",
          "[devices][ap3][datalogger_cfg]") {
    std::string const cfg =
        "preset_description=orphan description\r\n"
        "log_mon_list=rpm\r\n";
    auto r = st::devices::ap3::decode_datalogger_cfg(cfg);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ParseError);
}

TEST_CASE("datalogger_cfg: decode tolerates unknown keys (forward-compat)",
          "[devices][ap3][datalogger_cfg]") {
    std::string const cfg =
        "preset_name=X\r\n"
        "future_feature_key=hello\r\n"
        "preset_description=Y\r\n"
        "another_unknown=1234\r\n"
        "log_mon_list=a\r\n";
    auto r = st::devices::ap3::decode_datalogger_cfg(cfg);
    REQUIRE(r.has_value());
    CHECK(r->name == "X");
    CHECK(r->description == "Y");
    REQUIRE(r->log_mon_list.size() == 1);
    CHECK(r->log_mon_list[0] == "a");
}

TEST_CASE("datalogger_cfg: decode handles empty log_mon_list",
          "[devices][ap3][datalogger_cfg]") {
    std::string const cfg =
        "preset_name=Empty\r\n"
        "preset_description=\r\n"
        "log_mon_list=\r\n";
    auto r = st::devices::ap3::decode_datalogger_cfg(cfg);
    REQUIRE(r.has_value());
    CHECK(r->description.empty());
    CHECK(r->log_mon_list.empty());
}

TEST_CASE("datalogger_cfg: encode emits CRLF + AP-canonical key order",
          "[devices][ap3][datalogger_cfg]") {
    st::devices::ap3::DataloggerPreset p;
    p.name = "WideOpen";
    p.description = "track day cluster";
    p.log_mon_list = {"rpm", "boost", "afr"};
    auto const text = st::devices::ap3::encode_datalogger_cfg(p);
    std::string const expected =
        "preset_name=WideOpen\r\n"
        "preset_description=track day cluster\r\n"
        "log_mon_list=rpm,boost,afr\r\n";
    CHECK(text == expected);
}

TEST_CASE("datalogger_cfg: encode -> decode round-trip preserves fields",
          "[devices][ap3][datalogger_cfg]") {
    st::devices::ap3::DataloggerPreset src;
    src.name = "Cold Start";
    src.description = "fuel trims + ECT for the first 60 s";
    src.log_mon_list = {"ect", "fuel_trim_short", "fuel_trim_long", "rpm",
                        "throttle", "iam"};
    auto const text = st::devices::ap3::encode_datalogger_cfg(src);
    auto round = st::devices::ap3::decode_datalogger_cfg(text);
    REQUIRE(round.has_value());
    CHECK(round->name == src.name);
    CHECK(round->description == src.description);
    REQUIRE(round->log_mon_list.size() == src.log_mon_list.size());
    for (std::size_t i = 0; i < src.log_mon_list.size(); ++i) {
        CHECK(round->log_mon_list[i] == src.log_mon_list[i]);
    }
}
