// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::obdx::dvi — OBDX Pro Direct Vehicle Interface
// (binary) frame codec.
//
// Implements the framing layer of OBDX's native binary protocol per
// the public *OBDX Pro VT Command Set Reference Guide v1.06*. Pure
// utility: no USB, no serial, no I/O. Higher layers (the eventual
// transport::obdx::Transport) wrap this codec with the actual
// device handle + ELM-mode handshake (`AT @1` probe → `DX DP 1`
// switch into DVI).
//
// Frame layout (request):
//
//   +---------+--------+-----------+----------+
//   | CMD     | LEN(s) | DATA…     | CHK      |
//   | 1 byte  | 1 or 2 | LEN bytes | 1 byte   |
//   +---------+--------+-----------+----------+
//
// Frame layout (response, normal):
//
//   +---------+--------+-----------+----------+
//   | RESP_OP | LEN(s) | DATA…     | CHK      |
//   +---------+--------+-----------+----------+
//
// Frame layout (response, error):
//
//   +------+--------+------+--------+----------+
//   | 0x7F | LEN(s) | CMD  | ERR    | CHK      |
//   +------+--------+------+--------+----------+
//
// Length-field width is 1 byte for the "small" RX/TX opcodes
// (0x08/0x10, payload ≤ 255 B) and 2 bytes big-endian for the
// "large" RX/TX opcodes (0x09/0x11, payload up to ~12000 B per the
// spec). All other opcodes use 1-byte length.
//
// Checksum is the sum of every byte preceding the checksum slot,
// then bitwise-NOT, kept as a uint8.
//
// The codec is opcode-agnostic for framing — it doesn't know that
// e.g. opcode 0x22 sub-op 0x02 means "firmware string." That
// dispatch lives in the eventual Transport class. Here we just
// build correct frames and validate incoming ones.

#ifndef ST_TRANSPORT_OBDX_DVI_HPP
#define ST_TRANSPORT_OBDX_DVI_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace st::transport::obdx::dvi {

// Documented opcodes. Subset present here is what we expect to use;
// adding new ones is one enum entry + the right length width.
enum class Opcode : std::uint8_t {
    // Network I/O (split by payload-length-field width).
    RxSmall = 0x08, // RX from network, ≤255 B
    RxLarge = 0x09, // RX from network, 2-byte length
    TxSmall = 0x10, // TX to network, ≤255 B
    TxLarge = 0x11, // TX to network, up to 12000 B per spec
    // Adapter control / introspection.
    ScantoolInfo = 0x22, // sub-ops: HW / FW / model / name / serial / protocols
    Settings = 0x24,     // includes sub-op 0x03 = enable µs RX timestamps
    SoftReboot = 0x25,   // return to ELM mode
    SetProtocol = 0x31,  // OBD protocol + enable + ELM↔DVI switch
    // 0x33 is documented in VT v1.06 §3.11 as "VPW Specific Settings".
    // VPW-only on the VT; the VX shares the opcode for legacy reasons
    // but VPW-specific sub-ops (4x speed, CRC validation) are no-ops
    // on the VX since it can't bus-master VPW. Kept here for sniffing
    // older GM platforms via the VPW pin.
    ProtocolSettings = 0x33,
    // OBDX Pro Developers Reference Manual v3.00 §3.14 "CAN Protocol
    // Settings". Family covers filter setup (sub-op 0x00 Entire Filter),
    // flow-control configuration (0x0A FC frame data, 0x0B FC delay,
    // 0x0E/0x0F padding byte/state), and per-baud-rate selection.
    // Critical for ISO15765 — without an explicit Flow filter, the
    // adapter has no idea which CAN IDs are the ECU's responses and
    // drops every incoming frame.
    CanProtocolSettings = 0x34,
    Adc = 0x3A,          // DLC pin 16 battery voltage
};

// Marker for the error-frame opcode (response slot).
inline constexpr std::uint8_t kErrorOpcode = 0x7F;

// Length-field width selector. The "small" RX/TX opcodes carry a
// 1-byte length; the "large" variants carry 2 big-endian bytes.
// Everything else is 1-byte by convention.
enum class LengthWidth : std::uint8_t {
    OneByte,
    TwoBytes,
};

[[nodiscard]] constexpr LengthWidth length_width_for(Opcode op) noexcept {
    return (op == Opcode::RxLarge || op == Opcode::TxLarge) ? LengthWidth::TwoBytes
                                                            : LengthWidth::OneByte;
}

// Practical payload upper bound per the v1.06 spec for each opcode.
// `RxLarge` / `TxLarge` cap at 12000 B; everything else at 255 B
// (the natural 1-byte-length-field limit).
[[nodiscard]] constexpr std::size_t max_payload_for(Opcode op) noexcept {
    if (op == Opcode::RxLarge || op == Opcode::TxLarge) {
        return 12000;
    }
    return 255;
}

// Sum of all bytes preceding the checksum slot, bitwise NOT, kept
// as uint8. Public so tests + fuzzers can verify independently.
[[nodiscard]] std::uint8_t checksum(std::span<std::uint8_t const> bytes) noexcept;

// Build a complete request frame: CMD + LEN(1 or 2 BE) + payload +
// CHK. Fails if `payload.size()` exceeds the opcode's documented
// upper bound. The returned vector is the byte stream to write to
// the device handle as-is.
[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_request(Opcode op, std::span<std::uint8_t const> payload);

// Decoded normal response: the bytes between LEN and CHK.
struct ResponseFrame {
    std::uint8_t response_opcode{0};
    std::vector<std::uint8_t> payload;
};

// Decoded error frame: per the spec the payload after LEN is
// `<CMD> <ERR>` (the offending command's opcode + a one-byte error
// code). We surface both directly so callers can map them to a
// user-facing message.
struct ErrorFrame {
    std::uint8_t request_opcode{0};
    std::uint8_t error_code{0};
};

using DecodedFrame = std::variant<ResponseFrame, ErrorFrame>;

// Parse one complete frame from `bytes`. Returns ParseError when:
//   - the buffer is shorter than the minimum (CMD + LEN + CHK),
//   - the declared length field overruns the buffer,
//   - the checksum byte doesn't match.
// Length-field width is inferred from the response opcode's bit
// pattern (response of RxLarge/TxLarge → 2-byte length); error
// frames always carry a 1-byte length.
//
// The decoder does NOT consume any trailing bytes past the frame
// it parsed — `bytes` must contain exactly one frame. Stream
// reassembly (joining USB chunks into complete frames) is a higher-
// layer concern.
[[nodiscard]] Result<DecodedFrame> decode_frame(std::span<std::uint8_t const> bytes);

} // namespace st::transport::obdx::dvi

#endif // ST_TRANSPORT_OBDX_DVI_HPP
