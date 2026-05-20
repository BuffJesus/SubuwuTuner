// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace {

st::Result<int> divide(int num, int den) {
    if (den == 0) {
        return st::failure(st::ErrorCode::InvalidArgument, "division by zero");
    }
    return num / den;
}

st::Status validate_positive(int x) {
    if (x <= 0) {
        return st::failure(st::ErrorCode::OutOfRange, "x must be positive");
    }
    return st::ok();
}

} // namespace

TEST_CASE("Result<int> happy path holds a value", "[core][result]") {
    auto const r = divide(10, 2);
    REQUIRE(r.has_value());
    REQUIRE(*r == 5);
}

TEST_CASE("Result<int> error path holds an Error with code and message", "[core][result]") {
    auto const r = divide(10, 0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(r.error().message() == "division by zero");
}

TEST_CASE("Status models void-returning operations", "[core][result]") {
    REQUIRE(validate_positive(1).has_value());

    auto const r = validate_positive(-3);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::OutOfRange);
}

TEST_CASE("failure(code) builds an unexpected with default message", "[core][result]") {
    st::Result<std::string> const r = st::failure(st::ErrorCode::NotImplemented);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
    REQUIRE(r.error().message().empty());
}

TEST_CASE("failure preserves a moved Error", "[core][result]") {
    st::Error orig{st::ErrorCode::Cancelled, "user pressed Esc"};
    st::Result<int> const r = st::failure(std::move(orig));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::Cancelled);
    REQUIRE(r.error().message() == "user pressed Esc");
}

#ifdef __GNUC__
TEST_CASE("ST_TRY unwraps a value", "[core][result][st_try]") {
    auto compute = []() -> st::Result<int> {
        auto a = ST_TRY(divide(20, 4));    // 5
        auto b = ST_TRY(divide(a + 5, 2)); // 5
        return a + b;
    };
    auto const r = compute();
    REQUIRE(r.has_value());
    REQUIRE(*r == 10);
}

TEST_CASE("ST_TRY short-circuits on error", "[core][result][st_try]") {
    auto compute = []() -> st::Result<int> {
        auto a = ST_TRY(divide(10, 0)); // bails here
        return a + 1;                   // unreachable
    };
    auto const r = compute();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    REQUIRE(r.error().message() == "division by zero");
}
#endif
