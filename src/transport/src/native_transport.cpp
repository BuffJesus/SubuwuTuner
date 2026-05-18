// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/native_transport.hpp"

#include "st/core/error.hpp"
#include "st/transport/native.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace st::transport::native {

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// Protocol-byte assignments inside the native Connect payload.
// Picked to mirror J2534's ProtocolId values so anyone reading
// both modules sees the parallel; the wire format is otherwise
// our own choice.
inline constexpr std::uint8_t kProtoKLine   = 0x03;   // matches J2534 ISO9141
inline constexpr std::uint8_t kProtoIso15765 = 0x06;  // matches J2534 ISO15765

constexpr milliseconds kHelloTimeout    {500};
constexpr milliseconds kConnectTimeout  {500};
constexpr milliseconds kCloseTimeout    {200};

// Read exactly `n` bytes from the channel, accumulating across
// multiple read_bytes calls until `n` bytes are collected or the
// deadline passes. Mirrors the helper in obdx_transport.cpp;
// kept module-local to avoid leaking a transport-shared private
// API surface for one helper.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_exactly(
    IByteChannel &chan, std::size_t n, milliseconds timeout) {
    std::vector<std::uint8_t> out;
    out.reserve(n);
    auto const deadline = steady_clock::now() + timeout;
    while (out.size() < n) {
        auto const remaining_ms =
            std::chrono::duration_cast<milliseconds>(
                deadline - steady_clock::now());
        if (remaining_ms.count() <= 0) {
            return failure(ErrorCode::TransportTimeout,
                           "native::read_exactly: deadline expired with "
                           + std::to_string(out.size()) + "/"
                           + std::to_string(n) + " bytes accumulated");
        }
        auto chunk = chan.read_bytes(n - out.size(), remaining_ms);
        if (!chunk.has_value()) return failure(chunk.error());
        if (!chunk->empty()) {
            out.insert(out.end(), chunk->begin(), chunk->end());
        }
    }
    return out;
}

// Read one complete native frame off the byte stream. The native
// framing is fixed-shape: SOF + seq + opcode + 2-byte LEN +
// payload + 2-byte CRC. So we can read the 5-byte header to
// learn the payload length, then read the rest, then hand the
// whole buffer to decode_frame for CRC + structural validation.
[[nodiscard]] Result<DecodedFrame> read_native_frame(
    IByteChannel &chan, milliseconds timeout) {
    auto header = read_exactly(chan, 5, timeout);
    if (!header.has_value()) return failure(header.error());
    if ((*header)[0] != kStartOfFrame) {
        char buf[80];
        std::snprintf(buf, sizeof buf,
                      "native::read_native_frame: wrong start-of-frame "
                      "byte (expected 0x%02X, got 0x%02X)",
                      kStartOfFrame, (*header)[0]);
        return failure(ErrorCode::ParseError, std::string{buf});
    }
    std::size_t const declared_len =
        (static_cast<std::size_t>((*header)[3]) << 8U)
        | static_cast<std::size_t>((*header)[4]);
    // Remaining: payload (declared_len) + 2-byte CRC.
    auto const deadline = steady_clock::now() + timeout;
    auto const remaining_ms =
        std::chrono::duration_cast<milliseconds>(
            deadline - steady_clock::now());
    auto rest = read_exactly(chan, declared_len + 2,
                              std::max(milliseconds{0}, remaining_ms));
    if (!rest.has_value()) return failure(rest.error());
    std::vector<std::uint8_t> frame = std::move(*header);
    frame.insert(frame.end(), rest->begin(), rest->end());
    return decode_frame(frame);
}

// Send a native request with the given seq + opcode + payload,
// read the response frame, and return the response payload (for
// Response variant) or surface as Error / unsupported.
[[nodiscard]] Result<std::vector<std::uint8_t>> request_response(
    IByteChannel &chan, std::uint8_t seq, Opcode op,
    std::span<std::uint8_t const> payload, milliseconds timeout) {
    auto enc = encode_request(seq, op, payload);
    if (!enc.has_value()) return failure(enc.error());
    if (auto s = chan.write_bytes(*enc); !s.has_value()) {
        return failure(s.error());
    }
    auto resp = read_native_frame(chan, timeout);
    if (!resp.has_value()) return failure(resp.error());

    if (auto const *ef = std::get_if<ErrorFrame>(&*resp)) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "native::request_response: device returned error "
                      "0x%02X for request seq=0x%02X opcode=0x%02X",
                      static_cast<unsigned>(ef->error_code),
                      static_cast<unsigned>(ef->seq),
                      static_cast<unsigned>(ef->request_op));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    if (auto const *ev = std::get_if<Event>(&*resp)) {
        // Unsolicited streamed event arrived where we expected a
        // response — adapter is misconfigured (we didn't start a
        // stream) or there's a stream packet from a previous
        // session in the buffer. Surface so the caller can resync.
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "native::request_response: unexpected streamed "
                      "event (code 0x%02X) in response slot",
                      static_cast<unsigned>(ev->event_code));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    auto const *rf = std::get_if<Response>(&*resp);
    if (rf == nullptr) {
        return failure(ErrorCode::Unknown,
                       "native::request_response: decoded frame "
                       "matched no known variant");
    }
    if (rf->seq != seq) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "native::request_response: seq mismatch "
                      "(sent 0x%02X, got 0x%02X)",
                      static_cast<unsigned>(seq),
                      static_cast<unsigned>(rf->seq));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    if (rf->request_op != op) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "native::request_response: opcode mismatch "
                      "(sent 0x%02X, got response for 0x%02X)",
                      static_cast<unsigned>(op),
                      static_cast<unsigned>(rf->request_op));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    return rf->payload;
}

} // namespace

std::vector<std::uint8_t> ConnectPayload::encode() const {
    std::vector<std::uint8_t> out(9, 0);
    out[0] = protocol_id;
    out[1] = static_cast<std::uint8_t>(baud >> 24U);
    out[2] = static_cast<std::uint8_t>(baud >> 16U);
    out[3] = static_cast<std::uint8_t>(baud >> 8U);
    out[4] = static_cast<std::uint8_t>(baud);
    out[5] = static_cast<std::uint8_t>(can_id_request >> 8U);
    out[6] = static_cast<std::uint8_t>(can_id_request);
    out[7] = static_cast<std::uint8_t>(can_id_response >> 8U);
    out[8] = static_cast<std::uint8_t>(can_id_response);
    return out;
}

Result<ConnectPayload> connect_payload_for(LinkConfig const &cfg) noexcept {
    ConnectPayload p{};
    switch (cfg.kind) {
        case LinkKind::KLine:
            p.protocol_id     = kProtoKLine;
            break;
        case LinkKind::CanIso15765:
            p.protocol_id     = kProtoIso15765;
            p.can_id_request  = static_cast<std::uint16_t>(cfg.can_id_request);
            p.can_id_response = static_cast<std::uint16_t>(cfg.can_id_response);
            break;
        case LinkKind::CanFd:
            return failure(ErrorCode::InvalidArgument,
                           "native::connect_payload_for: doc-18 hardware "
                           "is classical-CAN-only (no CAN-FD).");
    }
    if (cfg.baud <= 0) {
        return failure(ErrorCode::InvalidArgument,
                       "native::connect_payload_for: LinkConfig.baud "
                       "must be positive");
    }
    p.baud = static_cast<std::uint32_t>(cfg.baud);
    return p;
}

Transport::Transport(std::unique_ptr<IByteChannel> channel) noexcept
    : channel_(std::move(channel)) {}

Transport::~Transport() {
    if (open_) {
        (void) close();
    }
}

st::Status Transport::open(LinkConfig const &cfg) {
    if (open_) {
        return failure(ErrorCode::InvalidArgument,
                       "native::Transport::open: already open — close() first");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "native::Transport::open: no byte channel attached");
    }

    // 1. Validate LinkConfig before any IO. Bad config (CAN-FD,
    // zero baud) fails fast without needing the device to be
    // present — the user gets a clean error even if they haven't
    // plugged the adapter in yet.
    auto cp = connect_payload_for(cfg);
    if (!cp.has_value()) {
        return failure(cp.error());
    }
    auto const connect_payload = cp->encode();

    // 2. Hello probe. Confirms we're talking to our firmware (not
    // some other USB CDC device on the same port) and stashes the
    // device's firmware string for the firmware() inspection path.
    next_seq_ = 0;
    auto hello = request_response(*channel_, next_seq_++,
                                    Opcode::Hello, {}, kHelloTimeout);
    if (!hello.has_value()) {
        next_seq_ = 0;
        return failure(hello.error());
    }
    // Hello response payload format (our wire choice): the entire
    // payload, after stripping any trailing NUL, is the firmware
    // identification string. Trim NULs so a fixed-size firmware-
    // buffer on the device side doesn't bleed into our display.
    firmware_.assign(hello->begin(), hello->end());
    while (!firmware_.empty() && firmware_.back() == '\0') {
        firmware_.pop_back();
    }
    auto connect = request_response(*channel_, next_seq_++,
                                      Opcode::Connect, connect_payload,
                                      kConnectTimeout);
    if (!connect.has_value()) {
        next_seq_ = 0;
        firmware_.clear();
        return failure(connect.error());
    }

    open_ = true;
    return ok();
}

st::Status Transport::close() {
    if (!open_) {
        return ok();
    }
    open_ = false;  // mark closed up front so a failing Disconnect
                    // doesn't trap us in a re-close loop
    // Best-effort Disconnect. Ignore failures — the channel may
    // already be gone (USB unplugged), which is fine for close().
    (void) request_response(*channel_, next_seq_++,
                              Opcode::Disconnect, {}, kCloseTimeout);
    firmware_.clear();
    next_seq_ = 0;
    return ok();
}

Result<Frame> Transport::send_recv(std::span<std::uint8_t const> payload,
                                     std::chrono::milliseconds      timeout) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "native::Transport::send_recv: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "native::Transport::send_recv: no byte channel");
    }

    // Same budget split as obdx::Transport: ~25% to the TX
    // exchange (device just ACKs the write), 75% to the RX
    // exchange (waiting for the ECU's reply). Minimum 1ms per
    // exchange so local fakes never dead-loop.
    auto const tx_budget = std::max(
        milliseconds{1}, milliseconds{timeout.count() / 4});
    auto const rx_budget = std::max(milliseconds{1}, timeout - tx_budget);

    // Phase 1: SendBytes — push the ECU payload onto the bus.
    auto tx = request_response(*channel_, next_seq_++,
                                 Opcode::SendBytes, payload, tx_budget);
    if (!tx.has_value()) return failure(tx.error());

    // Phase 2: RecvBytes — pull the ECU's reply back. Payload of
    // RecvBytes is a timeout argument the device uses for its own
    // bus-side wait; for now we encode the rx_budget as a 2-byte
    // BE millisecond value and let the device honor it.
    std::array<std::uint8_t, 2> const rx_args{
        static_cast<std::uint8_t>(rx_budget.count() >> 8U),
        static_cast<std::uint8_t>(rx_budget.count() & 0xFFU),
    };
    auto rx = request_response(*channel_, next_seq_++,
                                 Opcode::RecvBytes, rx_args, rx_budget);
    if (!rx.has_value()) return failure(rx.error());
    Frame f;
    f.data    = std::move(*rx);
    f.arrived = steady_clock::now();
    return f;
}

st::Status Transport::send(std::span<std::uint8_t const> payload) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "native::Transport::send: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "native::Transport::send: no byte channel");
    }
    auto tx = request_response(*channel_, next_seq_++,
                                 Opcode::SendBytes, payload,
                                 milliseconds{100});
    if (!tx.has_value()) return failure(tx.error());
    return ok();
}

st::Status Transport::start_streaming(FrameCallback /*callback*/) {
    return failure(ErrorCode::NotImplemented,
                   "native::Transport::start_streaming: streaming lands "
                   "with the datalogger I/O thread + ring buffer.");
}

st::Status Transport::stop_streaming() {
    return failure(ErrorCode::NotImplemented,
                   "native::Transport::stop_streaming: see start_streaming");
}

} // namespace st::transport::native
