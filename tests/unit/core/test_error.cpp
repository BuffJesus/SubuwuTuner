// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_CASE("Error default-constructs with Unknown code and empty message", "[core][error]") {
    st::Error const err;
    REQUIRE(err.code() == st::ErrorCode::Unknown);
    REQUIRE(err.message().empty());
}

TEST_CASE("Error stores explicit code", "[core][error]") {
    st::Error const err{st::ErrorCode::FileNotFound};
    REQUIRE(err.code() == st::ErrorCode::FileNotFound);
    REQUIRE(err.message().empty());
}

TEST_CASE("Error stores code and message", "[core][error]") {
    st::Error const err{st::ErrorCode::BadChecksum, "expected 0xCAFEBABE"};
    REQUIRE(err.code() == st::ErrorCode::BadChecksum);
    REQUIRE(err.message() == "expected 0xCAFEBABE");
}

TEST_CASE("to_string covers known error codes", "[core][error]") {
    using st::ErrorCode;
    REQUIRE(st::to_string(ErrorCode::Ok) == "ok");
    REQUIRE(st::to_string(ErrorCode::InvalidArgument) == "invalid argument");
    REQUIRE(st::to_string(ErrorCode::FileNotFound) == "file not found");
    REQUIRE(st::to_string(ErrorCode::BadChecksum) == "checksum mismatch");
    REQUIRE(st::to_string(ErrorCode::SeedKeyFailed) == "seed/key authentication failed");
}

TEST_CASE("to_string handles unknown codes gracefully", "[core][error]") {
    auto const bogus = static_cast<st::ErrorCode>(0xFFFF);
    REQUIRE(st::to_string(bogus) == "unrecognised error code");
}

TEST_CASE("Error::to_string includes message when present", "[core][error]") {
    st::Error const with_msg{st::ErrorCode::ParseError, "line 42: unexpected '}'"};
    REQUIRE(with_msg.to_string() == "parse error: line 42: unexpected '}'");

    st::Error const without_msg{st::ErrorCode::Cancelled};
    REQUIRE(without_msg.to_string() == "cancelled");
}

TEST_CASE("Error equality compares code and message", "[core][error]") {
    st::Error const a{st::ErrorCode::IoFailure, "disk full"};
    st::Error const b{st::ErrorCode::IoFailure, "disk full"};
    st::Error const c{st::ErrorCode::IoFailure, "read past end"};
    st::Error const d{st::ErrorCode::FileNotFound, "disk full"};

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE_FALSE(a == d);
}
