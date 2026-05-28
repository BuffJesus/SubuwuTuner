// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "ascii_relay_bench_power.hpp"

#include "st/core/error.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace st::test::bench {

namespace {

// Prompt trailer the board emits after every command response. We
// look for "\r>" (CR then prompt) rather than "\n\r>" because some
// firmware variants reorder the LF/CR pair; "\r>" is the longest
// suffix that's stable across the families we target.
constexpr std::string_view kPromptTrailer = "\r>";

// Sleep helper that's a no-op in test builds when a real_sleep isn't
// desired. We DO actually sleep in pulse_off — the brick-recovery
// gate is timing-sensitive and "drop for 50 ms" needs to drop for
// ~50 ms, not 0.
void sleep_for(std::chrono::milliseconds d) {
    if (d.count() > 0) {
        std::this_thread::sleep_for(d);
    }
}

// Build "relay on N\r" / "relay off N\r" / "relay read N\r". The
// trailing CR is the line terminator the boards consume; some boards
// also accept CRLF but every variant we target accepts bare CR, so
// CR is the lowest-common-denominator choice.
std::string build_command(std::string_view verb, std::uint8_t channel_number) {
    std::string cmd;
    cmd.reserve(16);
    cmd += "relay ";
    cmd += verb;
    cmd += ' ';
    cmd += std::to_string(static_cast<unsigned>(channel_number));
    cmd += '\r';
    return cmd;
}

[[nodiscard]] st::Status write_command(st::transport::IByteChannel &ch, std::string const &cmd) {
    auto bytes_ptr = reinterpret_cast<std::uint8_t const *>(cmd.data());
    return ch.write_bytes(std::span<std::uint8_t const>{bytes_ptr, cmd.size()});
}

// Strip a leading echo of the sent command from the response body.
// Many firmwares echo each character typed; some don't. We tolerate
// either by detecting the echo prefix and removing it. The prompt
// has already been stripped before this is called.
std::string_view strip_echo(std::string_view body, std::string_view sent) {
    if (body.size() >= sent.size() && body.substr(0, sent.size()) == sent) {
        return body.substr(sent.size());
    }
    // Some firmwares echo without the trailing \r — try that too.
    if (!sent.empty() && sent.back() == '\r') {
        std::string_view sent_no_cr = sent.substr(0, sent.size() - 1);
        if (body.size() >= sent_no_cr.size() && body.substr(0, sent_no_cr.size()) == sent_no_cr) {
            return body.substr(sent_no_cr.size());
        }
    }
    return body;
}

// Trim ASCII whitespace from both ends of a view. Used on the
// post-echo response body to extract just "on" / "off".
std::string_view trim_ascii_ws(std::string_view s) {
    auto const not_ws = [](char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    };
    auto begin_it = std::find_if(s.begin(), s.end(), not_ws);
    auto end_it = std::find_if(s.rbegin(), s.rend(), not_ws).base();
    if (begin_it >= end_it) {
        return {};
    }
    return std::string_view{&*begin_it, static_cast<std::size_t>(end_it - begin_it)};
}

} // namespace

AsciiRelayBenchPower::AsciiRelayBenchPower(st::transport::IByteChannel &channel,
                                           AsciiRelayChannels channels,
                                           AsciiRelayTimings timings) noexcept
    : channel_{channel}, channels_{channels}, timings_{timings} {}

std::uint8_t AsciiRelayBenchPower::channel_for(Rail rail) const noexcept {
    switch (rail) {
    case Rail::MainPower:
        return channels_.main_power;
    case Rail::BackupPower:
        return channels_.backup_power;
    case Rail::Ignition:
        return channels_.ignition;
    }
    return 0; // unreachable; switch is exhaustive on the enum
}

st::Status AsciiRelayBenchPower::send_set_command(std::uint8_t channel_number, bool on) {
    auto const cmd = build_command(on ? "on" : "off", channel_number);
    if (auto s = write_command(channel_, cmd); !s.has_value()) {
        return s;
    }
    auto response = read_until_prompt();
    if (!response.has_value()) {
        return st::failure(response.error());
    }
    return st::ok();
}

st::Result<bool> AsciiRelayBenchPower::send_read_command(std::uint8_t channel_number) {
    auto const cmd = build_command("read", channel_number);
    if (auto s = write_command(channel_, cmd); !s.has_value()) {
        return st::failure(s.error());
    }
    auto response = read_until_prompt();
    if (!response.has_value()) {
        return st::failure(response.error());
    }
    std::string_view body = strip_echo(*response, cmd);
    body = trim_ascii_ws(body);
    if (body == "on" || body == "ON" || body == "1") {
        return true;
    }
    if (body == "off" || body == "OFF" || body == "0") {
        return false;
    }
    std::string msg;
    msg.reserve(64 + body.size());
    msg += "ascii_relay: unparseable read response: '";
    msg.append(body);
    msg += "'";
    return st::failure(st::ErrorCode::ProtocolError, std::move(msg));
}

st::Result<std::string> AsciiRelayBenchPower::read_until_prompt() {
    using clock = std::chrono::steady_clock;
    auto const deadline = clock::now() + timings_.response_timeout;
    std::string acc;
    acc.reserve(64);

    // Per-read chunk size: 64 bytes is more than any single board
    // response we expect (longest is "relay read 16\r\non\r\n>" at
    // ~22 bytes). We use this as a ceiling, not a target — read_bytes
    // returns what's available up to this much.
    constexpr std::size_t kChunkBytes = 64;

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - clock::now());
        if (remaining.count() <= 0) {
            std::string msg = "ascii_relay: timed out waiting for prompt; got: '";
            msg += acc;
            msg += "'";
            return st::failure(st::ErrorCode::TransportTimeout, std::move(msg));
        }
        auto chunk = channel_.read_bytes(kChunkBytes, remaining);
        if (!chunk.has_value()) {
            return st::failure(chunk.error());
        }
        if (chunk->empty()) {
            // Channel timeout (not an error per IByteChannel contract).
            // Loop and re-check our own deadline; if it's elapsed the
            // next iteration's `remaining <= 0` branch returns Timeout.
            continue;
        }
        acc.append(reinterpret_cast<char const *>(chunk->data()), chunk->size());
        if (auto pos = acc.find(kPromptTrailer); pos != std::string::npos) {
            // Return the body BEFORE the prompt. Anything after the
            // prompt in the same chunk is left unread — boards don't
            // emit unsolicited output between commands, so this
            // shouldn't happen in practice, but if it does we'd
            // rather discard it than confuse the next command.
            acc.resize(pos);
            return acc;
        }
    }
}

st::Status AsciiRelayBenchPower::set(Rail rail, bool on) {
    return send_set_command(channel_for(rail), on);
}

st::Result<bool> AsciiRelayBenchPower::get(Rail rail) {
    return send_read_command(channel_for(rail));
}

st::Status AsciiRelayBenchPower::pulse_off(Rail rail, std::chrono::milliseconds duration) {
    auto const ch = channel_for(rail);
    auto const sleep = std::max(duration, timings_.pulse_minimum);
    if (auto s = send_set_command(ch, false); !s.has_value()) {
        return s;
    }
    sleep_for(sleep);
    return send_set_command(ch, true);
}

} // namespace st::test::bench
