// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::native — SubuwuTuner's own wired-adapter protocol.
//
// When the doc-18 handheld is plugged into a PC over USB, it acts
// as a wired pass-thru in the same operational role as a Tactrix
// OpenPort or an OBDX Pro VX: PC drives, adapter relays bytes to
// the K-Line / CAN bus, adapter relays bytes back. Unlike J2534
// (vendor DLL) or DVI (OBDX's documented protocol), this framing
// is OURS — we own both ends, so we get to skip legacy cruft and
// pick framing choices that suit a flash channel specifically.
//
// Design choices:
//   - Start-of-frame byte (0xAA) so stream readers can resync after
//     a USB stall or partial-frame discard.
//   - 1-byte sequence number so the host can pipeline requests and
//     match responses to in-flight commands.
//   - 1-byte opcode; response opcode = request | 0x80. Reserved
//     opcode 0x7F = async error frame, 0x6F = streamed event
//     (datalogger push, no seq match).
//   - 2-byte big-endian length field → 64 KiB max payload. Largest
//     single-frame transfer we'd ever do is a ~12 KiB flash block.
//   - 16-bit CRC (CCITT-FALSE: init=0xFFFF, poly=0x1021, no refin/
//     refout/xorout). Catches every 1- and 2-bit error and most
//     bursts up to 16 bits — much stronger than the sum-and-NOT
//     used by DVI. Worth the 2 extra bytes per frame on a flash
//     channel.
//
// Frame layout (request):
//
//   +------+------+--------+----------+-----------+----------+
//   | 0xAA | seq  | opcode | len (BE) | payload   | CRC BE   |
//   | 1 B  | 1 B  | 1 B    | 2 B      | len B     | 2 B      |
//   +------+------+--------+----------+-----------+----------+
//   |<-------- CRC covers these bytes ---------->|
//
// Response: same layout, `opcode | 0x80` in the opcode slot, seq
// echoed from the request.
//
// Error (async, device → PC): opcode = 0x7F, payload = seq (1 B) +
// request_op (1 B) + error_code (1 B).
//
// Event (streamed, device → PC, no request): opcode = 0x6F, seq =
// 0, payload is event-type-specific.
//
// This header is pure codec — no USB, no serial. The eventual
// transport::native::Transport class wraps it with the actual
// device handle.

#ifndef ST_TRANSPORT_NATIVE_HPP
#define ST_TRANSPORT_NATIVE_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace st::transport::native {

// Frame markers.
inline constexpr std::uint8_t kStartOfFrame  = 0xAA;
inline constexpr std::uint8_t kResponseMask  = 0x80;  // OR'd into response opcode
inline constexpr std::uint8_t kErrorOpcode   = 0x7F;  // async error (seq matches request)
inline constexpr std::uint8_t kEventOpcode   = 0x6F;  // streamed (datalogger), no seq

// Maximum payload bytes a single frame may carry. Bounded by the
// 2-byte length field. The actual practical cap is much lower —
// our largest single transfer (a flash block) is ≈ 4-12 KiB.
inline constexpr std::size_t kMaxPayloadBytes = 0xFFFF;

// Per-opcode payload structure is the next layer's concern; the
// codec only knows about framing. Opcodes documented here for
// reference + so the dispatcher has a stable enum to switch on.
//
// **Opcode-space layout** (load-bearing invariant):
//   0x01..0x6E — request opcodes (this enum)
//   0x6F       — kEventOpcode  (streamed device→PC)
//   0x70..0x7E — reserved for future requests
//   0x7F       — kErrorOpcode  (async device→PC error)
//   0x80..0xFF — responses (request opcode OR'd with kResponseMask)
//
// Request opcodes MUST keep bit 7 clear so the response-bit set/clear
// test in decode_frame can distinguish them from response frames.
enum class Opcode : std::uint8_t {
    Hello              = 0x01,  // probe: response carries fw_version + capability bits
    Connect            = 0x02,  // bring up the bus (protocol + baud)
    Disconnect         = 0x03,
    SendBytes          = 0x04,  // PC → bus, app-layer payload
    RecvBytes          = 0x05,  // bus → PC, with a timeout argument
    SetConfig          = 0x06,  // baud, parity, ISO-15765 flow-control params
    StreamStart        = 0x07,  // begin datalog push (filter set in payload)
    StreamStop         = 0x08,
    ReadBatteryVoltage = 0x09,  // DLC pin 16 ADC; sanity check before flash
    Reset              = 0x0F,  // adapter soft-reboot
};

// CRC-16/CCITT-FALSE — init 0xFFFF, polynomial 0x1021, no refin /
// refout / xorout. Public so tests + the eventual transport class
// can compute it independently.
[[nodiscard]] std::uint16_t crc16_ccitt_false(
    std::span<std::uint8_t const> bytes) noexcept;

// Build a complete request frame. Returns InvalidArgument if the
// payload exceeds kMaxPayloadBytes. The returned vector is the
// byte stream to write to the device handle as-is.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_request(
    std::uint8_t                    seq,
    Opcode                          op,
    std::span<std::uint8_t const>   payload);

// Decoded normal response.
struct Response {
    std::uint8_t              seq{0};
    Opcode                    request_op{};   // request that produced this response
    std::vector<std::uint8_t> payload;
};

// Decoded async error frame. Payload shape (after the opcode-
// agnostic framing) is fixed at 3 bytes: seq + request_op + err.
struct ErrorFrame {
    std::uint8_t seq{0};
    Opcode       request_op{};
    std::uint8_t error_code{0};
};

// Decoded streamed event (datalogger push, no request matched).
struct Event {
    std::uint8_t              event_code{0};  // first payload byte categorizes the event
    std::vector<std::uint8_t> payload;        // remainder, event-specific
};

using DecodedFrame = std::variant<Response, ErrorFrame, Event>;

// Parse exactly one frame. Returns ParseError when:
//   - buffer is shorter than the minimum header (SOF + seq + op + 2-byte len + 2-byte CRC = 7 bytes),
//   - the SOF byte is wrong,
//   - the declared length overruns the buffer,
//   - the CRC doesn't match,
//   - an error frame has the wrong payload length (must be exactly 3),
//   - an event frame has an empty payload (must carry at least 1 event_code byte).
//
// Stream reassembly (joining USB-read chunks into complete frames
// + SOF-driven resync) is a higher-layer concern. The decoder
// requires `bytes` to contain exactly one frame from SOF through CRC.
[[nodiscard]] Result<DecodedFrame> decode_frame(
    std::span<std::uint8_t const> bytes);

} // namespace st::transport::native

#endif // ST_TRANSPORT_NATIVE_HPP
