// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::ap3 — wire-level codec for the AP3 USB byte protocol.
//
// Pure utility, no I/O. Implements the wire format per the spec at
// specs/references/cobb-ap3-usb-protocol.md §4 and the CRC-32 variant
// per §5. Higher layers (st::devices::ap3::Client) sit on top of an
// IByteChannel and drive request/response dispatch using this codec.
//
// Frame layout (both directions):
//
//   +-----+---------+----------+------+------+--------+
//   | SYN | wire_len| reserved | type | body | CRC32  |
//   | 02  | u24 BE  | 0x00     | u8   | …    | u32 BE |
//   | 00  | [2..4]  | [5]      | [6]  | …    |        |
//   +-----+---------+----------+------+------+--------+
//
// wire_len counts body + CRC trailer (i.e. all bytes after the 7-byte
// header). The u24-BE-at-[2..4] encoding is load-bearing: an
// incorrect u16-LE-at-[4..5] read produces identical wire bytes for
// wire_len < 256 and silently corrupts every larger packet. See
// spec §4.1.

#ifndef ST_TRANSPORT_COBB_AP_PACKET_HPP
#define ST_TRANSPORT_COBB_AP_PACKET_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace st::transport::ap3 {

inline constexpr std::uint8_t kSyncByte0 = 0x02;
inline constexpr std::uint8_t kSyncByte1 = 0x00;

// Length-field width. CRC trailer width. Total fixed overhead per
// packet = 7 (header) + 4 (CRC) = 11 bytes; max body size is
// 0xFFFFFF - 4 = 16777211 bytes (u24 wire_len caps the count).
inline constexpr std::size_t kHeaderSize = 7;
inline constexpr std::size_t kCrcSize = 4;
inline constexpr std::size_t kFixedOverhead = kHeaderSize + kCrcSize;
inline constexpr std::size_t kMaxBodySize = 0xFFFFFFU - kCrcSize;

// Response-type codes the device emits in the `type` byte. Carried
// here so the client doesn't need to redefine them.
enum class ResponseType : std::uint8_t {
    AckOrContinuation = 0x00,
    Info = 0x01,
    Error = 0x02,
};

// CRC-32 per spec §5: zlib variant (reflected, init 0xFFFFFFFF, no
// final XOR), computed over the bytes preceding the CRC slot with
// the slot bytes treated as 0x00 0x00 0x00 0x00 (i.e. the CRC's
// input domain extends 4 bytes past the trailer position, all
// zeros).
//
// Exposed publicly so tests + fuzzers can verify independently.
[[nodiscard]] std::uint32_t packet_crc(std::span<std::uint8_t const> packet_without_crc) noexcept;

// Encode a complete packet: header + body + CRC trailer. Returns
// InvalidArgument if body.size() exceeds kMaxBodySize, or
// PolicyDenied if `type` is on the spec §6.0 command-block list
// (reboot / OTA / legacy-upload-to-system / infinite-loop /
// DirectDongleTalk / above-range). Defense-in-depth: the codec
// itself refuses to construct dangerous packets so a buggy caller
// can't accidentally send one.
[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_packet(std::uint8_t type, std::span<std::uint8_t const> body);

// True iff the cmd byte is on the spec §6.0 codec-level block list:
//   0x05 (reboot), 0x06 (OTA firmware), 0x07/0x08 (legacy upload to
//   path_type=0 / system area), 0x18 (infinite-loop handler),
//   0x29 (DirectDongleTalk), and anything > 0x31 (beyond the
//   documented dispatcher range).
// Exposed publicly so the client layer can surface a typed refusal
// without re-implementing the table.
[[nodiscard]] bool is_blocked_command(std::uint8_t type) noexcept;

// Decoded header. `body_size` = wire_len - kCrcSize, i.e. how many
// bytes of body to expect between the header and the CRC trailer.
struct PacketHeader {
    std::uint8_t type{0};
    std::uint32_t wire_len{0}; // includes CRC
    std::uint32_t body_size{0};
};

// Decode the 7-byte header. Validates the sync magic and the
// reserved byte, returns ParseError otherwise. Returns
// UnexpectedEof when fewer than kHeaderSize bytes are available.
[[nodiscard]] Result<PacketHeader> decode_header(std::span<std::uint8_t const> bytes);

// Verify the CRC trailer matches the computed CRC over the rest of
// the packet. Returns BadChecksum on mismatch, ParseError if the
// buffer is too short to even contain a header + CRC.
[[nodiscard]] Status verify_crc(std::span<std::uint8_t const> packet);

// Convenience: decode an already-assembled complete packet
// (header + body + CRC). Validates header and CRC, returns the
// body span (a view into `packet`). Caller owns the underlying
// storage.
[[nodiscard]] Result<std::span<std::uint8_t const>>
decode_packet_body(std::span<std::uint8_t const> packet);

} // namespace st::transport::ap3

#endif // ST_TRANSPORT_COBB_AP_PACKET_HPP
