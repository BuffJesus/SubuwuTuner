// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Unit tests for AsciiRelayBenchPower — exercise command emission,
// response parsing, and error propagation against a LoopbackByteChannel
// so no USB hardware is involved. These always run in CI; the
// hardware-bound variant under tests/hil/ uses the same class against
// a real serial port.
//
// What this file does NOT test:
//   * Real serial-port I/O — covered by tests/unit/transport/.
//   * pulse_off's actual wall-clock duration — pulse_minimum is
//     enforced as a floor but we don't validate timing here because
//     it'd make the unit suite slow + flaky. tests/hil exercises real
//     timing on a real board.

#include "../../_helpers/ascii_relay_bench_power.hpp"
#include "../../_helpers/bench_power.hpp"
#include "../../_helpers/loopback_byte_channel.hpp"

#include "st/core/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace bench = st::test::bench;
namespace tt = st::test::transport;

namespace {

// Helper: minimal board response with no payload, just the prompt
// trailer. Used after every "relay on"/"relay off" command.
constexpr std::string_view kAckOnly = "\r>";

// Helper: read response with the typical "echo + payload + prompt"
// shape Numato/SainSmart boards emit for "relay read".
std::string read_response(std::string_view sent_cmd, std::string_view payload) {
    std::string r;
    r.reserve(sent_cmd.size() + payload.size() + 6);
    r += sent_cmd;
    r += '\n';
    r += payload;
    r += '\r';
    r += '>';
    return r;
}

} // namespace

TEST_CASE("AsciiRelayBenchPower: set(MainPower, true) emits 'relay on 1\\r' and consumes the prompt",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(kAckOnly);

    auto const status = power.set(bench::Rail::MainPower, true);
    REQUIRE(status.has_value());
    REQUIRE(ch.drain_written_as_string() == "relay on 1\r");
    REQUIRE(ch.pending_read_count() == 0);
}

TEST_CASE("AsciiRelayBenchPower: set(BackupPower, false) emits 'relay off 2\\r'",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(kAckOnly);

    auto const status = power.set(bench::Rail::BackupPower, false);
    REQUIRE(status.has_value());
    REQUIRE(ch.drain_written_as_string() == "relay off 2\r");
}

TEST_CASE("AsciiRelayBenchPower: set(Ignition, true) emits 'relay on 3\\r'",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(kAckOnly);

    auto const status = power.set(bench::Rail::Ignition, true);
    REQUIRE(status.has_value());
    REQUIRE(ch.drain_written_as_string() == "relay on 3\r");
}

TEST_CASE("AsciiRelayBenchPower: custom channel mapping overrides the defaults",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayChannels const custom{.main_power = 7, .backup_power = 8, .ignition = 9};
    bench::AsciiRelayBenchPower power{ch, custom};

    ch.queue_read(kAckOnly);
    REQUIRE(power.set(bench::Rail::MainPower, true).has_value());
    REQUIRE(ch.drain_written_as_string() == "relay on 7\r");

    ch.queue_read(kAckOnly);
    REQUIRE(power.set(bench::Rail::Ignition, false).has_value());
    REQUIRE(ch.drain_written_as_string() == "relay off 9\r");
}

TEST_CASE("AsciiRelayBenchPower: get parses 'on' response as true",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(read_response("relay read 1\r", "on"));

    auto const r = power.get(bench::Rail::MainPower);
    REQUIRE(r.has_value());
    REQUIRE(*r == true);
    REQUIRE(ch.drain_written_as_string() == "relay read 1\r");
}

TEST_CASE("AsciiRelayBenchPower: get parses 'off' response as false",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(read_response("relay read 2\r", "off"));

    auto const r = power.get(bench::Rail::BackupPower);
    REQUIRE(r.has_value());
    REQUIRE(*r == false);
}

TEST_CASE("AsciiRelayBenchPower: get tolerates uppercase ON/OFF responses",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(read_response("relay read 1\r", "ON"));
    auto const r_on = power.get(bench::Rail::MainPower);
    REQUIRE(r_on.has_value());
    REQUIRE(*r_on == true);

    ch.queue_read(read_response("relay read 2\r", "OFF"));
    auto const r_off = power.get(bench::Rail::BackupPower);
    REQUIRE(r_off.has_value());
    REQUIRE(*r_off == false);
}

TEST_CASE("AsciiRelayBenchPower: get tolerates 1/0 numeric responses",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(read_response("relay read 1\r", "1"));
    auto const r_hi = power.get(bench::Rail::MainPower);
    REQUIRE(r_hi.has_value());
    REQUIRE(*r_hi == true);

    ch.queue_read(read_response("relay read 1\r", "0"));
    auto const r_lo = power.get(bench::Rail::MainPower);
    REQUIRE(r_lo.has_value());
    REQUIRE(*r_lo == false);
}

TEST_CASE("AsciiRelayBenchPower: get returns ProtocolError on unparseable response",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read(read_response("relay read 1\r", "maybe"));

    auto const r = power.get(bench::Rail::MainPower);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::ProtocolError);
}

TEST_CASE("AsciiRelayBenchPower: set propagates write_bytes failure as TransportUnavailable",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.inject_next_error(st::ErrorCode::TransportUnavailable, "USB unplugged");

    auto const status = power.set(bench::Rail::MainPower, true);
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::TransportUnavailable);
}

TEST_CASE("AsciiRelayBenchPower: set returns Timeout when no prompt arrives",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayTimings const fast{
        .response_timeout = std::chrono::milliseconds{1},
        .pulse_minimum = std::chrono::milliseconds{1},
    };
    bench::AsciiRelayBenchPower power{ch, {}, fast};

    // No queued response — read_bytes returns empty (timeout-style)
    // forever until our response_timeout deadline kicks in.

    auto const status = power.set(bench::Rail::MainPower, true);
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::TransportTimeout);
}

TEST_CASE("AsciiRelayBenchPower: pulse_off sends off then on with the right channel",
          "[bench][ascii_relay]") {
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayTimings const fast{
        .response_timeout = std::chrono::milliseconds{50},
        .pulse_minimum = std::chrono::milliseconds{1},
    };
    bench::AsciiRelayBenchPower power{ch, {}, fast};

    ch.queue_read(kAckOnly); // ack the off
    ch.queue_read(kAckOnly); // ack the on

    auto const status = power.pulse_off(bench::Rail::MainPower, std::chrono::milliseconds{1});
    REQUIRE(status.has_value());

    auto const written = ch.drain_written_as_string();
    REQUIRE(written == "relay off 1\rrelay on 1\r");
}

TEST_CASE("AsciiRelayBenchPower: response with multi-chunk arrival reassembles correctly",
          "[bench][ascii_relay]") {
    // Real serial ports often deliver one logical response across
    // multiple read_bytes calls (especially under load). We queue the
    // response in two halves to verify the accumulator handles that.
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayBenchPower power{ch};

    ch.queue_read("relay read 1\r\n");
    ch.queue_read("on\r>");

    auto const r = power.get(bench::Rail::MainPower);
    REQUIRE(r.has_value());
    REQUIRE(*r == true);
}

TEST_CASE("LoopbackByteChannel: read_bytes honors timeout when no data queued",
          "[bench][ascii_relay][loopback]") {
    // Regression guard: previously the loopback returned an empty vector
    // instantly when the queue was empty, ignoring the timeout. That
    // caused AsciiRelayBenchPower::read_until_prompt to burn CPU in its
    // outer loop until the consumer's own deadline elapsed. Verify the
    // loopback now actually waits for the timeout.
    tt::LoopbackByteChannel ch;
    auto const t_start = std::chrono::steady_clock::now();
    auto const r = ch.read_bytes(64, std::chrono::milliseconds{40});
    auto const elapsed = std::chrono::steady_clock::now() - t_start;

    REQUIRE(r.has_value());
    REQUIRE(r->empty());
    // 30 ms floor leaves room for sleep_for jitter on busy CI runners
    // (Windows scheduler ~16ms granularity worst case). We're not
    // asserting the exact timeout, just that it didn't return instantly.
    REQUIRE(elapsed >= std::chrono::milliseconds{30});
}

TEST_CASE("AsciiRelayBenchPower: read times out cleanly when board never responds",
          "[bench][ascii_relay]") {
    // End-to-end version of the above: nothing queued, set() should
    // succeed at write but fail at the read-prompt step with a
    // TransportTimeout, not spin forever. The total wall time should
    // approximate response_timeout — not be either instant or hung.
    tt::LoopbackByteChannel ch;
    bench::AsciiRelayTimings timings;
    timings.response_timeout = std::chrono::milliseconds{60};
    bench::AsciiRelayBenchPower power{ch, bench::AsciiRelayChannels{}, timings};

    auto const t_start = std::chrono::steady_clock::now();
    auto const status = power.set(bench::Rail::MainPower, true);
    auto const elapsed = std::chrono::steady_clock::now() - t_start;

    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == st::ErrorCode::TransportTimeout);
    // Sanity: bounded by the timeout (with generous headroom for
    // scheduler jitter). Pre-fix this would have spun on CPU for the
    // same duration — but at least it's bounded now, AND not wasting
    // a core.
    REQUIRE(elapsed >= std::chrono::milliseconds{40});
    REQUIRE(elapsed < std::chrono::milliseconds{500});
}
