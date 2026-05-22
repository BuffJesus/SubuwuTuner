// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/obdx_transport.hpp"

#include "st/core/error.hpp"
#include "st/transport/obdx_dvi.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::transport::obdx {

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// Read exactly `n` bytes from the channel, accumulating across
// multiple read_bytes calls until `n` bytes are collected or the
// deadline passes. The byte-channel contract says read_bytes
// returns "what's available, up to max"; we re-poll until full.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_exactly(IDeviceChannel &chan, std::size_t n,
                                                             milliseconds timeout) {
    std::vector<std::uint8_t> out;
    out.reserve(n);
    auto const deadline = steady_clock::now() + timeout;
    while (out.size() < n) {
        auto const remaining_ms =
            std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining_ms.count() <= 0) {
            return failure(ErrorCode::TransportTimeout,
                           "obdx::read_exactly: deadline expired with " +
                               std::to_string(out.size()) + "/" + std::to_string(n) +
                               " bytes accumulated");
        }
        auto chunk = chan.read_bytes(n - out.size(), remaining_ms);
        if (!chunk.has_value())
            return failure(chunk.error());
        if (!chunk->empty()) {
            out.insert(out.end(), chunk->begin(), chunk->end());
        }
        // Empty chunk = timeout-shaped "no data this poll." Loop and
        // re-check the deadline. Real USB drivers often return early
        // with zero bytes on a short timeout — we shouldn't treat
        // that as fatal until our outer deadline runs out.
    }
    return out;
}

// Read bytes until the ELM prompt character ('>') appears or the
// deadline passes. Returns the response content (without the
// trailing '>'). ELM responses are CR-terminated ASCII; the prompt
// signals the adapter is ready for the next command.
[[nodiscard]] Result<std::string> read_until_prompt(IDeviceChannel &chan, milliseconds timeout) {
    std::string out;
    auto const deadline = steady_clock::now() + timeout;
    while (true) {
        auto const remaining_ms =
            std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining_ms.count() <= 0) {
            return failure(ErrorCode::TransportTimeout, "obdx::read_until_prompt: no '>' before "
                                                        "deadline (got " +
                                                            std::to_string(out.size()) + " bytes)");
        }
        auto chunk = chan.read_bytes(64, remaining_ms);
        if (!chunk.has_value())
            return failure(chunk.error());
        for (auto b : *chunk) {
            if (b == '>') {
                return out;
            }
            out.push_back(static_cast<char>(b));
        }
    }
}

// ELM-mode command-response round trip. Writes the command with a
// trailing CR (ELM protocol convention) and reads everything up to
// the next '>' prompt. The returned string contains the adapter's
// raw response (typically a few lines of ASCII echoed back from the
// device).
[[nodiscard]] Result<std::string> elm_exchange(IDeviceChannel &chan, std::string_view cmd,
                                               milliseconds timeout) {
    std::vector<std::uint8_t> tx;
    tx.reserve(cmd.size() + 1);
    for (char c : cmd)
        tx.push_back(static_cast<std::uint8_t>(c));
    tx.push_back('\r');
    if (auto s = chan.write_bytes(tx); !s.has_value()) {
        return failure(s.error());
    }
    return read_until_prompt(chan, timeout);
}

// Read one complete DVI frame from the stream, then hand the bytes
// to obdx::dvi::decode_frame. Strategy: read 2 bytes (opcode +
// 1-byte LEN), inspect the opcode to decide whether a 2-byte LEN
// applies, read the rest of the LEN field if so, then read
// declared_len payload bytes + 1 CRC byte. Assembled buffer goes
// to the codec for CRC + structural validation.
[[nodiscard]] Result<dvi::DecodedFrame> read_dvi_frame(IDeviceChannel &chan, milliseconds timeout) {
    auto const deadline = steady_clock::now() + timeout;
    auto const remaining = [deadline]() {
        return std::max(milliseconds{0},
                        std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()));
    };

    auto head = read_exactly(chan, 2, remaining());
    if (!head.has_value())
        return failure(head.error());
    std::vector<std::uint8_t> frame = std::move(*head);

    // Is this a 2-byte-length opcode? Only RxLarge (request 0x09)
    // and TxLarge (request 0x11) carry a 2-byte LEN — their
    // responses are 0x19 / 0x11. Error frames always use 1-byte.
    std::uint8_t const opcode = frame[0];
    bool wide_length = false;
    if (opcode != dvi::kErrorOpcode) {
        std::uint8_t const req = opcode & 0xEFU;
        if (req == static_cast<std::uint8_t>(dvi::Opcode::RxLarge) ||
            req == static_cast<std::uint8_t>(dvi::Opcode::TxLarge)) {
            wide_length = true;
        }
    }
    std::size_t declared_len = 0;
    if (wide_length) {
        auto lo = read_exactly(chan, 1, remaining());
        if (!lo.has_value())
            return failure(lo.error());
        frame.push_back((*lo)[0]);
        declared_len =
            (static_cast<std::size_t>(frame[1]) << 8U) | static_cast<std::size_t>(frame[2]);
    } else {
        declared_len = frame[1];
    }
    auto rest = read_exactly(chan, declared_len + 1, remaining());
    if (!rest.has_value())
        return failure(rest.error());
    frame.insert(frame.end(), rest->begin(), rest->end());
    return dvi::decode_frame(frame);
}

// Send a DVI request, read the response frame, return the payload
// (for normal responses) or surface an error (for error frames /
// CRC failures).
[[nodiscard]] Result<std::vector<std::uint8_t>> dvi_exchange(IDeviceChannel &chan, dvi::Opcode op,
                                                             std::span<std::uint8_t const> payload,
                                                             milliseconds timeout) {
    auto enc = dvi::encode_request(op, payload);
    if (!enc.has_value())
        return failure(enc.error());
    if (auto s = chan.write_bytes(*enc); !s.has_value()) {
        return failure(s.error());
    }
    auto resp = read_dvi_frame(chan, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    if (auto const *ef = std::get_if<dvi::ErrorFrame>(&*resp)) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "obdx::dvi_exchange: device returned error 0x%02X "
                      "for request opcode 0x%02X",
                      static_cast<unsigned>(ef->error_code),
                      static_cast<unsigned>(ef->request_opcode));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    if (auto const *rf = std::get_if<dvi::ResponseFrame>(&*resp)) {
        return rf->payload;
    }
    // Unreachable per the DVI decoder's variant shape (Response or
    // Error are the only alternatives). Belt-and-suspenders for the
    // null-deref analyzer.
    return failure(ErrorCode::Unknown, "obdx::dvi_exchange: decoded frame matched no known "
                                       "variant");
}

// Map a LinkConfig to the bytes we'd hand the SetProtocol opcode.
// **NOT FINAL** — the VT v1.06 PDF documents the exact sub-op /
// flag layout; until the developer's adapter arrives we send a
// best-guess shape and mark this helper as needs-verification.
//
// What's settled:
//   - The VX hardware supports HSCAN (CAN ISO-15765), J1850 VPW,
//     GM UART ALDL. NO K-Line. (per docs/13 OBDX rewrite this
//     session.)
//   - The opcode is 0x31 SetProtocol.
//
// What's NOT settled:
//   - Exact payload byte layout (protocol enum + flags + CAN
//     filter IDs). The PDF has it; we don't yet.
//
// Returns a payload byte vector or an InvalidArgument when
// LinkConfig requests a protocol the VX can't do.
[[nodiscard]] Result<std::vector<std::uint8_t>> set_protocol_payload(LinkConfig const &cfg) {
    switch (cfg.kind) {
    case LinkKind::KLine:
        return failure(ErrorCode::InvalidArgument,
                       "obdx::Transport: OBDX VX doesn't support K-Line / ISO9141. "
                       "Only pre-2008 Subarus need K-Line; 2008+ (including VA/VB WRX) "
                       "run CAN-ISO15765 — use LinkKind::CanIso15765 with the engine ECU "
                       "CAN IDs (request 0x7E0, response 0x7E8). If you DO need K-Line "
                       "for an older EJ Subaru, a Tactrix OpenPort is the usual choice. "
                       "(OBDX VX supported protocols: HSCAN, J1850 VPW, GM UART ALDL.)");
    case LinkKind::CanFd:
        return failure(ErrorCode::InvalidArgument, "obdx::Transport: OBDX VX doesn't support "
                                                   "CAN-FD (the STN2120 silicon is classical-"
                                                   "CAN-only).");
    case LinkKind::CanIso15765: {
        // TODO(transport_obdx): final byte layout pending VT
        // v1.06 PDF cross-check on real hardware. Current shape:
        // [protocol = 0x06 (ISO15765-ish)] [flags lo / hi]
        // [baud BE bytes] [request id BE] [response id BE].
        // Real adapter will confirm or correct this.
        std::vector<std::uint8_t> bytes;
        bytes.reserve(12);
        bytes.push_back(0x06U);
        bytes.push_back(0x00U);
        bytes.push_back(0x00U);
        auto const baud = static_cast<std::uint32_t>(cfg.baud);
        bytes.push_back(static_cast<std::uint8_t>(baud >> 24U));
        bytes.push_back(static_cast<std::uint8_t>(baud >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(baud >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(baud));
        bytes.push_back(static_cast<std::uint8_t>(cfg.can_id_request >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(cfg.can_id_request));
        bytes.push_back(static_cast<std::uint8_t>(cfg.can_id_response >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(cfg.can_id_response));
        return bytes;
    }
    }
    return failure(ErrorCode::InvalidArgument, "obdx::Transport: unknown LinkKind");
}

// Sniff an ELM-mode response for a string that suggests we're
// talking to an actual OBDX device. The probe response varies
// across firmware revisions; this is a loose-and-permissive check
// (anything mentioning "OBDX" passes). Tightening lands when we
// have a real adapter's response to pattern-match against.
[[nodiscard]] bool looks_like_obdx(std::string_view probe) noexcept {
    auto const has = [probe](std::string_view needle) {
        return probe.find(needle) != std::string_view::npos;
    };
    return has("OBDX") || has("obdx");
}

constexpr milliseconds kElmProbeTimeout{500};
constexpr milliseconds kDviSwitchTimeout{500};
constexpr milliseconds kSetProtocolTimeout{500};
constexpr milliseconds kCloseTimeout{200};

} // namespace

Transport::Transport(std::unique_ptr<IDeviceChannel> channel) noexcept
    : channel_(std::move(channel)) {}

Transport::~Transport() {
    if (open_) {
        (void)close();
    }
}

st::Status Transport::open(LinkConfig const &cfg) {
    if (open_) {
        return failure(ErrorCode::InvalidArgument,
                       "obdx::Transport::open: already open — close() first");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::open: no byte channel attached");
    }

    // 1. ELM-mode probe. `AT @1` returns the device's identifier
    // string; we verify it mentions OBDX before sending anything
    // that could disturb a non-OBDX device on the same port.
    auto probe = elm_exchange(*channel_, "AT @1", kElmProbeTimeout);
    if (!probe.has_value()) {
        return failure(probe.error());
    }
    if (!looks_like_obdx(*probe)) {
        std::string msg{"obdx::Transport::open: device did not identify as OBDX "
                        "(probe response: '"};
        msg.append(*probe);
        msg.append("'). Wrong adapter on this port?");
        return failure(ErrorCode::TransportUnavailable, std::move(msg));
    }
    firmware_ = *probe;

    // 2. Switch to DVI binary mode. Last ASCII exchange — after
    // this the adapter speaks DVI on the wire.
    auto switch_rsp = elm_exchange(*channel_, "DX DP 1", kDviSwitchTimeout);
    if (!switch_rsp.has_value()) {
        return failure(switch_rsp.error());
    }

    // 3. Configure the bus per LinkConfig via DVI SetProtocol.
    auto sp = set_protocol_payload(cfg);
    if (!sp.has_value()) {
        return failure(sp.error());
    }
    auto setp = dvi_exchange(*channel_, dvi::Opcode::SetProtocol, *sp, kSetProtocolTimeout);
    if (!setp.has_value()) {
        return failure(setp.error());
    }

    open_ = true;
    return ok();
}

st::Status Transport::close() {
    if (!open_) {
        return ok();
    }
    open_ = false; // mark closed up front so a failing reboot
                   // doesn't trap us in a re-close loop
    // Best-effort SoftReboot — returns the adapter to ELM mode
    // for the next consumer. Ignore the result; the channel may
    // already be gone (USB unplugged), which is fine for close().
    (void)dvi_exchange(*channel_, dvi::Opcode::SoftReboot, std::span<std::uint8_t const>{},
                       kCloseTimeout);
    firmware_.clear();
    return ok();
}

Result<Frame> Transport::send_recv(std::span<std::uint8_t const> payload,
                                   std::chrono::milliseconds timeout) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send_recv: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send_recv: no byte channel");
    }

    // Split the host-side budget across the two DVI exchanges: the
    // TX confirmation should land quickly (adapter ACKs the write),
    // so give it ~25% and the remaining 75% to RX. If the caller
    // passes a tiny budget the split may both round to 0; in that
    // case we still attempt each exchange with at least 1ms so a
    // local fake doesn't dead-loop.
    auto const tx_budget = std::max(milliseconds{1}, milliseconds{timeout.count() / 4});
    auto const rx_budget = std::max(milliseconds{1}, timeout - tx_budget);

    // Phase 1: TX the ECU payload onto the bus via DVI TxSmall.
    if (payload.size() > 255) {
        // TODO(transport_obdx): TxLarge (opcode 0x11, 2-byte length)
        // for payloads > 255 B. Not relevant for ITransport's normal
        // request shape (SSM/UDS payloads are well under 100 B) but
        // worth surfacing as a clean error until implemented.
        return failure(ErrorCode::InvalidArgument, "obdx::Transport::send_recv: payload > 255 B "
                                                   "needs DVI TxLarge — not yet wired");
    }
    auto tx = dvi_exchange(*channel_, dvi::Opcode::TxSmall, payload, tx_budget);
    if (!tx.has_value()) {
        return failure(tx.error());
    }

    // Phase 2: pull the ECU's response off the bus via DVI RxSmall.
    // Payload of RxSmall is the read-side timeout argument; for
    // simplicity we let the adapter wait for the full remaining
    // budget. Exact byte format pending PDF verification on real
    // hardware — for the codec-only slice this stays a TODO.
    auto rx =
        dvi_exchange(*channel_, dvi::Opcode::RxSmall, std::span<std::uint8_t const>{}, rx_budget);
    if (!rx.has_value()) {
        return failure(rx.error());
    }
    Frame f;
    f.data = std::move(*rx);
    f.arrived = steady_clock::now();
    return f;
}

st::Status Transport::send(std::span<std::uint8_t const> payload) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable, "obdx::Transport::send: no byte channel");
    }
    if (payload.size() > 255) {
        return failure(ErrorCode::InvalidArgument, "obdx::Transport::send: payload > 255 B "
                                                   "needs DVI TxLarge — not yet wired");
    }
    auto tx = dvi_exchange(*channel_, dvi::Opcode::TxSmall, payload, milliseconds{100});
    if (!tx.has_value())
        return failure(tx.error());
    return ok();
}

st::Status Transport::start_streaming(FrameCallback /*callback*/) {
    return failure(ErrorCode::NotImplemented, "obdx::Transport::start_streaming: streaming lands "
                                              "with the datalogger I/O thread + ring buffer "
                                              "(Phase 3 follow-up).");
}

st::Status Transport::stop_streaming() {
    return failure(ErrorCode::NotImplemented,
                   "obdx::Transport::stop_streaming: see start_streaming");
}

} // namespace st::transport::obdx
