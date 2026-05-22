// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/j2534_discovery.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace j2534 = st::transport::j2534;

// ---- registry_view_name -----------------------------------------

TEST_CASE("j2534::registry_view_name: stable strings for each view",
          "[transport][j2534_discovery]") {
    REQUIRE(std::string_view{j2534::registry_view_name(j2534::RegistryView::Native64)} ==
            "native64");
    REQUIRE(std::string_view{j2534::registry_view_name(j2534::RegistryView::Wow6432)} == "wow6432");
}

// ---- format_protocols_supported ---------------------------------

TEST_CASE("j2534::format_protocols_supported: zero mask -> '(none)'",
          "[transport][j2534_discovery]") {
    REQUIRE(j2534::format_protocols_supported(0) == "(none)");
}

TEST_CASE("j2534::format_protocols_supported: single known bit", "[transport][j2534_discovery]") {
    // J1850VPW is bit 0 per spec (ProtocolId 0x01 -> 1 << 0).
    REQUIRE(j2534::format_protocols_supported(1U << 0U) == "J1850VPW");
    REQUIRE(j2534::format_protocols_supported(1U << 1U) == "J1850PWM");
    REQUIRE(j2534::format_protocols_supported(1U << 2U) == "ISO9141");
    REQUIRE(j2534::format_protocols_supported(1U << 3U) == "ISO14230");
    REQUIRE(j2534::format_protocols_supported(1U << 4U) == "CAN");
    REQUIRE(j2534::format_protocols_supported(1U << 5U) == "ISO15765");
}

TEST_CASE("j2534::format_protocols_supported: ISO9141 + ISO15765 (the "
          "Subaru-only two protocols)",
          "[transport][j2534_discovery]") {
    REQUIRE(j2534::format_protocols_supported((1U << 2U) | (1U << 5U)) == "ISO9141, ISO15765");
}

TEST_CASE("j2534::format_protocols_supported: all six known bits set",
          "[transport][j2534_discovery]") {
    REQUIRE(j2534::format_protocols_supported(0x3FU) ==
            "J1850VPW, J1850PWM, ISO9141, ISO14230, CAN, ISO15765");
}

TEST_CASE("j2534::format_protocols_supported: unknown high bit -> "
          "hex annotation",
          "[transport][j2534_discovery]") {
    REQUIRE(j2534::format_protocols_supported(0x80000000U) == "(unknown: 0x80000000)");
}

TEST_CASE("j2534::format_protocols_supported: mix of known + unknown",
          "[transport][j2534_discovery]") {
    // CAN (bit 4) + a future / unspecified protocol at bit 16.
    std::uint32_t const mask = (1U << 4U) | (1U << 16U);
    REQUIRE(j2534::format_protocols_supported(mask) == "CAN, (unknown: 0x00010000)");
}

// ---- discover_adapters (smoke) ----------------------------------

TEST_CASE("j2534::discover_adapters: runs without crashing + returns a vector "
          "(content host-dependent)",
          "[transport][j2534_discovery]") {
    // The contents depend on what J2534 v04.04 vendor DLLs are
    // registered on the host's HKLM\Software\PassThruSupport.04.04
    // keys. On non-Windows: always empty. On Windows with no
    // adapters installed: empty. On Windows with adapters: one
    // or more entries. We test only the bits that are
    // host-independent.
    auto const adapters = j2534::discover_adapters();
    // No crash + valid vector. The content sanity check below
    // runs on whatever the host happens to have.
    for (auto const &a : adapters) {
        // Subkey + name are always non-empty (impl falls back
        // name -> subkey).
        REQUIRE_FALSE(a.subkey.empty());
        REQUIRE_FALSE(a.name.empty());
        // function_library is enforced non-empty by walk_view
        // (entries without a DLL path are skipped — a vendor
        // installer that registered a skeleton without filling in
        // FunctionLibrary shouldn't surface as a usable adapter).
        REQUIRE_FALSE(a.function_library.empty());
        // View must be one of the two defined values.
        REQUIRE(
            (a.view == j2534::RegistryView::Native64 || a.view == j2534::RegistryView::Wow6432));
    }
}

#ifndef _WIN32
TEST_CASE("j2534::discover_adapters: non-Windows host -> empty list",
          "[transport][j2534_discovery]") {
    REQUIRE(j2534::discover_adapters().empty());
}
#endif
