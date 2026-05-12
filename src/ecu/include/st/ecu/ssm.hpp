// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_ECU_SSM_HPP
#define ST_ECU_SSM_HPP

#include "st/core/result.hpp"
#include "st/transport.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace st::ecu::ssm {

// Wire-format constants for Subaru Select Monitor (the protocol modern
// Subaru ECUs speak over K-Line as ISO 9141/14230 or encapsulated over
// CAN-TP). The framing below is what's documented in publicly-available
// references (RomRaider source, NASIOC forum threads, vendor docs); none
// of it has been validated against a real Subaru ECU yet — this is the
// design we'll test as soon as the developer's OBDX Pro VX adapter and a
// real car are both available. Discrepancies discovered then will land
// as targeted fixes here.
//
// Request framing (read-by-address):
//
//     +------+------+------+-----+-----+-----+---- N times ----+------+
//     | 0x80 | dst  | src  | LEN | CMD | PAD | addr_hi|md|lo  | CSUM |
//     +------+------+------+-----+-----+-----+----------------+------+
//
//     dst   = 0x10 (engine ECU)
//     src   = 0xF0 (diagnostic tool)
//     LEN   = number of payload bytes (CMD + PAD + 3*N), excluding CSUM
//     CMD   = 0xA8 (read-by-address)
//     PAD   = 0x00
//     CSUM  = 8-bit sum of all preceding bytes, mod 256
//
// Response framing:
//
//     +------+------+------+-----+-----+---- N times ----+------+
//     | 0x80 | 0xF0 | 0x10 | LEN | RSP | data_byte       | CSUM |
//     +------+------+------+-----+-----+-----------------+------+
//
//     RSP   = 0xE8 (positive A8 response = CMD + 0x40)
//     LEN   = 1 (RSP) + N (data bytes)
//
// Negative responses use RSP = 0x7F with a one-byte NRC; SsmClient maps
// these to ErrorCode::EcuRejected with the NRC in the message.

inline constexpr std::uint8_t kHeader            = 0x80;
inline constexpr std::uint8_t kDestEcu           = 0x10;
inline constexpr std::uint8_t kSrcTool           = 0xF0;
inline constexpr std::uint8_t kCmdReadByAddress  = 0xA8;
inline constexpr std::uint8_t kRespReadByAddress = 0xE8;
inline constexpr std::uint8_t kNegativeResponse  = 0x7F;
inline constexpr std::uint8_t kPadByte           = 0x00;

// Maximum addressable address. SSM uses 24-bit memory addresses.
inline constexpr std::uint32_t kMaxAddress = 0x00FFFFFFU;

// Sum bytes mod 256.
[[nodiscard]] std::uint8_t ssm_checksum(std::span<std::uint8_t const> bytes) noexcept;

// Build the wire bytes for a multi-address read-by-address request.
// Returns InvalidArgument if any address exceeds kMaxAddress, or if the
// resulting payload would exceed what the LEN byte can encode.
[[nodiscard]] Result<std::vector<std::uint8_t>> build_a8_request(
    std::span<std::uint32_t const> addresses);

// Parse a response frame from the ECU, expecting `expected_n` data bytes.
// Verifies the header, source/dest, length, response byte, and checksum.
// Returns the N data bytes on success.
[[nodiscard]] Result<std::vector<std::uint8_t>> parse_a8_response(
    std::span<std::uint8_t const> resp, std::size_t expected_n);

// High-level client that wraps the framing and talks to a transport.
class SsmClient {
  public:
    explicit SsmClient(transport::ITransport &t) noexcept : transport_{&t} {}

    // Read one byte per address. The returned vector has size addresses.size().
    [[nodiscard]] Result<std::vector<std::uint8_t>> read(
        std::span<std::uint32_t const> addresses,
        std::chrono::milliseconds      timeout = std::chrono::milliseconds{500});

    // Read N contiguous bytes starting at base_address. Convenience over read().
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_block(
        std::uint32_t base_address, std::size_t length,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

  private:
    transport::ITransport *transport_;
};

} // namespace st::ecu::ssm

#endif // ST_ECU_SSM_HPP
