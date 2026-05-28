// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// HIL coverage for AsciiRelayBenchPower against a real USB relay
// board on a real serial port. Gated on ST_BUILD_HIL_TESTS=ON.
//
// Hardware required: any Numato Lab / SainSmart 16-ch / KMtronic USB
// relay enumerated as a virtual COM port. The board's COM port + the
// channel mapping come from environment variables so this test can
// run on whatever rig is at hand:
//
//   ST_HIL_RELAY_PORT     — required. e.g. "COM4" on Windows,
//                           "/dev/ttyUSB0" on Linux.
//   ST_HIL_RELAY_BAUD     — optional, default 19200 (the Numato +
//                           SainSmart 16-ch factory default).
//   ST_HIL_RELAY_CHANNEL  — optional, default 1. The channel number
//                           the test will toggle. Pick one that's
//                           NOT wired to anything important — the
//                           test clicks it on and off and you'll
//                           hear it audibly.
//
// If ST_HIL_RELAY_PORT is unset, every test in this file SKIPs.
// That lets a partially-wired bench (relay not yet connected) still
// run the rest of the [hil] suite.

#include "../_helpers/ascii_relay_bench_power.hpp"
#include "../_helpers/bench_power.hpp"

#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/serial_byte_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace bench = st::test::bench;
namespace stxp = st::transport;

namespace {

[[nodiscard]] std::string get_env(char const *name) {
    char const *v = std::getenv(name);
    return v ? std::string{v} : std::string{};
}

[[nodiscard]] std::uint32_t get_env_u32(char const *name, std::uint32_t fallback) {
    auto const s = get_env(name);
    if (s.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(s));
    } catch (...) {
        return fallback;
    }
}

// Open the configured serial port. Returns nullptr if the env vars
// say "no relay attached"; in that case the caller SKIPs.
std::unique_ptr<stxp::IByteChannel> open_configured_relay() {
    auto const port = get_env("ST_HIL_RELAY_PORT");
    if (port.empty()) {
        return nullptr;
    }
    stxp::SerialChannelConfig cfg{
        .device_path = port,
        .baud_rate = get_env_u32("ST_HIL_RELAY_BAUD", 19200U),
        .read_timeout_ms = 1000U,
        .write_timeout_ms = 1000U,
    };
    auto chan = stxp::make_serial_byte_channel(cfg);
    if (!chan.has_value()) {
        return nullptr;
    }
    return std::move(*chan);
}

} // namespace

TEST_CASE("HIL: AsciiRelayBenchPower toggles the configured relay channel",
          "[hil][bench][ascii_relay]") {
    auto channel = open_configured_relay();
    if (!channel) {
        SKIP("Set ST_HIL_RELAY_PORT (e.g. COM4) to run this test");
    }
    auto const ch = static_cast<std::uint8_t>(get_env_u32("ST_HIL_RELAY_CHANNEL", 1U));

    // Map all three rails onto the single test channel so the test
    // doesn't depend on multi-channel wiring. set(MainPower, …) below
    // toggles whatever channel the user told us is safe.
    bench::AsciiRelayChannels const channels{
        .main_power = ch,
        .backup_power = ch,
        .ignition = ch,
    };
    bench::AsciiRelayBenchPower power{*channel, channels};

    // Click on
    REQUIRE(power.set(bench::Rail::MainPower, true).has_value());
    // Read-back should agree
    auto const after_on = power.get(bench::Rail::MainPower);
    REQUIRE(after_on.has_value());
    CHECK(*after_on == true);

    // Click off
    REQUIRE(power.set(bench::Rail::MainPower, false).has_value());
    auto const after_off = power.get(bench::Rail::MainPower);
    REQUIRE(after_off.has_value());
    CHECK(*after_off == false);
}

TEST_CASE("HIL: AsciiRelayBenchPower pulse_off cycles the configured channel",
          "[hil][bench][ascii_relay]") {
    auto channel = open_configured_relay();
    if (!channel) {
        SKIP("Set ST_HIL_RELAY_PORT (e.g. COM4) to run this test");
    }
    auto const ch = static_cast<std::uint8_t>(get_env_u32("ST_HIL_RELAY_CHANNEL", 1U));

    bench::AsciiRelayChannels const channels{
        .main_power = ch,
        .backup_power = ch,
        .ignition = ch,
    };
    bench::AsciiRelayBenchPower power{*channel, channels};

    // Pre-condition: relay starts on.
    REQUIRE(power.set(bench::Rail::MainPower, true).has_value());

    // 100 ms pulse — long enough to hear the click cycle audibly and
    // for any board-side response timing to be uneventful.
    auto const t0 = std::chrono::steady_clock::now();
    REQUIRE(power.pulse_off(bench::Rail::MainPower, std::chrono::milliseconds{100}).has_value());
    auto const elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    // Wall-clock check: must be AT LEAST the pulse duration. Upper
    // bound is loose because the two command round-trips add ~10–30 ms
    // on top depending on baud and board firmware.
    CHECK(elapsed >= std::chrono::milliseconds{100});
    CHECK(elapsed < std::chrono::milliseconds{2000});

    // Post-condition: rail is back on.
    auto const after = power.get(bench::Rail::MainPower);
    REQUIRE(after.has_value());
    CHECK(*after == true);
}
