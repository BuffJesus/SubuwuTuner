// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/factory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tp = st::transport;

// ---- kind_name / parse_kind round-trip --------------------------

TEST_CASE("factory::kind_name maps each defined Kind to its CLI string", "[transport][factory]") {
    REQUIRE(std::string_view{tp::kind_name(tp::Kind::J2534)} == "j2534");
    REQUIRE(std::string_view{tp::kind_name(tp::Kind::Obdx)} == "obdx");
    REQUIRE(std::string_view{tp::kind_name(tp::Kind::Native)} == "native");
}

TEST_CASE("factory::parse_kind accepts every canonical name", "[transport][factory]") {
    REQUIRE(tp::parse_kind("j2534") == tp::Kind::J2534);
    REQUIRE(tp::parse_kind("obdx") == tp::Kind::Obdx);
    REQUIRE(tp::parse_kind("native") == tp::Kind::Native);
}

TEST_CASE("factory::parse_kind is case-sensitive (lowercase only)", "[transport][factory]") {
    REQUIRE_FALSE(tp::parse_kind("J2534").has_value());
    REQUIRE_FALSE(tp::parse_kind("OBDX").has_value());
    REQUIRE_FALSE(tp::parse_kind("Native").has_value());
}

TEST_CASE("factory::parse_kind rejects unknown + empty strings", "[transport][factory]") {
    REQUIRE_FALSE(tp::parse_kind("").has_value());
    REQUIRE_FALSE(tp::parse_kind("tactrix").has_value());
    REQUIRE_FALSE(tp::parse_kind("elm").has_value());
    REQUIRE_FALSE(tp::parse_kind("foo").has_value());
}

TEST_CASE("factory: every Kind round-trips through name → parse", "[transport][factory]") {
    for (auto k : {tp::Kind::J2534, tp::Kind::Obdx, tp::Kind::Native}) {
        auto const name = tp::kind_name(k);
        auto const parsed = tp::parse_kind(name);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == k);
    }
}

// ---- open_transport validation paths ----------------------------

TEST_CASE("open_transport(j2534, empty dll_path) → InvalidArgument with hint",
          "[transport][factory]") {
    tp::TransportSpec spec{tp::Kind::J2534, /*dll_path=*/"", /*device_path=*/""};
    auto r = tp::open_transport(spec);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    auto const m = r.error().message();
    REQUIRE(m.find("--dll") != std::string::npos);
    REQUIRE(m.find("op20pt32.dll") != std::string::npos);
}

TEST_CASE("open_transport(obdx, empty device_path) → InvalidArgument with hint",
          "[transport][factory]") {
    tp::TransportSpec spec{tp::Kind::Obdx, "", ""};
    auto r = tp::open_transport(spec);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
    auto const m = r.error().message();
    REQUIRE(m.find("--device") != std::string::npos);
    REQUIRE(m.find("COM") != std::string::npos); // example path mentioned
}

TEST_CASE("open_transport(native, empty device_path) → InvalidArgument", "[transport][factory]") {
    tp::TransportSpec spec{tp::Kind::Native, "", ""};
    auto r = tp::open_transport(spec);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

// ---- open_transport NotImplemented paths ------------------------
//
// Every concrete kind currently returns NotImplemented because the
// platform layer (LoadLibraryA / libusb / native CDC) isn't wired
// yet. The CLI flag is meant to be useful TODAY even so — clean
// errors that name the missing platform piece let the user verify
// the dispatch works before hardware arrives.

TEST_CASE("open_transport(j2534, valid path) → NotImplemented "
          "(platform dynload not yet wired)",
          "[transport][factory]") {
    tp::TransportSpec spec{tp::Kind::J2534, "op20pt32.dll", ""};
    auto r = tp::open_transport(spec);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::NotImplemented);
    // The error message should name what's missing.
    auto const m = r.error().message();
    REQUIRE(m.find("dynamic-load") != std::string::npos);
}

TEST_CASE("open_transport(obdx, nonexistent path) → FileNotFound "
          "(platform USB CDC wired; this device doesn't exist)",
          "[transport][factory]") {
    tp::TransportSpec spec{tp::Kind::Obdx, "", "COM_NONEXISTENT_TEST"};
    auto r = tp::open_transport(spec);
    REQUIRE_FALSE(r.has_value());
    // On Windows, opening a missing COM port → FileNotFound. On other
    // platforms (where the serial layer is NotImplemented) → NotImplemented.
    auto const code = r.error().code();
    REQUIRE((code == st::ErrorCode::FileNotFound || code == st::ErrorCode::NotImplemented));
    auto const m = r.error().message();
    REQUIRE(m.find("COM_NONEXISTENT_TEST") != std::string::npos);
}

TEST_CASE("open_transport(native, nonexistent path) → FileNotFound or "
          "NotImplemented depending on platform",
          "[transport][factory]") {
    tp::TransportSpec spec{tp::Kind::Native, "", "COM_NONEXISTENT_NATIVE"};
    auto r = tp::open_transport(spec);
    REQUIRE_FALSE(r.has_value());
    auto const code = r.error().code();
    REQUIRE((code == st::ErrorCode::FileNotFound || code == st::ErrorCode::NotImplemented));
    auto const m = r.error().message();
    REQUIRE(m.find("COM_NONEXISTENT_NATIVE") != std::string::npos);
}
