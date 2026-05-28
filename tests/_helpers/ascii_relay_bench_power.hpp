// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::test::bench::AsciiRelayBenchPower — IBenchPower concrete for USB
// relay boards that speak the conventional ASCII command set:
//
//   send: "relay on N\r"   →  board energises relay N, replies "\n\r>"
//   send: "relay off N\r"  →  board de-energises relay N, replies "\n\r>"
//   send: "relay read N\r" →  board replies "\non\n\r>" or "\noff\n\r>"
//
// Compatible boards (verified against manufacturer docs — no GPL
// source contamination, see CLAUDE.md "House style"):
//   * Numato Lab USB relay modules (4 / 8 / 16 channel variants)
//   * SainSmart 16-channel 9–36V USB relay (Amazon.ca B0793MZH2B)
//   * KMtronic USB 4/8 relay boards
//   * Most "smart" MCU-fronted USB relays that enumerate as a virtual
//     COM port
//
// NOT supported here:
//   * SainSmart 4-channel automation relay (B009A5246E) — FTDI bit-bang
//     mode via libftdi, not a virtual COM port. A separate
//     Ft245BitbangBenchPower concrete will handle that board if/when
//     we ship support; the IByteChannel abstraction this class uses
//     can't reach the FT245RL's parallel bit-bang interface.
//
// Wiring vs. docs/28 Phase 6 (bench-rig brick-recovery loop):
//   R1 → MainPower    (B134-12 + B134-24, switched 12 V)
//   R2 → BackupPower  (B134-23, always-on 12 V)
//   R3 → Ignition     (B134-32, ignition switch)
// AsciiRelayChannels maps each Rail to a relay number on the board.
// Defaults are 1/2/3 — the obvious mapping that matches how the
// docs/28 wiring diagrams will label the harness.
//
// Why this lives under tests/_helpers/ rather than tests/hil/:
//   The class itself is hardware-agnostic — it takes an IByteChannel
//   reference. Unit tests inject a LoopbackByteChannel so protocol
//   parsing and command emission are validated without any USB device
//   plugged in. Hardware-bound tests under tests/hil/ open a real
//   serial port (via make_serial_byte_channel) and pass that channel
//   to this class. The gate for needing real hardware is the call
//   site, not this class.
//
// Concurrency: not thread-safe. Each operation is a request/response
// round-trip; interleaving from multiple threads would scramble the
// protocol. The brick-recovery fixture drives this from one thread.

#ifndef ST_TEST_ASCII_RELAY_BENCH_POWER_HPP
#define ST_TEST_ASCII_RELAY_BENCH_POWER_HPP

#include "bench_power.hpp"

#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace st::test::bench {

// Per-rail relay channel mapping on the board. Numbering follows the
// board's own convention (Numato/SainSmart 16-ch use 0-based, but
// nearly all newer firmware accepts both 0- and 1-based; we emit
// 1-based by default — most human-facing docs are 1-based and that
// keeps the wiring diagrams readable). Override by constructing with
// a custom AsciiRelayChannels if your board uses 0-based.
struct AsciiRelayChannels {
    std::uint8_t main_power{1};
    std::uint8_t backup_power{2};
    std::uint8_t ignition{3};
};

// Protocol timing knobs. response_timeout is the per-command wait for
// the board's prompt trailer; pulse_minimum is the floor for
// pulse_off durations — a zero or negative duration would otherwise
// race the relay's mechanical settling time and produce an
// indistinguishable-from-no-pull cycle, which is the worst possible
// failure mode for a brick-recovery test.
struct AsciiRelayTimings {
    std::chrono::milliseconds response_timeout{500};
    std::chrono::milliseconds pulse_minimum{20};
};

class AsciiRelayBenchPower final : public IBenchPower {
public:
    // Construct over a borrowed byte channel and channel mapping.
    // Caller owns the channel; typical usage holds the unique_ptr
    // from make_serial_byte_channel and constructs this against the
    // dereferenced pointer.
    AsciiRelayBenchPower(st::transport::IByteChannel &channel,
                         AsciiRelayChannels channels = {},
                         AsciiRelayTimings timings = {}) noexcept;

    [[nodiscard]] st::Status set(Rail rail, bool on) override;
    [[nodiscard]] st::Result<bool> get(Rail rail) override;
    [[nodiscard]] st::Status pulse_off(Rail rail, std::chrono::milliseconds duration) override;

    [[nodiscard]] AsciiRelayChannels const &channels() const noexcept {
        return channels_;
    }
    [[nodiscard]] AsciiRelayTimings const &timings() const noexcept {
        return timings_;
    }

private:
    st::transport::IByteChannel &channel_;
    AsciiRelayChannels channels_;
    AsciiRelayTimings timings_;

    [[nodiscard]] std::uint8_t channel_for(Rail rail) const noexcept;

    // Send "relay <verb> <n>\r" and confirm the board echoes the
    // prompt trailer. Returns ok on success, Timeout if the prompt
    // doesn't arrive within timings_.response_timeout, or
    // TransportUnavailable on channel I/O failure.
    [[nodiscard]] st::Status send_set_command(std::uint8_t channel_number, bool on);

    // Send "relay read <n>\r" and parse the response body for "on"
    // or "off". Returns InvalidArgument if the body is neither.
    [[nodiscard]] st::Result<bool> send_read_command(std::uint8_t channel_number);

    // Read response bytes until the board's prompt trailer ("\r>") is
    // observed or response_timeout elapses. Returns the response body
    // bytes BEFORE the trailer (echoed command + any payload).
    [[nodiscard]] st::Result<std::string> read_until_prompt();
};

} // namespace st::test::bench

#endif // ST_TEST_ASCII_RELAY_BENCH_POWER_HPP
